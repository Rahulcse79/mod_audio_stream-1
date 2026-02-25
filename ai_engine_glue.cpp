#include "mod_audio_stream.h"
#include "audio_streamer_glue.h"
#include "ai_engine/ai_engine.h"
#include "ai_engine/telemetry_db.h"
#include <memory>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

static inline const char* get_channel_var(switch_core_session_t* session,
                                           const char* name,
                                           const char* def) {
    switch_channel_t* channel = switch_core_session_get_channel(session);
    const char* val = channel ? switch_channel_get_variable(channel, name) : NULL;
    return (val && *val) ? val : def;
}

static inline int get_channel_var_int(switch_core_session_t* session,
                                       const char* name, int def) {
    const char* val = get_channel_var(session, name, NULL);
    if (!val) return def;
    return atoi(val);
}

static inline float get_channel_var_float(switch_core_session_t* session,
                                           const char* name, float def) {
    const char* val = get_channel_var(session, name, NULL);
    if (!val) return def;
    return (float)atof(val);
}

static inline void safe_strncpy(char* dest, const char* src, size_t max) {
    if (!src) { dest[0] = '\0'; return; }
    strncpy(dest, src, max);
    dest[max - 1] = '\0';
}

static void ai_event_handler(switch_core_session_t* session,
                               responseHandler_t responseHandler,
                               const std::string& event_name,
                               const std::string& json) {
    if (!responseHandler || !session) return;

    const char* fs_event = EVENT_AI_STATE;
    if (event_name == "user_transcript") {
        fs_event = EVENT_AI_TRANSCRIPT;
    } else if (event_name == "response_done") {
        fs_event = EVENT_AI_RESPONSE;
    } else if (event_name == "function_call") {
        fs_event = EVENT_AI_ACTION;
    }

    /* Accumulate transcript text for telemetry DB */
    if (event_name == "user_transcript" || event_name == "response_done") {
        switch_channel_t* ch = switch_core_session_get_channel(session);
        switch_media_bug_t* bug = (switch_media_bug_t*)switch_channel_get_private(ch, MY_BUG_NAME_AI);
        if (bug) {
            private_t* pvt = (private_t*)switch_core_media_bug_get_user_data(bug);
            if (pvt && pvt->conversation_text) {
                auto* text = static_cast<std::string*>(pvt->conversation_text);

                /* Extract clean text from JSON payload */
                std::string clean_text;
                if (event_name == "user_transcript") {
                    /* JSON: {"transcript":"Hello"} — extract the transcript value */
                    const char* key = "\"transcript\":\"";
                    size_t pos = json.find(key);
                    if (pos != std::string::npos) {
                        pos += strlen(key);
                        size_t end = json.find("\"", pos);
                        /* Handle escaped quotes in transcript */
                        while (end != std::string::npos && end > 0 && json[end - 1] == '\\') {
                            end = json.find("\"", end + 1);
                        }
                        if (end != std::string::npos) {
                            clean_text = json.substr(pos, end - pos);
                        }
                    }
                    if (clean_text.empty()) clean_text = json; /* fallback */
                } else {
                    /* response_done JSON: {"length":69,"text":"actual AI response"} */
                    const char* key = "\"text\":\"";
                    size_t pos = json.find(key);
                    if (pos != std::string::npos) {
                        pos += strlen(key);
                        /* Find the closing quote, skipping escaped quotes */
                        size_t end = pos;
                        while (end < json.size()) {
                            end = json.find("\"", end);
                            if (end == std::string::npos) break;
                            /* Count preceding backslashes */
                            size_t bs = 0;
                            size_t bp = end;
                            while (bp > pos && json[bp - 1] == '\\') { bs++; bp--; }
                            if (bs % 2 == 0) break; /* even backslashes = real quote */
                            end++;
                        }
                        if (end != std::string::npos && end > pos) {
                            clean_text = json.substr(pos, end - pos);
                        }
                    }
                    if (clean_text.empty()) return; /* skip if no actual text */
                }

                switch_mutex_lock(pvt->mutex);
                if (!text->empty()) *text += "\n";
                if (event_name == "user_transcript") {
                    *text += "[User]: " + clean_text;
                } else {
                    *text += "[AI]: " + clean_text;
                }
                switch_mutex_unlock(pvt->mutex);
            }
        }
    }

    responseHandler(session, fs_event, json.c_str());
}

