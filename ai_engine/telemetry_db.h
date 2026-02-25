#ifndef TELEMETRY_DB_H
#define TELEMETRY_DB_H

/*
 * telemetry_db.h — PostgreSQL telemetry writer for mod_audio_stream.
 *
 * Database: ai_user_data (auto-created if missing)
 * Schema:   public
 * Table:    telemetry
 *
 * Columns:
 *   id                    SERIAL PRIMARY KEY
 *   channel_id            TEXT       — FreeSWITCH channel UUID
 *   channel_details       TEXT       — full CSV row from "show channels"
 *   extension             TEXT       — caller extension (e.g. "1002")
 *   conversation_wav_file TEXT       — path to call recording WAV
 *   conversation_text     TEXT       — accumulated user + AI transcript
 *   created_at            TIMESTAMPTZ DEFAULT NOW()
 */

#include <string>

namespace telemetry {

struct DBConfig {
    std::string host      = "127.0.0.1";
    std::string port      = "5432";
    std::string user      = "rahulsingh";
    std::string password;                       // empty = peer/trust auth
    std::string dbname    = "ai_user_data";
};

struct CallRecord {
    std::string channel_id;
    std::string channel_details;
    std::string extension;
    std::string conversation_wav_file;
    std::string conversation_text;
};

/*
 * Ensure the target database and table exist.
 * Connects to "postgres" first to CREATE DATABASE IF NOT EXISTS,
 * then connects to the target DB to CREATE TABLE IF NOT EXISTS.
 * Returns true on success.
 */
bool ensure_schema(const DBConfig& cfg);

/*
 * Insert a call record into the telemetry table.
 * Returns true on success, logs errors internally.
 */
bool insert_call_record(const DBConfig& cfg, const CallRecord& rec);

}  // namespace telemetry

#endif  // TELEMETRY_DB_H
