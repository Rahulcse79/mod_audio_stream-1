#include "mod_audio_stream.h"
#include "audio_streamer_glue.h"

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_audio_stream_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_audio_stream_load);

SWITCH_MODULE_DEFINITION(
    mod_audio_stream,
    mod_audio_stream_load,
    mod_audio_stream_shutdown,
    NULL
);

static void responseHandler(switch_core_session_t* session,
                            const char* eventName,
                            const char* json)
{
    switch_event_t *event;
    switch_channel_t *channel = switch_core_session_get_channel(session);

    switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, eventName);
    switch_channel_event_set_data(channel, event);
    if (json) {
        switch_event_add_body(event, "%s", json);
    }
    switch_event_fire(&event);
}

static switch_bool_t capture_callback(switch_media_bug_t *bug,
                                      void *user_data,
                                      switch_abc_type_t type)
{
    switch_core_session_t *session =
        switch_core_media_bug_get_session(bug);
    private_t *tech_pvt = (private_t *) user_data;

    if (!tech_pvt) return SWITCH_TRUE;

    switch (type) {

    case SWITCH_ABC_TYPE_INIT:
        break;

    case SWITCH_ABC_TYPE_CLOSE:
        {
            switch_atomic_set(&tech_pvt->close_requested, SWITCH_TRUE);

            int channel_closing = 1;

            if (tech_pvt->ai_cfg.ai_mode_enabled) {
                ai_engine_session_cleanup(session, channel_closing);
            } else {
                stream_session_cleanup(session, NULL, channel_closing);
            }
        }
        break;

    case SWITCH_ABC_TYPE_READ:
        if (switch_atomic_read(&tech_pvt->close_requested)) {
            return SWITCH_FALSE;
        }
        if (tech_pvt->ai_cfg.ai_mode_enabled) {
            return ai_engine_feed_frame(bug);
        }
        return stream_frame(bug);

    case SWITCH_ABC_TYPE_WRITE_REPLACE:
        {
            switch_frame_t *frame =
                switch_core_media_bug_get_write_replace_frame(bug);

            if (!frame || !frame->data || frame->datalen == 0 || !tech_pvt) {
                break;
            }

            if (tech_pvt->ai_cfg.ai_mode_enabled) {
                switch_size_t filled = ai_engine_read_audio(
                    tech_pvt,
                    (int16_t *)frame->data,
                    frame->datalen / 2
                );
                if (filled == 0) {
                    memset(frame->data, 0, frame->datalen);
                }
                switch_core_media_bug_set_write_replace_frame(bug, frame);
                break;
            }

            switch_size_t need = frame->datalen;
            switch_size_t avail = 0;
            switch_size_t to_read = 0;
            switch_size_t got = 0;

            switch_mutex_lock(tech_pvt->mutex);

            if (!tech_pvt->inject_buffer) {
                switch_mutex_unlock(tech_pvt->mutex);
                break;
            }

            if (!tech_pvt->inject_scratch || tech_pvt->inject_scratch_len < need) {
                uint8_t *nbuf = (uint8_t *)switch_core_session_alloc(session, need);
                if (!nbuf) {
                    switch_mutex_unlock(tech_pvt->mutex);
                    break;
                }
                tech_pvt->inject_scratch = nbuf;
                tech_pvt->inject_scratch_len = need;
            }

            uint8_t *inj = tech_pvt->inject_scratch;
            memset(inj, 0, need);

            avail = switch_buffer_inuse(tech_pvt->inject_buffer);

            if (tech_pvt->cfg.inject_min_buffer_ms > 0 && tech_pvt->inject_sample_rate > 0) {
                const switch_size_t bytes_per_ms = (switch_size_t)tech_pvt->inject_sample_rate * 2u * (switch_size_t)tech_pvt->channels / 1000u;
                const switch_size_t min_bytes = bytes_per_ms * (switch_size_t)tech_pvt->cfg.inject_min_buffer_ms;
                if (avail < min_bytes) {
                    to_read = 0;
                } else {
                    to_read = (avail > need) ? need : avail;
                }
            } else {
                to_read = (avail > need) ? need : avail;
            }
            if (to_read > 0) {
                got = switch_buffer_read(tech_pvt->inject_buffer, inj, to_read);
            }

            if (got < need) {
                tech_pvt->inject_underruns++;
            }
            tech_pvt->inject_write_calls++;
            tech_pvt->inject_bytes += got;

            const unsigned long inject_inuse_now = tech_pvt->inject_buffer ?
                (unsigned long)switch_buffer_inuse(tech_pvt->inject_buffer) : 0;

            switch_mutex_unlock(tech_pvt->mutex);

            if (got > 0) {
                if (got >= need) {
                    memcpy(frame->data, inj, need);
                } else {
                    memcpy(frame->data, inj, got);
                }
            }

            switch_core_media_bug_set_write_replace_frame(bug, frame);

            {
                const switch_time_t now = switch_micro_time_now();
                if (!tech_pvt->inject_last_report) tech_pvt->inject_last_report = now;

                const switch_time_t log_interval_us =
                    (tech_pvt->cfg.inject_log_every_ms > 0 ? tech_pvt->cfg.inject_log_every_ms : 1000) * 1000LL;

                if ((now - tech_pvt->inject_last_report) > log_interval_us) {
                    switch_mutex_lock(tech_pvt->mutex);
                    const uint64_t snap_calls = tech_pvt->inject_write_calls;
                    const uint64_t snap_bytes = tech_pvt->inject_bytes;
                    const uint64_t snap_under = tech_pvt->inject_underruns;
                    tech_pvt->inject_write_calls = 0;
                    tech_pvt->inject_bytes = 0;
                    tech_pvt->inject_underruns = 0;
                    switch_mutex_unlock(tech_pvt->mutex);

                    const double loss_pct = snap_calls ?
                        (100.0 * (double)snap_under / (double)snap_calls) : 0.0;

                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                                      SWITCH_LOG_INFO,
                                      "(%s) PUSHBACK consume: write_calls=%llu bytes_read=%llu underruns=%llu loss%%=%.1f inject_inuse_now=%lu\n",
                                      switch_core_session_get_uuid(session),
                                      (unsigned long long)snap_calls,
                                      (unsigned long long)snap_bytes,
                                      (unsigned long long)snap_under,
                                      loss_pct,
                                      inject_inuse_now);
                    tech_pvt->inject_last_report = now;
                }
            }
        }
        break;

    default:
        break;
    }

    return SWITCH_TRUE;
}