static std::string build_transfer_tool_json() {
    std::string tool = "{\"type\":\"function\",\"name\":\"transfer_call\","
        "\"description\":\"Transfer the current phone call to a live human agent. "
        "Use this when the caller explicitly asks to speak to an agent, representative, "
        "or human, or when you cannot help them further. "
        "You MUST call this function - do NOT just say you will transfer.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{"
        "\"reason\":{\"type\":\"string\",\"description\":\"Brief reason for transfer\"}"
        "},\"required\":[\"reason\"]}}";
    return tool;
}

static void handle_transfer_action(switch_core_session_t* session,
                                    private_t* tech_pvt,
                                    ai_engine::AIEngine* engine,
                                    const std::string& call_id,
                                    const std::string& arguments) {
    const char* uuid = tech_pvt->sessionId;
    switch_channel_t* channel = switch_core_session_get_channel(session);

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                      "(%s) transfer_call action triggered: args=%s\n",
                      uuid, arguments.c_str());

    if (!tech_pvt->ai_cfg.transfer_extension[0]) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                          "(%s) No transfer extension configured (AI_CALL_TRANSFER_EXTENSION)\n", uuid);
        if (engine) {
            engine->send_action_result(call_id,
                "{\"success\":false,\"error\":\"No transfer extension configured\"}");
        }
        return;
    }

    /* Tell OpenAI that transfer is starting — it will say "connecting you now" */
    if (engine) {
        engine->send_action_result(call_id,
            "{\"success\":true,\"message\":\"Initiating transfer to agent\"}");
    }

    /* Set channel variables for the app function to pick up */
    switch_channel_set_variable(channel, "ai_transfer_reason", arguments.c_str());

    /* Signal the app function to perform the bridge */
    switch_mutex_lock(tech_pvt->mutex);
    snprintf(tech_pvt->pending_action, MAX_SESSION_ID, "bridge");
    tech_pvt->pending_action_data[0] = '\0';
    switch_atomic_set(&tech_pvt->action_pending, SWITCH_TRUE);
    switch_mutex_unlock(tech_pvt->mutex);
}

