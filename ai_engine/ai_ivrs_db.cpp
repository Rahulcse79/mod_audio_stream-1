/*
 * ai_ivrs.cpp — PostgreSQL ai_ivrs writer for mod_audio_stream.
 *
 * Auto-creates the database "ai_user_data" and the "ai_ivrs" table
 * in the public schema if they don't exist.
 *
 * Guarded by HAVE_LIBPQ — if libpq is not available, all functions
 * become no-ops that return false.
 */

#include "ai_ivrs_db.h"

#ifdef HAVE_LIBPQ
#include <libpq-fe.h>
#endif

#ifdef HAVE_SWITCH_H
#include <switch.h>
#define TDB_LOG_INFO(fmt, ...)  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,    fmt, ##__VA_ARGS__)
#define TDB_LOG_WARN(fmt, ...)  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, fmt, ##__VA_ARGS__)
#define TDB_LOG_ERR(fmt, ...)   switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,   fmt, ##__VA_ARGS__)
#else
#include <cstdio>
#define TDB_LOG_INFO(fmt, ...)  fprintf(stdout, "[TELE-INFO] " fmt, ##__VA_ARGS__)
#define TDB_LOG_WARN(fmt, ...)  fprintf(stderr, "[TELE-WARN] " fmt, ##__VA_ARGS__)
#define TDB_LOG_ERR(fmt, ...)   fprintf(stderr, "[TELE-ERR]  " fmt, ##__VA_ARGS__)
#endif

namespace ai_ivrs {

#ifdef HAVE_LIBPQ

/* ---------- helpers ---------- */

static std::string build_conninfo(const DBConfig& cfg, const std::string& dbname) {
    std::string s;
    s += "host=" + cfg.host;
    s += " port=" + cfg.port;
    s += " user=" + cfg.user;
    if (!cfg.password.empty()) {
        s += " password=" + cfg.password;
    }
    s += " dbname=" + dbname;
    s += " connect_timeout=5";
    return s;
}

/* Escape a string for SQL literal (caller frees result). */
static std::string pg_escape(PGconn* conn, const std::string& raw) {
    char* escaped = PQescapeLiteral(conn, raw.c_str(), raw.size());
    if (!escaped) return "''";
    std::string result(escaped);
    PQfreemem(escaped);
    return result;
}

/* ---------- public API ---------- */

bool ensure_schema(const DBConfig& cfg) {
    /* 1. Connect to the default "postgres" database to check/create target DB */
    {
        std::string ci = build_conninfo(cfg, "postgres");
        PGconn* conn = PQconnectdb(ci.c_str());
        if (PQstatus(conn) != CONNECTION_OK) {
            TDB_LOG_ERR("[ai_ivrs] Cannot connect to postgres DB: %s\n", PQerrorMessage(conn));
            PQfinish(conn);
            return false;
        }

        /* Check if our target database exists */
        std::string check_sql = "SELECT 1 FROM pg_database WHERE datname = "
                                + pg_escape(conn, cfg.dbname);
        PGresult* res = PQexec(conn, check_sql.c_str());
        bool db_exists = (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0);
        PQclear(res);

        if (!db_exists) {
            TDB_LOG_INFO("[ai_ivrs] Database '%s' not found — creating it\n", cfg.dbname.c_str());
            /* CREATE DATABASE cannot run inside a transaction and cannot use params,
             * but the dbname is already validated via pg_escape above. */
            std::string create_sql = "CREATE DATABASE " + cfg.dbname;
            res = PQexec(conn, create_sql.c_str());
            if (PQresultStatus(res) != PGRES_COMMAND_OK) {
                TDB_LOG_ERR("[ai_ivrs] CREATE DATABASE failed: %s\n", PQerrorMessage(conn));
                PQclear(res);
                PQfinish(conn);
                return false;
            }
            PQclear(res);
            TDB_LOG_INFO("[ai_ivrs] Database '%s' created successfully\n", cfg.dbname.c_str());
        }

        PQfinish(conn);
    }

    /* 2. Connect to the target database and create the table if needed */
    {
        std::string ci = build_conninfo(cfg, cfg.dbname);
        PGconn* conn = PQconnectdb(ci.c_str());
        if (PQstatus(conn) != CONNECTION_OK) {
            TDB_LOG_ERR("[ai_ivrs] Cannot connect to '%s': %s\n",
                        cfg.dbname.c_str(), PQerrorMessage(conn));
            PQfinish(conn);
            return false;
        }

        const char* create_table_sql =
            "CREATE TABLE IF NOT EXISTS ai_ivrs ("
            "  id                        SERIAL PRIMARY KEY,"
            "  channel_id                TEXT,"
            "  channel_details           TEXT,"
            "  extension                 INTEGER,"
            "  conversation_wavfile_path TEXT,"
            "  conversation_text         TEXT,"
            "  created_at                INTEGER"
            ")";

        PGresult* res = PQexec(conn, create_table_sql);
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            TDB_LOG_ERR("[ai_ivrs] CREATE TABLE failed: %s\n", PQerrorMessage(conn));
            PQclear(res);
            PQfinish(conn);
            return false;
        }
        PQclear(res);

        /* Migration: rename old columns and fix types to match backend entity.
         * - conversation_wav_file -> conversation_wavfile_path
         * - extension TEXT -> INTEGER
         * - created_at BIGINT -> INTEGER
         * Also drop call_epoch if it exists from prior versions. */
        const char* migrate_sql =
            "DO $$ BEGIN "
            "  IF EXISTS ("
            "    SELECT 1 FROM information_schema.columns "
            "    WHERE table_name='ai_ivrs' AND column_name='call_epoch'"
            "  ) THEN "
            "    ALTER TABLE ai_ivrs DROP COLUMN call_epoch;"
            "  END IF; "
            "  IF EXISTS ("
            "    SELECT 1 FROM information_schema.columns "
            "    WHERE table_name='ai_ivrs' AND column_name='conversation_wav_file'"
            "  ) THEN "
            "    ALTER TABLE ai_ivrs RENAME COLUMN conversation_wav_file TO conversation_wavfile_path;"
            "  END IF; "
            "  IF EXISTS ("
            "    SELECT 1 FROM information_schema.columns "
            "    WHERE table_name='ai_ivrs' AND column_name='extension' "
            "      AND data_type <> 'integer'"
            "  ) THEN "
            "    ALTER TABLE ai_ivrs DROP COLUMN extension;"
            "    ALTER TABLE ai_ivrs ADD COLUMN extension INTEGER;"
            "  END IF; "
            "  IF EXISTS ("
            "    SELECT 1 FROM information_schema.columns "
            "    WHERE table_name='ai_ivrs' AND column_name='created_at' "
            "      AND data_type <> 'integer'"
            "  ) THEN "
            "    ALTER TABLE ai_ivrs DROP COLUMN created_at;"
            "    ALTER TABLE ai_ivrs ADD COLUMN created_at INTEGER;"
            "  END IF; "
            "END $$";
        res = PQexec(conn, migrate_sql);
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            TDB_LOG_WARN("[ai_ivrs] Schema migration warning: %s\n", PQerrorMessage(conn));
        }
        PQclear(res);