static int get_channel_var_int(switch_core_session_t *session, const char* name, int def)
{
    switch_channel_t *channel = switch_core_session_get_channel(session);
    const char* val = channel ? switch_channel_get_variable(channel, name) : NULL;
    if (!val || !*val) return def;
    return atoi(val);
}

static switch_status_t start_capture(switch_core_session_t *session,
                                     switch_media_bug_flag_t flags,
                                     char* wsUri,
                                     int sampling,
                                     char* metadata)
{
    switch_channel_t *channel =
        switch_core_session_get_channel(session);

    switch_media_bug_t *bug;
    switch_codec_t *read_codec;
    void *pUserData = NULL;

    int channels = (flags & SMBF_STEREO) ? 2 : 1;

    if (switch_channel_get_private(channel, MY_BUG_NAME)) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                          "start_capture: streaming bug already running on this channel\n");
        return SWITCH_STATUS_FALSE;
    }

    if (switch_channel_pre_answer(channel)
        != SWITCH_STATUS_SUCCESS) {
        return SWITCH_STATUS_FALSE;
    }

    read_codec = switch_core_session_get_read_codec(session);

    if (!read_codec || !read_codec->implementation) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "start_capture: no read codec or implementation\n");
        return SWITCH_STATUS_FALSE;
    }

    if (stream_session_init(
            session,
            responseHandler,
            read_codec->implementation->actual_samples_per_second,
            wsUri,
            sampling,
            channels,
            metadata,
            &pUserData
        ) != SWITCH_STATUS_SUCCESS) {
        return SWITCH_STATUS_FALSE;
    }

    flags |= SMBF_READ_STREAM;
    flags |= SMBF_WRITE_REPLACE;

    if (switch_core_media_bug_add(
            session,
            MY_BUG_NAME,
            NULL,
            capture_callback,
            pUserData,
            0,
            flags,
            &bug
        ) != SWITCH_STATUS_SUCCESS) {
        stream_session_cleanup(session, NULL, 0);
        return SWITCH_STATUS_FALSE;
    }

    switch_channel_set_private(channel, MY_BUG_NAME, bug);
    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t do_stop(switch_core_session_t *session, char* text)
{
    switch_channel_t *channel = switch_core_session_get_channel(session);

    switch_media_bug_t *ai_bug = (switch_media_bug_t *)switch_channel_get_private(channel, MY_BUG_NAME_AI);
    if (ai_bug) {
        return ai_engine_session_cleanup(session, 0);
    }

    switch_media_bug_t *bug = (switch_media_bug_t *)switch_channel_get_private(channel, MY_BUG_NAME);
    if (bug) {
        return stream_session_cleanup(session, text, 0);
    }

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                      "do_stop: no bug found\n");
    return SWITCH_STATUS_FALSE;
}

