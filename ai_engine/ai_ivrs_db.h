#ifndef ai_ivrs_H
#define ai_ivrs_H

/*
 * ai_ivrs.h — PostgreSQL ai_ivrs writer for mod_audio_stream.
 *
 * Database: ai_user_data (auto-created if missing)
 * Schema:   public
 * Table:    ai_ivrs
 *
 * Columns:
 *   id                          SERIAL PRIMARY KEY
 *   channel_id                  TEXT       — FreeSWITCH channel UUID
 *   channel_details             TEXT       — full CSV row from "show channels"
 *   extension                   INTEGER    — caller extension (e.g. 1002)
 *   conversation_wavfile_path   TEXT       — path to call recording WAV
 *   conversation_text           TEXT       — accumulated user + AI transcript
 *   created_at                  INTEGER    — Unix epoch seconds at time of save
 */

#include <string>
#include <cstdint>

namespace ai_ivrs {

struct DBConfig {
    std::string host      = "127.0.0.1";
    std::string port      = "5432";
    std::string user      = "postgres";
    std::string password;                       // empty = peer/trust auth
    std::string dbname    = "coralapps";
};

struct CallRecord {
    std::string channel_id;
    std::string channel_details;
    int         extension = 0;
    std::string conversation_wavfile_path;
    std::string conversation_text;
    int         created_at = 0;   // Unix epoch seconds — always set before insert
};

/*
 * Ensure the target database and table exist.
 * Connects to "postgres" first to CREATE DATABASE IF NOT EXISTS,
 * then connects to the target DB to CREATE TABLE IF NOT EXISTS.
 * Returns true on success.
 */
bool ensure_schema(const DBConfig& cfg);

/*
 * Insert a call record into the ai_ivrs table.
 * Returns true on success, logs errors internally.
 */
bool insert_call_record(const DBConfig& cfg, const CallRecord& rec);

}  // namespace ai_ivrs

#endif  // ai_ivrs_H
