/**
 * ═══════════════════════════════════════════════════════════════════════════
 * mod_audio_stream.c
 *
 * FreeSWITCH module: External WebSocket Audio Injector
 *
 * This module provides ONE API command:
 *
 *   uuid_ext_audio_inject <uuid> start <ws-uri>
 *   uuid_ext_audio_inject <uuid> stop
 *   uuid_ext_audio_inject <uuid> status
 *
 * HOW IT WORKS:
 *   1. You dial into FreeSWITCH (e.g. call extension 1234)
 *   2. From fs_cli or ESL, run:
 *        uuid_ext_audio_inject <UUID> start ws://yourserver:8777
 *   3. The module connects to the WebSocket server
 *   4. When the server sends { type:"audio_chunk", callId:"<UUID>",
 *      sampleRate:8000, audioData:"<base64 PCM-16-LE>" }
 *      → the caller hears that audio
 *   5. If the WebSocket disconnects, it retries every 10 seconds
 *   6. Logs status every 30 seconds in FreeSWITCH console
 *
 * BUILD:
 *   mkdir build && cd build
 *   cmake .. && make
 *   sudo cp mod_audio_stream.so /usr/local/freeswitch/mod/
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include "mod_audio_stream.h"
#include "external_audio_inject.h"

/* ── Module lifecycle declarations ── */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_audio_stream_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_audio_stream_load);

SWITCH_MODULE_DEFINITION(
    mod_audio_stream,
    mod_audio_stream_load,
    mod_audio_stream_shutdown,
    NULL
);

/* ── API syntax ── */
#define EXT_INJECT_API_SYNTAX \
    "<uuid> start <ws-uri> | <uuid> stop | <uuid> status"

/**
 * uuid_ext_audio_inject API handler
 *
 * Commands:
 *   uuid_ext_audio_inject <uuid> start ws://server:8777
 *   uuid_ext_audio_inject <uuid> stop
 *   uuid_ext_audio_inject <uuid> status
 */
SWITCH_STANDARD_API(ext_audio_inject_function)
{
    char *argv[4] = { 0 };
    char *mycmd = NULL;
    int argc = 0;
    switch_status_t status = SWITCH_STATUS_FALSE;

    if (!zstr(cmd) && (mycmd = strdup(cmd))) {
        argc = switch_separate_string(mycmd, ' ', argv, 4);
    }

    if (argc < 2) {
        stream->write_function(stream, "-USAGE: %s\n", EXT_INJECT_API_SYNTAX);
        goto done;
    }

    /* ── START ── */
    if (!strcasecmp(argv[1], "start")) {
        if (argc < 3 || !argv[2]) {
            stream->write_function(stream,
                "-USAGE: uuid_ext_audio_inject <uuid> start <ws-uri>\n");
            goto done;
        }

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                          "[mod_audio_stream] API: start uuid=%s ws_uri=%s\n",
                          argv[0], argv[2]);

        status = external_audio_inject_start(argv[0], argv[2]);
    }
    /* ── STOP ── */
    else if (!strcasecmp(argv[1], "stop")) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                          "[mod_audio_stream] API: stop uuid=%s\n", argv[0]);

        status = external_audio_inject_stop(argv[0]);
    }
    /* ── STATUS ── */
    else if (!strcasecmp(argv[1], "status")) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                          "[mod_audio_stream] API: status uuid=%s\n", argv[0]);

        status = external_audio_inject_status(argv[0], stream);
        /* status writes its own output, skip +OK/-ERR */
        switch_safe_free(mycmd);
        return SWITCH_STATUS_SUCCESS;
    }
    else {
        stream->write_function(stream, "-USAGE: %s\n", EXT_INJECT_API_SYNTAX);
        goto done;
    }

done:
    if (status == SWITCH_STATUS_SUCCESS) {
        stream->write_function(stream, "+OK\n");
    } else {
        stream->write_function(stream, "-ERR\n");
    }

    switch_safe_free(mycmd);
    return SWITCH_STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Module load                                                              */
/* ═══════════════════════════════════════════════════════════════════════════ */

SWITCH_MODULE_LOAD_FUNCTION(mod_audio_stream_load)
{
    switch_api_interface_t *api_interface;

    *module_interface =
        switch_loadable_module_create_module_interface(pool, modname);

    /* Register custom events */
    switch_event_reserve_subclass(EVENT_JSON);
    switch_event_reserve_subclass(EVENT_CONNECT);
    switch_event_reserve_subclass(EVENT_ERROR);
    switch_event_reserve_subclass(EVENT_DISCONNECT);
    switch_event_reserve_subclass(EVENT_PLAY);

    /* Register the API command */
    SWITCH_ADD_API(
        api_interface,
        "uuid_ext_audio_inject",
        "External WebSocket audio injector — "
        "connects to a WS server, receives audio_chunk JSON with base64 PCM, "
        "injects decoded audio into the caller's ear. "
        "Auto-reconnects every 10 seconds. Logs status every 30 seconds.",
        ext_audio_inject_function,
        EXT_INJECT_API_SYNTAX
    );

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
        "══════════════════════════════════════════════════════\n"
        "  mod_audio_stream LOADED\n"
        "  API: uuid_ext_audio_inject <uuid> start|stop|status\n"
        "══════════════════════════════════════════════════════\n");

    return SWITCH_STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Module shutdown                                                          */
/* ═══════════════════════════════════════════════════════════════════════════ */

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_audio_stream_shutdown)
{
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                      "[mod_audio_stream] SHUTTING DOWN — stopping all injectors...\n");

    /* Stop all running external audio injectors */
    external_audio_inject_shutdown();

    switch_event_free_subclass(EVENT_JSON);
    switch_event_free_subclass(EVENT_CONNECT);
    switch_event_free_subclass(EVENT_DISCONNECT);
    switch_event_free_subclass(EVENT_ERROR);
    switch_event_free_subclass(EVENT_PLAY);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                      "[mod_audio_stream] SHUTDOWN COMPLETE ✓\n");

    return SWITCH_STATUS_SUCCESS;
}