extern "C" {

switch_status_t ai_engine_session_init(switch_core_session_t *session,
                                        responseHandler_t responseHandler,
                                        uint32_t samples_per_second,
                                        int sampling, int channels,
                                        void **ppUserData) {

    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_memory_pool_t *pool = switch_core_session_get_pool(session);
    const char* uuid = switch_core_session_get_uuid(session);

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                      "(%s) ai_engine_session_init: initializing AI mode\n", uuid);

    auto* tech_pvt = (private_t*)*ppUserData;
    if (!tech_pvt) {
        tech_pvt = (private_t*)switch_core_session_alloc(session, sizeof(private_t));
        if (!tech_pvt) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "(%s) Error allocating memory for AI engine\n", uuid);
            return SWITCH_STATUS_FALSE;
        }
        memset(tech_pvt, 0, sizeof(*tech_pvt));
        switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED, pool);
    }

    strncpy(tech_pvt->sessionId, uuid, MAX_SESSION_ID);
    tech_pvt->sessionId[MAX_SESSION_ID - 1] = '\0';
    tech_pvt->sampling = sampling;
    if (channels > 0) tech_pvt->channels = channels;
    tech_pvt->responseHandler = responseHandler;
    ai_engine_config_t& ai_cfg = tech_pvt->ai_cfg;
    ai_cfg.ai_mode_enabled = 1;

    safe_strncpy(ai_cfg.openai_api_key,
                 get_channel_var(session, "AI_OPENAI_API_KEY", ""),
                 MAX_API_KEY_LEN);
    safe_strncpy(ai_cfg.openai_model,
                 get_channel_var(session, "AI_OPENAI_MODEL", "gpt-4o-realtime-preview"),
                 MAX_MODEL_LEN);
    safe_strncpy(ai_cfg.system_prompt,
                 get_channel_var(session, "AI_SYSTEM_PROMPT",
                     "You are a helpful AI assistant on a phone call. "
                     "Keep responses brief and conversational. "
                     "Respond in the same language the caller uses."),
                 MAX_PROMPT_LEN);

    ai_cfg.vad_threshold = get_channel_var_float(session, "AI_VAD_THRESHOLD", 0.5f);
    ai_cfg.vad_prefix_padding_ms = get_channel_var_int(session, "AI_VAD_PREFIX_PADDING_MS", 300);
    ai_cfg.vad_silence_duration_ms = get_channel_var_int(session, "AI_VAD_SILENCE_DURATION_MS", 500);
    ai_cfg.temperature = get_channel_var_float(session, "AI_TEMPERATURE", 0.8f);
    ai_cfg.max_response_tokens = get_channel_var_int(session, "AI_MAX_RESPONSE_TOKENS", 4096);

    safe_strncpy(ai_cfg.tts_provider,
                 get_channel_var(session, "AI_TTS_PROVIDER", "elevenlabs"),
                 sizeof(ai_cfg.tts_provider));
    safe_strncpy(ai_cfg.elevenlabs_api_key,
                 get_channel_var(session, "AI_ELEVENLABS_API_KEY", ""),
                 MAX_API_KEY_LEN);
    safe_strncpy(ai_cfg.elevenlabs_voice_id,
                 get_channel_var(session, "AI_ELEVENLABS_VOICE_ID", ""),
                 MAX_VOICE_ID_LEN);
    safe_strncpy(ai_cfg.elevenlabs_model_id,
                 get_channel_var(session, "AI_ELEVENLABS_MODEL_ID", "eleven_turbo_v2_5"),
                 MAX_MODEL_LEN);
    ai_cfg.elevenlabs_stability = get_channel_var_float(session, "AI_ELEVENLABS_STABILITY", 0.5f);
    ai_cfg.elevenlabs_similarity_boost = get_channel_var_float(session, "AI_ELEVENLABS_SIMILARITY_BOOST", 0.75f);

    safe_strncpy(ai_cfg.openai_tts_voice,
                 get_channel_var(session, "AI_OPENAI_TTS_VOICE", "alloy"),
                 sizeof(ai_cfg.openai_tts_voice));

    ai_cfg.dsp_enabled = get_channel_var_int(session, "AI_DSP_ENABLED", 1);
    ai_cfg.compressor_threshold_db = get_channel_var_float(session, "AI_COMPRESSOR_THRESHOLD_DB", -18.0f);
    ai_cfg.compressor_makeup_db = get_channel_var_float(session, "AI_COMPRESSOR_MAKEUP_DB", 6.0f);
    ai_cfg.high_shelf_gain_db = get_channel_var_float(session, "AI_HIGH_SHELF_GAIN_DB", 3.0f);
    ai_cfg.lpf_cutoff_hz = get_channel_var_float(session, "AI_LPF_CUTOFF_HZ", 3800.0f);
    ai_cfg.enable_barge_in = get_channel_var_int(session, "AI_ENABLE_BARGE_IN", 1);
    ai_cfg.enable_tts_cache = get_channel_var_int(session, "AI_ENABLE_TTS_CACHE", 1);
    ai_cfg.debug_ai = get_channel_var_int(session, "AI_DEBUG", 0);

    /* Read transfer call config: extension, host, port */
    safe_strncpy(ai_cfg.transfer_extension,
                 get_channel_var(session, "AI_CALL_TRANSFER_EXTENSION", ""),
                 MAX_SESSION_ID);
    safe_strncpy(ai_cfg.transfer_host,
                 get_channel_var(session, "AI_CALL_TRANSFER_HOST", ""),
                 MAX_SESSION_ID);
    safe_strncpy(ai_cfg.transfer_port,
                 get_channel_var(session, "AI_CALL_TRANSFER_PORT", "5060"),
                 sizeof(ai_cfg.transfer_port));
    if (ai_cfg.transfer_extension[0]) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                          "(%s) Transfer config: %s@%s:%s\n", uuid,
                          ai_cfg.transfer_extension, ai_cfg.transfer_host, ai_cfg.transfer_port);
    }

    /* Read PostgreSQL telemetry config */
    ai_cfg.db_enabled = get_channel_var_int(session, "AI_DB_ENABLED", 0);
    safe_strncpy(ai_cfg.db_host,
                 get_channel_var(session, "AI_DB_HOST", "127.0.0.1"),
                 MAX_SESSION_ID);
    safe_strncpy(ai_cfg.db_port,
                 get_channel_var(session, "AI_DB_PORT", "5432"),
                 sizeof(ai_cfg.db_port));
    safe_strncpy(ai_cfg.db_user,
                 get_channel_var(session, "AI_DB_USER", "rahulsingh"),
                 MAX_SESSION_ID);
    safe_strncpy(ai_cfg.db_password,
                 get_channel_var(session, "AI_DB_PASSWORD", ""),
                 MAX_SESSION_ID);
    safe_strncpy(ai_cfg.db_name,
                 get_channel_var(session, "AI_DB_NAME", "ai_user_data"),
                 MAX_SESSION_ID);
    /* Build recording file path: {wav_file_path}/{uuid}.wav
     * If wav_file_path channel var is set, use it as the directory.
     * Otherwise recording_path stays empty and no recording is done. */
    {
        const char *wav_dir = get_channel_var(session, "wav_file_path", "");
        if (wav_dir[0]) {
            const char *uuid = switch_core_session_get_uuid(session);
            char rec_path[MAX_METADATA_LEN];
            snprintf(rec_path, sizeof(rec_path), "%s/%s.wav", wav_dir, uuid);
            safe_strncpy(ai_cfg.recording_path, rec_path, MAX_METADATA_LEN);
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                              "(%s) WAV recording path: %s\n", uuid, ai_cfg.recording_path);
        } else {
            ai_cfg.recording_path[0] = '\0';
        }
    }

    /* Allocate the conversation text buffer */
    tech_pvt->conversation_text = new std::string();
    switch_atomic_set(&tech_pvt->db_saved, SWITCH_FALSE);

    /* Initialize telemetry schema (auto-create DB + table if missing) */
    if (ai_cfg.db_enabled) {
        telemetry::DBConfig dbcfg;
        dbcfg.host     = ai_cfg.db_host;
        dbcfg.port     = ai_cfg.db_port;
        dbcfg.user     = ai_cfg.db_user;
        dbcfg.password = ai_cfg.db_password;
        dbcfg.dbname   = ai_cfg.db_name;
        if (telemetry::ensure_schema(dbcfg)) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                              "(%s) Telemetry DB ready: %s@%s:%s/%s\n",
                              uuid, ai_cfg.db_user, ai_cfg.db_host,
                              ai_cfg.db_port, ai_cfg.db_name);
        } else {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                              "(%s) Telemetry DB init failed — call data will not be saved\n", uuid);
        }
    }

    if (strlen(ai_cfg.openai_api_key) == 0) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "(%s) AI_OPENAI_API_KEY not set — cannot start AI engine\n", uuid);
        goto init_error;
    }
    if (strcmp(ai_cfg.tts_provider, "elevenlabs") == 0 && strlen(ai_cfg.elevenlabs_api_key) == 0) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "(%s) AI_ELEVENLABS_API_KEY not set — cannot start ElevenLabs TTS\n", uuid);
        goto init_error;
    }

    tech_pvt->inject_buffer = NULL;
    tech_pvt->inject_sample_rate = sampling;
    tech_pvt->inject_bytes_per_sample = 2;

    tech_pvt->read_scratch_len = 0;
    tech_pvt->read_scratch = NULL;
    tech_pvt->inject_scratch_len = 0;
    tech_pvt->inject_scratch = NULL;

    if ((int)samples_per_second != sampling) {
        int err = 0;
        tech_pvt->resampler = speex_resampler_init(channels, samples_per_second, sampling, 7, &err);
        if (err != 0 || !tech_pvt->resampler) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "(%s) Error creating resampler for AI mode\n", uuid);
            return SWITCH_STATUS_FALSE;
        }
    }

    {
        ai_engine::AIEngineConfig engine_cfg;

        engine_cfg.openai.api_key = ai_cfg.openai_api_key;
        engine_cfg.openai.model = ai_cfg.openai_model;
        engine_cfg.openai.system_prompt = ai_cfg.system_prompt;
        engine_cfg.openai.vad_threshold = ai_cfg.vad_threshold;
        engine_cfg.openai.vad_prefix_padding_ms = ai_cfg.vad_prefix_padding_ms;
        engine_cfg.openai.vad_silence_duration_ms = ai_cfg.vad_silence_duration_ms;
        engine_cfg.openai.temperature = ai_cfg.temperature;
        engine_cfg.openai.max_response_output_tokens = ai_cfg.max_response_tokens;
        engine_cfg.openai.input_sample_rate = 24000;
        engine_cfg.tts.provider = ai_cfg.tts_provider;
        engine_cfg.tts.elevenlabs_api_key = ai_cfg.elevenlabs_api_key;
        engine_cfg.tts.elevenlabs_voice_id = ai_cfg.elevenlabs_voice_id;
        engine_cfg.tts.elevenlabs_model_id = ai_cfg.elevenlabs_model_id;
        engine_cfg.tts.elevenlabs_stability = ai_cfg.elevenlabs_stability;
        engine_cfg.tts.elevenlabs_similarity_boost = ai_cfg.elevenlabs_similarity_boost;
        engine_cfg.tts.elevenlabs_output_sample_rate = sampling;
        engine_cfg.tts.openai_api_key = ai_cfg.openai_api_key;
        engine_cfg.tts.openai_voice = ai_cfg.openai_tts_voice;
        engine_cfg.tts.enable_cache = ai_cfg.enable_tts_cache;
        engine_cfg.dsp.sample_rate = sampling;
        engine_cfg.dsp.compressor_enabled = ai_cfg.dsp_enabled;
        engine_cfg.dsp.high_shelf_enabled = ai_cfg.dsp_enabled;
        engine_cfg.dsp.lpf_enabled = ai_cfg.dsp_enabled;
        engine_cfg.dsp.soft_clipper_enabled = ai_cfg.dsp_enabled;
        engine_cfg.dsp.dc_blocker_enabled = ai_cfg.dsp_enabled;
        engine_cfg.dsp.noise_gate_enabled = ai_cfg.dsp_enabled;
        engine_cfg.dsp.compressor_threshold_db = ai_cfg.compressor_threshold_db;
        engine_cfg.dsp.compressor_makeup_db = ai_cfg.compressor_makeup_db;
        engine_cfg.dsp.high_shelf_gain_db = ai_cfg.high_shelf_gain_db;
        engine_cfg.dsp.lpf_cutoff_hz = ai_cfg.lpf_cutoff_hz;
        engine_cfg.freeswitch_sample_rate = sampling;
        engine_cfg.openai_send_rate = 24000;
        engine_cfg.enable_barge_in = ai_cfg.enable_barge_in;
        engine_cfg.debug_logging = ai_cfg.debug_ai;
        engine_cfg.session_uuid = uuid;
        engine_cfg.inject_buffer_ms = 5000;

        /* Register transfer_call tool with OpenAI if transfer is configured */
        if (ai_cfg.transfer_extension[0]) {
            engine_cfg.transfer_extension = ai_cfg.transfer_extension;
            engine_cfg.transfer_host = ai_cfg.transfer_host;
            engine_cfg.transfer_port = ai_cfg.transfer_port;
            engine_cfg.openai.tools.push_back(
                build_transfer_tool_json()
            );
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                              "(%s) Registered transfer_call tool: %s@%s:%s\n",
                              uuid, ai_cfg.transfer_extension,
                              ai_cfg.transfer_host, ai_cfg.transfer_port);
        }

        auto* engine = new ai_engine::AIEngine();

        std::string session_uuid_str(uuid);
        engine->set_event_callback(
            [session_uuid_str, responseHandler](const std::string& event_name, const std::string& json) {
                switch_core_session_t* psession = switch_core_session_locate(
                    session_uuid_str.c_str()
                );
                if (psession) {
                    ai_event_handler(psession, responseHandler, event_name, json);
                    switch_core_session_rwunlock(psession);
                }
            }
        );

        /* Set action callback for OpenAI function calls (e.g. transfer_call) */
        engine->set_action_callback(
            [session_uuid_str, engine](const std::string& function_name,
                               const std::string& arguments,
                               const std::string& call_id) {
                switch_core_session_t* psession = switch_core_session_locate(
                    session_uuid_str.c_str()
                );
                if (!psession) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                                      "Action callback: session %s not found\n",
                                      session_uuid_str.c_str());
                    return;
                }

                auto* bug = (switch_media_bug_t*)switch_channel_get_private(
                    switch_core_session_get_channel(psession), MY_BUG_NAME_AI);
                if (!bug) {
                    switch_core_session_rwunlock(psession);
                    return;
                }
                auto* pvt = (private_t*)switch_core_media_bug_get_user_data(bug);
                if (!pvt) {
                    switch_core_session_rwunlock(psession);
                    return;
                }

                if (function_name == "transfer_call") {
                    handle_transfer_action(psession, pvt, engine, call_id, arguments);
                } else {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_WARNING,
                                      "(%s) Unknown function call: %s\n",
                                      pvt->sessionId, function_name.c_str());
                    engine->send_action_result(call_id,
                        "{\"success\":false,\"error\":\"Unknown function\"}");
                }

                switch_core_session_rwunlock(psession);
            }
        );

        if (!engine->start(engine_cfg, session)) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "(%s) Failed to start AI engine\n", uuid);
            delete engine;
            goto init_error;
        }

        tech_pvt->pAIEngine = engine;
    }

    /* Reset cleanup_started for re-init after transfer failure */
    switch_atomic_set(&tech_pvt->cleanup_started, SWITCH_FALSE);
    switch_atomic_set(&tech_pvt->close_requested, SWITCH_FALSE);

    *ppUserData = tech_pvt;

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                      "(%s) AI engine initialized: model=%s tts=%s voice=%s barge_in=%d\n",
                      uuid, ai_cfg.openai_model, ai_cfg.tts_provider,
                      ai_cfg.elevenlabs_voice_id, ai_cfg.enable_barge_in);

    return SWITCH_STATUS_SUCCESS;

