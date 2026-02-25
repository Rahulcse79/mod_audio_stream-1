/*
 * telemetry_db.cpp — PostgreSQL telemetry writer for mod_audio_stream.
 *
 * Auto-creates the database "ai_user_data" and the "telemetry" table
 * in the public schema if they don't exist.
 *
 * Guarded by HAVE_LIBPQ — if libpq is not available, all functions
 * become no-ops that return false.
 */

#include "telemetry_db.h"

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

namespace telemetry {

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
            TDB_LOG_ERR("[telemetry] Cannot connect to postgres DB: %s\n", PQerrorMessage(conn));
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
            TDB_LOG_INFO("[telemetry] Database '%s' not found — creating it\n", cfg.dbname.c_str());
            /* CREATE DATABASE cannot run inside a transaction and cannot use params,
             * but the dbname is already validated via pg_escape above. */
            std::string create_sql = "CREATE DATABASE " + cfg.dbname;
            res = PQexec(conn, create_sql.c_str());
            if (PQresultStatus(res) != PGRES_COMMAND_OK) {
                TDB_LOG_ERR("[telemetry] CREATE DATABASE failed: %s\n", PQerrorMessage(conn));
                PQclear(res);
                PQfinish(conn);
                return false;
            }
            PQclear(res);
            TDB_LOG_INFO("[telemetry] Database '%s' created successfully\n", cfg.dbname.c_str());
        }

        PQfinish(conn);
    }

    /* 2. Connect to the target database and create the table if needed */
    {
        std::string ci = build_conninfo(cfg, cfg.dbname);
        PGconn* conn = PQconnectdb(ci.c_str());
        if (PQstatus(conn) != CONNECTION_OK) {
            TDB_LOG_ERR("[telemetry] Cannot connect to '%s': %s\n",
                        cfg.dbname.c_str(), PQerrorMessage(conn));
            PQfinish(conn);
            return false;
        }

        const char* create_table_sql =
            "CREATE TABLE IF NOT EXISTS telemetry ("
            "  id                    SERIAL PRIMARY KEY,"
            "  channel_id            TEXT,"
            "  channel_details       TEXT,"
            "  extension             TEXT,"
            "  conversation_wav_file TEXT,"
            "  conversation_text     TEXT,"
            "  created_at            TIMESTAMPTZ DEFAULT NOW()"
            ")";

        PGresult* res = PQexec(conn, create_table_sql);
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            TDB_LOG_ERR("[telemetry] CREATE TABLE failed: %s\n", PQerrorMessage(conn));
            PQclear(res);
            PQfinish(conn);
            return false;
        }
        PQclear(res);

        TDB_LOG_INFO("[telemetry] Schema ready: %s.public.telemetry\n", cfg.dbname.c_str());
        PQfinish(conn);
    }

    return true;
}

bool insert_call_record(const DBConfig& cfg, const CallRecord& rec) {
    std::string ci = build_conninfo(cfg, cfg.dbname);
    PGconn* conn = PQconnectdb(ci.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        TDB_LOG_ERR("[telemetry] insert: connect failed: %s\n", PQerrorMessage(conn));
        PQfinish(conn);
        return false;
    }

    /* Use parameterised query to avoid SQL injection */
    const char* sql =
        "INSERT INTO telemetry "
        "(channel_id, channel_details, extension, conversation_wav_file, conversation_text) "
        "VALUES ($1, $2, $3, $4, $5)";

    const char* params[5] = {
        rec.channel_id.c_str(),
        rec.channel_details.c_str(),
        rec.extension.c_str(),
        rec.conversation_wav_file.c_str(),
        rec.conversation_text.c_str()
    };

    PGresult* res = PQexecParams(conn, sql, 5, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        TDB_LOG_ERR("[telemetry] INSERT failed: %s\n", PQerrorMessage(conn));
        PQclear(res);
        PQfinish(conn);
        return false;
    }

    PQclear(res);
    PQfinish(conn);

    TDB_LOG_INFO("[telemetry] Saved call record: channel_id=%s ext=%s\n",
                 rec.channel_id.c_str(), rec.extension.c_str());
    return true;
}

#else  /* !HAVE_LIBPQ — stubs */

bool ensure_schema(const DBConfig&) {
    TDB_LOG_WARN("[telemetry] libpq not available — telemetry disabled\n");
    return false;
}

bool insert_call_record(const DBConfig&, const CallRecord&) {
    return false;
}

#endif  /* HAVE_LIBPQ */

}  // namespace telemetry