        TDB_LOG_INFO("[ai_ivrs] Schema ready: %s.public.ai_ivrs\n", cfg.dbname.c_str());
        PQfinish(conn);
    }

    return true;
}

bool insert_call_record(const DBConfig& cfg, const CallRecord& rec) {
    std::string ci = build_conninfo(cfg, cfg.dbname);
    PGconn* conn = PQconnectdb(ci.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        TDB_LOG_ERR("[ai_ivrs] insert: connect failed: %s\n", PQerrorMessage(conn));
        PQfinish(conn);
        return false;
    }

    /* Use parameterised query to avoid SQL injection */
    const char* sql =
        "INSERT INTO ai_ivrs "
        "(channel_id, channel_details, extension, conversation_wavfile_path, conversation_text, created_at) "
        "VALUES ($1, $2, $3, $4, $5, $6)";

    /* Convert integer fields to string for PQexecParams (text mode) */
    std::string extension_str = std::to_string(rec.extension);
    std::string epoch_str = std::to_string(rec.created_at);

    const char* params[6] = {
        rec.channel_id.c_str(),
        rec.channel_details.c_str(),
        extension_str.c_str(),
        rec.conversation_wavfile_path.c_str(),
        rec.conversation_text.c_str(),
        epoch_str.c_str()
    };

    PGresult* res = PQexecParams(conn, sql, 6, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        TDB_LOG_ERR("[ai_ivrs] INSERT failed: %s\n", PQerrorMessage(conn));
        PQclear(res);
        PQfinish(conn);
        return false;
    }

    PQclear(res);
    PQfinish(conn);

    TDB_LOG_INFO("[ai_ivrs] Saved call record: channel_id=%s ext=%d\n",
                 rec.channel_id.c_str(), rec.extension);
    return true;
}

#else  /* !HAVE_LIBPQ — stubs */

bool ensure_schema(const DBConfig&) {
    TDB_LOG_WARN("[ai_ivrs] libpq not available — ai_ivrs disabled\n");
    return false;
}

bool insert_call_record(const DBConfig&, const CallRecord&) {
    return false;
}

#endif  /* HAVE_LIBPQ */

}  // namespace ai_ivrs