init_error:
    /* Clean up conversation_text on early failure */
    if (tech_pvt->conversation_text) {
        delete static_cast<std::string*>(tech_pvt->conversation_text);
        tech_pvt->conversation_text = nullptr;
    }
    return SWITCH_STATUS_FALSE;
}

switch_bool_t ai_engine_feed_frame(switch_media_bug_t *bug) {
    auto *tech_pvt = (private_t*)switch_core_media_bug_get_user_data(bug);
    if (!tech_pvt) return SWITCH_TRUE;
    if (switch_atomic_read(&tech_pvt->audio_paused) || switch_atomic_read(&tech_pvt->cleanup_started)) return SWITCH_TRUE;

    auto* engine = static_cast<ai_engine::AIEngine*>(tech_pvt->pAIEngine);
    if (!engine || !engine->is_running()) return SWITCH_TRUE;

    uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
    switch_frame_t frame = {};
    frame.data = data;
    frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

    while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
        if (!frame.datalen) continue;

        const int16_t* samples = (const int16_t*)frame.data;
        size_t num_samples = frame.datalen / sizeof(int16_t);

        engine->feed_audio(samples, num_samples);
    }

    return SWITCH_TRUE;
}

switch_size_t ai_engine_read_audio(private_t *tech_pvt, int16_t* dest, size_t num_samples) {
    if (!tech_pvt || !dest || num_samples == 0) return 0;

    auto* engine = static_cast<ai_engine::AIEngine*>(tech_pvt->pAIEngine);
    if (!engine || !engine->is_running()) return 0;

    return engine->read_audio(dest, num_samples);
}

