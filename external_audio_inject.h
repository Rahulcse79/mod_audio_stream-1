#ifndef EXTERNAL_AUDIO_INJECT_H
#define EXTERNAL_AUDIO_INJECT_H

#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  external_audio_inject — Standalone External WebSocket Audio Injector
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Connects to an external WebSocket (e.g. ws://192.168.6.233:8080/ws/testCall),
 * receives JSON "audio_chunk" messages with base64-encoded raw PCM-16-LE audio,
 * matches them to active FreeSWITCH calls via "callId", resamples to the call's
 * native rate, and injects the audio into the call so the caller hears it.
 *
 * This is a STANDALONE system — it does NOT depend on uuid_audio_stream.
 * It creates its own media bug on the channel for WRITE_REPLACE injection.
 *
 * Protocol (JSON text frames from the WS server):
 *  {
 *    "type"      : "audio_chunk",
 *    "callId"    : "<FreeSWITCH-UUID-or-custom-id>",
 *    "seqNum"    : 0,
 *    "sampleRate": 22050,
 *    "isFinal"   : false,
 *    "timestamp" : 1644768000000000000,
 *    "audioData" : "<base64 of raw 16-bit signed PCM little-endian>"
 *  }
 *
 * Usage:
 *   uuid_ext_audio_inject <uuid> start <ws-uri>
 *   uuid_ext_audio_inject <uuid> stop
 *   uuid_ext_audio_inject <uuid> status
 */

/**
 * Start an external WebSocket audio injector for a specific call.
 */
switch_status_t external_audio_inject_start(const char *uuid, const char *ws_uri);

/**
 * Stop the external WebSocket audio injector for a given UUID.
 */
switch_status_t external_audio_inject_stop(const char *uuid);

/**
 * Get status info about an active injector.
 */
switch_status_t external_audio_inject_status(const char *uuid, switch_stream_handle_t *stream);

/**
 * Stop ALL running external injectors (called at module shutdown).
 */
void external_audio_inject_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* EXTERNAL_AUDIO_INJECT_H */