static switch_status_t do_pauseresume(switch_core_session_t *session, int pause)
{
    return stream_session_pauseresume(session, pause);
}

static switch_status_t send_text(switch_core_session_t *session, char* text)
{
    return stream_session_send_text(session, text);
}

static switch_status_t start_capture_ai(switch_core_session_t *session)
{
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug;
    switch_codec_t *read_codec;
    private_t *tech_pvt = NULL;

    if (switch_channel_get_private(channel, MY_BUG_NAME_AI)) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                          "start_capture_ai: bug already running\n");
        return SWITCH_STATUS_FALSE;
    }

    if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
        return SWITCH_STATUS_FALSE;
    }

    read_codec = switch_core_session_get_read_codec(session);
    if (!read_codec || !read_codec->implementation) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "start_capture_ai: no read codec\n");
        return SWITCH_STATUS_FALSE;
    }

    tech_pvt = (private_t *)switch_core_session_alloc(session, sizeof(private_t));
    memset(tech_pvt, 0, sizeof(private_t));

    switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED,
                      switch_core_session_get_pool(session));

    tech_pvt->channels = 1;
    tech_pvt->ai_cfg.ai_mode_enabled = 1;

    int ai_sampling = read_codec->implementation->actual_samples_per_second;
    void *pUserData = tech_pvt;

    if (ai_engine_session_init(session, responseHandler,
                               ai_sampling, ai_sampling, 1,
                               &pUserData) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "start_capture_ai: ai_engine_session_init failed\n");
        return SWITCH_STATUS_FALSE;
    }

    switch_media_bug_flag_t flags = SMBF_READ_STREAM | SMBF_WRITE_REPLACE;

    if (switch_core_media_bug_add(
            session,
            MY_BUG_NAME_AI,
            NULL,
            capture_callback,
            tech_pvt,
            0,
            flags,
            &bug
        ) != SWITCH_STATUS_SUCCESS) {
        ai_engine_session_cleanup(session, 0);
        return SWITCH_STATUS_FALSE;
    }

    switch_channel_set_private(channel, MY_BUG_NAME_AI, bug);

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                      "(%s) AI voice agent started, rate=%d\n",
                      switch_core_session_get_uuid(session),
                      read_codec->implementation->actual_samples_per_second);

    return SWITCH_STATUS_SUCCESS;
}

#define STREAM_API_SYNTAX \
"<uuid> start <ws-uri> [mono|mixed|stereo] [8000|16000|24000|32000|48000] [metadata] | " \
"<uuid> start_ai | " \
"<uuid> stop [text] | " \
"<uuid> pause | " \
"<uuid> resume | " \
"<uuid> send_text <text>"