switch_status_t ai_engine_session_cleanup(switch_core_session_t *session,
                                           int channelIsClosing) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    auto *bug = (switch_media_bug_t*)switch_channel_get_private(channel, MY_BUG_NAME_AI);

    if (!bug) {
        /* This is expected when called from the CLOSE callback after the
         * bridge-action cleanup has already cleared the private data. */
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                          "ai_engine_session_cleanup: no bug found (already cleaned up)\n");
        return SWITCH_STATUS_FALSE;
    }

    auto* tech_pvt = (private_t*)switch_core_media_bug_get_user_data(bug);
    if (!tech_pvt) return SWITCH_STATUS_FALSE;

    switch_mutex_lock(tech_pvt->mutex);

    if (switch_atomic_read(&tech_pvt->cleanup_started)) {
        switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_STATUS_SUCCESS;
    }
    switch_atomic_set(&tech_pvt->cleanup_started, SWITCH_TRUE);
    switch_atomic_set(&tech_pvt->close_requested, SWITCH_TRUE);

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                      "(%s) ai_engine_session_cleanup\n", tech_pvt->sessionId);

    switch_channel_set_private(channel, MY_BUG_NAME_AI, nullptr);

    auto* engine = static_cast<ai_engine::AIEngine*>(tech_pvt->pAIEngine);
    tech_pvt->pAIEngine = nullptr;

    switch_mutex_unlock(tech_pvt->mutex);

    if (!channelIsClosing) {
        switch_core_media_bug_remove(session, &bug);
    }

    if (engine) {
        engine->stop();
        delete engine;
    }

    if (tech_pvt->resampler) {
        speex_resampler_destroy(tech_pvt->resampler);
        tech_pvt->resampler = nullptr;
    }

    /* Save telemetry to PostgreSQL if enabled and not already saved */
    if (tech_pvt->ai_cfg.db_enabled && !switch_atomic_read(&tech_pvt->db_saved)) {
        switch_atomic_set(&tech_pvt->db_saved, SWITCH_TRUE);

        telemetry::DBConfig dbcfg;
        dbcfg.host     = tech_pvt->ai_cfg.db_host;
        dbcfg.port     = tech_pvt->ai_cfg.db_port;
        dbcfg.user     = tech_pvt->ai_cfg.db_user;
        dbcfg.password = tech_pvt->ai_cfg.db_password;
        dbcfg.dbname   = tech_pvt->ai_cfg.db_name;

        telemetry::CallRecord rec;
        rec.channel_id = tech_pvt->sessionId;

        /* Build channel_details from channel variables */
        switch_channel_t* ch = switch_core_session_get_channel(session);
        if (ch) {
            std::string details;
            const char* vars[] = {
                "uuid", "direction", "created", "created_epoch", "channel_name",
                "state", "caller_id_name", "caller_id_number", "network_addr",
                "destination_number", "current_application", "current_application_data",
                "dialplan", "context", "read_codec", "read_rate", "write_codec",
                "write_rate", "secure", "hostname", "presence_id",
                "accountcode", "callstate", "callee_id_name", "callee_id_number",
                "call_uuid", "initial_caller_id_name", "initial_caller_id_number",
                "initial_network_addr", "initial_dest", "initial_dialplan", "initial_context",
                NULL
            };
            for (int i = 0; vars[i]; i++) {
                const char* val = switch_channel_get_variable(ch, vars[i]);
                if (i > 0) details += ",";
                details += (val ? val : "");
            }
            rec.channel_details = details;

            const char* caller = switch_channel_get_variable(ch, "caller_id_number");
            rec.extension = caller ? caller : "";
        }

        /* Recording WAV path — only set if recording actually started successfully */
        if (tech_pvt->ai_cfg.recording_path[0] &&
            switch_atomic_read(&tech_pvt->recording_started)) {
            rec.conversation_wav_file = tech_pvt->ai_cfg.recording_path;
        } else {
            rec.conversation_wav_file = "";
        }

        /* Conversation text */
        if (tech_pvt->conversation_text) {
            rec.conversation_text = *static_cast<std::string*>(tech_pvt->conversation_text);
        }

        /* Always save current Unix epoch as created_at */
        rec.created_at = (int64_t)time(NULL);

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                          "(%s) Saving telemetry to DB: ext=%s created_at=%lld text_len=%zu\n",
                          tech_pvt->sessionId, rec.extension.c_str(),
                          (long long)rec.created_at,
                          rec.conversation_text.size());

        telemetry::insert_call_record(dbcfg, rec);
    }

    /* Free conversation text buffer */
    if (tech_pvt->conversation_text) {
        delete static_cast<std::string*>(tech_pvt->conversation_text);
        tech_pvt->conversation_text = nullptr;
    }

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                      "(%s) AI engine cleanup complete\n", tech_pvt->sessionId);

    return SWITCH_STATUS_SUCCESS;
}

}