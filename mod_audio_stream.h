/**
 * ═══════════════════════════════════════════════════════════════════════════
 * mod_audio_stream.h
 *
 * FreeSWITCH module header — External WebSocket Audio Injector
 *
 * Provides the uuid_ext_audio_inject API command:
 *   - Connects to an external WebSocket server
 *   - Receives JSON { type:"audio_chunk", callId:"<UUID>",
 *     sampleRate:8000, audioData:"<base64 PCM-16-LE>" }
 *   - Injects decoded audio into the caller's ear via WRITE_REPLACE media bug
 *   - Auto-reconnects every 10 seconds on disconnect
 *   - Logs connection status every 30 seconds
 * ═══════════════════════════════════════════════════════════════════════════
 */

#ifndef MOD_AUDIO_STREAM_H
#define MOD_AUDIO_STREAM_H

#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Module / bug name ── */
#define MY_BUG_NAME        "audio_stream"

/* ── Limits ── */
#define MAX_SESSION_ID     (256)
#define MAX_WS_URI         (4096)
#define MAX_METADATA_LEN   (8192)

/* ── Custom event names (fired on FreeSWITCH event bus) ── */
#define EVENT_CONNECT      "mod_audio_stream::connect"
#define EVENT_DISCONNECT   "mod_audio_stream::disconnect"
#define EVENT_ERROR        "mod_audio_stream::error"
#define EVENT_JSON         "mod_audio_stream::json"
#define EVENT_PLAY         "mod_audio_stream::play"

#ifdef __cplusplus
}
#endif

#endif /* MOD_AUDIO_STREAM_H */