SWITCH_STANDARD_API(stream_function)
{
    char *argv[6] = { 0 };
    char *mycmd = NULL;
    int argc = 0;
    switch_status_t status = SWITCH_STATUS_FALSE;

    if (!zstr(cmd) && (mycmd = strdup(cmd))) {
        argc = switch_separate_string(mycmd, ' ', argv, 6);
    }

    if (argc < 2) {
        stream->write_function(stream, "-USAGE: %s\n", STREAM_API_SYNTAX);
        goto done;
    }

    switch_core_session_t *lsession =
        switch_core_session_locate(argv[0]);

    if (!lsession) goto done;

    if (!strcasecmp(argv[1], "stop")) {
        status = do_stop(lsession, argc > 2 ? argv[2] : NULL);
    }
    else if (!strcasecmp(argv[1], "pause")) {
        status = do_pauseresume(lsession, 1);
    }
    else if (!strcasecmp(argv[1], "resume")) {
        status = do_pauseresume(lsession, 0);
    }
    else if (!strcasecmp(argv[1], "send_text")) {
        status = (argc > 2 && argv[2]) ? send_text(lsession, argv[2]) : SWITCH_STATUS_FALSE;
    }
    else if (!strcasecmp(argv[1], "start")) {

    char wsUri[MAX_WS_URI];
    int sampling = 8000;
    switch_media_bug_flag_t flags = SMBF_READ_STREAM;
    int channels = 1;

        if (argc < 3 || !argv[2] || !validate_ws_uri(argv[2], wsUri)) {
            switch_core_session_rwunlock(lsession);
            goto done;
        }

        if (argc > 3 && argv[3]) {
            if (!strcasecmp(argv[3], "mono")) {
                channels = 1;
            } else if (!strcasecmp(argv[3], "mixed")) {
#ifdef SMBF_OPT_MIXED_READ
                flags |= SMBF_OPT_MIXED_READ;
#else
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(lsession), SWITCH_LOG_WARNING,
                                  "(%s) 'mixed' requested but SMBF_OPT_MIXED_READ not available; falling back to mono.\n",
                                  switch_core_session_get_uuid(lsession));
#endif
                channels = 1;
            } else if (!strcasecmp(argv[3], "stereo")) {
                flags |= SMBF_STEREO;
                channels = 2;
            }
        }

        if (argc > 4) {
            sampling = atoi(argv[4]);
            if (sampling != 8000 && sampling != 16000 && sampling != 24000 &&
                sampling != 32000 && sampling != 48000) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(lsession), SWITCH_LOG_WARNING,
                                  "Invalid sampling rate %d, defaulting to 8000\n", sampling);
                sampling = 8000;
            }
        }

        status = start_capture(
            lsession,
            flags,
            wsUri,
            sampling,
            argc > 5 ? argv[5] : NULL
        );
    }
    else if (!strcasecmp(argv[1], "start_ai")) {
        status = start_capture_ai(lsession);
    }

    switch_core_session_rwunlock(lsession);

done:
    if (status == SWITCH_STATUS_SUCCESS) {
        stream->write_function(stream, "+OK\n");
    } else {
        stream->write_function(stream, "-ERR\n");
    }

    switch_safe_free(mycmd);
    return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_APP(audio_stream_ai_app_function)
{
    switch_channel_t *channel = switch_core_session_get_channel(session);
    const char *uuid = switch_core_session_get_uuid(session);

    if (switch_channel_get_private(channel, MY_BUG_NAME_AI)) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                          "audio_stream_ai app: already running on this channel\n");
        return;
    }

    if (start_capture_ai(session) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "audio_stream_ai app: failed to start AI engine\n");
        return;
    }

    /* Start WAV recording if wav_file_path was configured.
     * The full path (dir/uuid.wav) was built by ai_engine_session_init
     * and stored in ai_cfg.recording_path. */
    char recording_path[MAX_METADATA_LEN] = {0};
    {
        switch_media_bug_t *bug = (switch_media_bug_t *)switch_channel_get_private(channel, MY_BUG_NAME_AI);
        if (bug) {
            private_t *tp = (private_t *)switch_core_media_bug_get_user_data(bug);
            if (tp && tp->ai_cfg.recording_path[0]) {
                strncpy(recording_path, tp->ai_cfg.recording_path, MAX_METADATA_LEN - 1);
                switch_status_t rec_status = switch_ivr_record_session(session, recording_path, 0, NULL);
                if (rec_status == SWITCH_STATUS_SUCCESS) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE,
                                      "(%s) WAV recording started: %s\n", uuid, recording_path);
                } else {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                      "(%s) WAV recording FAILED to start: %s "
                                      "(is mod_sndfile loaded?)\n", uuid, recording_path);
                    recording_path[0] = '\0';   /* clear so we don't try to stop later */
                }
            }
        }
    }

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE,
                      "(%s) audio_stream_ai app: AI engine started, playing silence to hold channel\n",
                      uuid);

    /* Main loop: play silence, wake up on actions */
    while (switch_channel_ready(channel)) {
        switch_ivr_play_file(session, NULL, "silence_stream://1000", NULL);

        if (!switch_channel_ready(channel)) break;

        /* Check if an action is pending */
        switch_media_bug_t *bug = (switch_media_bug_t *)switch_channel_get_private(channel, MY_BUG_NAME_AI);
        if (!bug) break;

        private_t *tech_pvt = (private_t *)switch_core_media_bug_get_user_data(bug);
        if (!tech_pvt) break;

        if (!switch_atomic_read(&tech_pvt->action_pending)) continue;

        /* Copy pending action under lock and clear it */
        switch_mutex_lock(tech_pvt->mutex);
        char action[MAX_SESSION_ID];
        char action_data[MAX_METADATA_LEN];
        strncpy(action, tech_pvt->pending_action, MAX_SESSION_ID);
        action[MAX_SESSION_ID - 1] = '\0';
        strncpy(action_data, tech_pvt->pending_action_data, MAX_METADATA_LEN);
        action_data[MAX_METADATA_LEN - 1] = '\0';
        switch_atomic_set(&tech_pvt->action_pending, SWITCH_FALSE);
        tech_pvt->pending_action[0] = '\0';
        tech_pvt->pending_action_data[0] = '\0';

        /* Save the transfer targets locally before we destroy the bug */
        int target_count = tech_pvt->ai_cfg.transfer_target_count;
        char targets[MAX_TRANSFER_TARGETS][MAX_SESSION_ID];
        for (int i = 0; i < target_count && i < MAX_TRANSFER_TARGETS; i++) {
            strncpy(targets[i], tech_pvt->ai_cfg.transfer_targets[i], MAX_SESSION_ID);
            targets[i][MAX_SESSION_ID - 1] = '\0';
        }
        switch_mutex_unlock(tech_pvt->mutex);

        if (!strcmp(action, "bridge")) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                              "(%s) Bridge action: stopping AI engine, trying %d agents\n",
                              uuid, target_count);

            /* 1) Stop the AI engine and remove the media bug.
             *    This disconnects OpenAI/TTS and frees resources.
             *    After this, tech_pvt/bug are invalid — do NOT use them. */
            ai_engine_session_cleanup(session, 0);
            tech_pvt = NULL;
            bug = NULL;

            /* 2) Try each agent target in priority order via bridge */
            int bridged = 0;
            const char *domain = switch_channel_get_variable(channel, "domain_name");
            if (!domain || !*domain) domain = "localhost";

            for (int i = 0; i < target_count; i++) {
                if (!switch_channel_ready(channel)) break;

                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                                  "(%s) Trying agent %d/%d: %s\n",
                                  uuid, i + 1, target_count, targets[i]);

                /* Build bridge dial string: user/1003@domain
                 * call_timeout=30 gives agent 30 seconds to pick up
                 * The bridge app will ring the agent and connect the caller
                 * directly when the agent answers. */
                char dial_string[1024];
                snprintf(dial_string, sizeof(dial_string),
                         "{call_timeout=30,hangup_after_bridge=true}user/%s@%s",
                         targets[i], domain);

                switch_channel_set_variable(channel, "ai_transfer_target", targets[i]);

                /* Set ringback so the caller hears ringing while agent phone rings */
                switch_channel_set_variable(channel, "ringback", "%(2000,4000,440,480)");

                /* Execute the bridge application — this blocks until:
                 *   - Agent answers → caller & agent are connected → blocks until hangup
                 *   - Agent busy/no-answer/reject → returns immediately
                 * On success the caller talked to the agent and one side hung up. */
                switch_core_session_execute_application(session, "bridge", dial_string);

                /* Check if the bridge succeeded by looking at the channel state.
                 * After a successful bridge + hangup, the channel may be done. */
                const char *cause_str = switch_channel_get_variable(channel, "DIALSTATUS");
                if (!cause_str) cause_str = switch_channel_get_variable(channel, "originate_disposition");

                if (cause_str && !strcasecmp(cause_str, "SUCCESS")) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                                      "(%s) Bridge to %s completed — call ended\n",
                                      uuid, targets[i]);
                    bridged = 1;
                    break;
                }

                /* If channel is no longer ready, the bridge worked and someone hung up */
                if (!switch_channel_ready(channel)) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                                      "(%s) Bridge to %s — channel done after bridge\n",
                                      uuid, targets[i]);
                    bridged = 1;
                    break;
                }

                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                                  "(%s) Bridge to %s failed (%s), trying next agent...\n",
                                  uuid, targets[i],
                                  cause_str ? cause_str : "unknown");
            }

            if (bridged) {
                /* Agent call is done (agent or caller hung up). End the app. */
                break;
            }

            /* All agents busy/unavailable — restart AI and let it tell the caller */
            if (switch_channel_ready(channel)) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                                  "(%s) All %d agents unavailable — restarting AI engine\n",
                                  uuid, target_count);

                /* Restart the AI engine so it can talk to the caller again */
                if (start_capture_ai(session) == SWITCH_STATUS_SUCCESS) {
                    /* Continue the while loop — AI will resume and tell the caller
                     * about the failed transfer (via the function_call_output result
                     * that was already sent to OpenAI before we stopped) */
                    continue;
                } else {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                      "(%s) Failed to restart AI engine after transfer failure\n", uuid);
                    break;
                }
            }
            break;
        }
    }

    /* Stop WAV recording first (before AI cleanup so DB gets the right path).
     * Don't require channel_ready — we want to finalize the file even on hangup. */
    if (recording_path[0]) {
        switch_ivr_stop_record_session(session, recording_path);
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE,
                          "(%s) WAV recording stopped: %s\n", uuid, recording_path);
    }

    /* Final cleanup if AI is still running */
    if (switch_channel_get_private(channel, MY_BUG_NAME_AI)) {
        ai_engine_session_cleanup(session, 0);
    }
}

SWITCH_MODULE_LOAD_FUNCTION(mod_audio_stream_load)
{
    switch_api_interface_t *api_interface;
    switch_application_interface_t *app_interface;

    *module_interface =
        switch_loadable_module_create_module_interface(pool, modname);

    switch_event_reserve_subclass(EVENT_JSON);
    switch_event_reserve_subclass(EVENT_CONNECT);
    switch_event_reserve_subclass(EVENT_ERROR);
    switch_event_reserve_subclass(EVENT_DISCONNECT);
    switch_event_reserve_subclass(EVENT_PLAY);
    switch_event_reserve_subclass(EVENT_AI_STATE);
    switch_event_reserve_subclass(EVENT_AI_TRANSCRIPT);
    switch_event_reserve_subclass(EVENT_AI_RESPONSE);
    switch_event_reserve_subclass(EVENT_AI_ACTION);

    SWITCH_ADD_API(
        api_interface,
        "uuid_audio_stream",
        "audio stream",
        stream_function,
        STREAM_API_SYNTAX
    );

    SWITCH_ADD_APP(
        app_interface,
        "audio_stream_ai",
        "AI Voice Agent",
        "Start AI voice agent on this channel (OpenAI Realtime + TTS)",
        audio_stream_ai_app_function,
        "",
        SAF_NONE
    );

    return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_audio_stream_shutdown)
{
    switch_event_free_subclass(EVENT_JSON);
    switch_event_free_subclass(EVENT_CONNECT);
    switch_event_free_subclass(EVENT_DISCONNECT);
    switch_event_free_subclass(EVENT_ERROR);
    switch_event_free_subclass(EVENT_PLAY);
    switch_event_free_subclass(EVENT_AI_STATE);
    switch_event_free_subclass(EVENT_AI_TRANSCRIPT);
    switch_event_free_subclass(EVENT_AI_RESPONSE);
    switch_event_free_subclass(EVENT_AI_ACTION);
    return SWITCH_STATUS_SUCCESS;
}
