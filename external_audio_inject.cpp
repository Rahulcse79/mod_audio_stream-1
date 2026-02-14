/**
 * ═══════════════════════════════════════════════════════════════════════════
 * external_audio_inject.cpp
 *
 * Production-grade external WebSocket audio injector for FreeSWITCH.
 *
 * WHAT IT DOES:
 *   1. Connects to an external WebSocket server (e.g. ws://server:8777)
 *   2. Receives JSON messages with { type:"audio_chunk", callId:"<UUID>",
 *      sampleRate:<int>, audioData:"<base64 PCM-16-LE>" }
 *   3. Matches callId to the target FreeSWITCH channel UUID
 *   4. Decodes base64 → raw PCM-16-LE, resamples to call's native rate
 *   5. Injects audio into the call via WRITE_REPLACE media bug
 *      → the CALLER hears the injected audio
 *
 * RECONNECT POLICY:
 *   - If WebSocket disconnects, retry every 10 seconds (fixed interval)
 *   - No limit on reconnect attempts — keeps trying forever until stopped
 *
 * LOGGING POLICY:
 *   - Every 30 seconds: log whether WebSocket is connected or not
 *   - Log every connection, disconnection, error, and chunk injection
 *   - Log playback stats (write_calls, underruns, buffer usage) every 30s
 *
 * FULLY SELF-CONTAINED — does NOT depend on uuid_audio_stream.
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include <string>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <thread>
#include <chrono>

#include "external_audio_inject.h"
#include "WebSocketClient.h"
#include "base64.h"

#include <switch.h>
#include <switch_json.h>
#include <speex/speex_resampler.h>

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Constants                                                                */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define TAG                     "EXT_INJECT"
#define EXT_INJ_BUG_NAME        "ext_audio_inject"
#define RESAMPLE_QUALITY        7               /* broadcast-grade sinc       */
#define MAX_AUDIO_B64           (4 * 1024 * 1024)
#define INJECT_BUFFER_MS        20000           /* 20 seconds of PCM buffer   */
#define FRAME_SIZE_8000         320             /* 20 ms @ 8 kHz mono         */

/*
 * RECONNECT:  Fixed 10-second interval, unlimited attempts.
 */
#define RECONNECT_INTERVAL_MS   10000
#define RECONNECT_CHECK_MS      500             /* how often the thread wakes */

/*
 * LOGGING:  Status printed every 30 seconds.
 */
#define STATUS_LOG_INTERVAL_US  (30LL * 1000000LL)  /* 30 s in microseconds  */
#define PLAYBACK_LOG_INTERVAL_US (30LL * 1000000LL)

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Utility helpers                                                          */
/* ═══════════════════════════════════════════════════════════════════════════ */

static inline size_t pcm16_bytes_per_ms(int sr, int ch) {
    if (sr <= 0 || ch <= 0) return 0;
    return (size_t)sr * 2u * (size_t)ch / 1000u;
}

static inline bool host_is_little_endian() {
    const uint16_t x = 1;
    return *((const uint8_t*)&x) == 1;
}

static inline void byteswap16(std::string& s) {
    const size_t n = s.size() & ~size_t(1);
    for (size_t i = 0; i < n; i += 2)
        std::swap(s[i], s[i + 1]);
}

static inline void drop_oldest(switch_buffer_t* buf, switch_size_t bytes) {
    if (!buf || bytes == 0) return;
    uint8_t tmp[8192];
    size_t rem = (size_t)bytes;
    while (rem > 0) {
        size_t chunk = rem > sizeof(tmp) ? sizeof(tmp) : rem;
        switch_buffer_read(buf, tmp, (switch_size_t)chunk);
        rem -= chunk;
    }
}

/**
 * Resample mono PCM-16-LE using Speex.
 * Returns resampled data as std::string, or original data if no resample needed.
 */
static std::string resample_mono(const uint8_t* in, size_t in_bytes,
                                 int in_sr, int out_sr,
                                 SpeexResamplerState* resampler) {
    if (!resampler || in_sr == out_sr)
        return std::string((const char*)in, in_bytes);

    const spx_uint32_t in_frames = (spx_uint32_t)(in_bytes / 2);
    if (in_frames == 0) return {};

    const uint64_t nom = (uint64_t)in_frames * (uint64_t)out_sr;
    const uint32_t out_cap = (uint32_t)(nom / (uint64_t)in_sr + 16);
    std::vector<int16_t> out(out_cap);

    spx_uint32_t iLen = in_frames;
    spx_uint32_t oLen = out_cap;
    int err = speex_resampler_process_int(resampler, 0,
                                          (const spx_int16_t*)in, &iLen,
                                          out.data(), &oLen);
    if (err != RESAMPLER_ERR_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "[%s] speex resample error: %s\n",
                          TAG, speex_resampler_strerror(err));
        return std::string((const char*)in, in_bytes);
    }

    return std::string((const char*)out.data(), (size_t)oLen * 2u);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Per-call injector state                                                  */
/* ═══════════════════════════════════════════════════════════════════════════ */

struct ExtInjector {
    /* Identity */
    std::string               uuid;            /* FreeSWITCH channel UUID    */
    std::string               ws_uri;          /* ws://server:port/path      */

    /* FreeSWITCH resources (allocated from session pool) */
    switch_mutex_t*           mutex;           /* guards inject_buffer       */
    switch_buffer_t*          inject_buffer;   /* ring buffer for PCM        */
    int                       call_sample_rate;
    int                       call_channels;
    switch_media_bug_t*       bug;

    uint8_t*                  scratch;         /* temp frame buffer          */
    switch_size_t             scratch_len;

    /* WebSocket client */
    std::unique_ptr<WebSocketClient> client;

    /* Speex resampler (re-created when source sample rate changes) */
    SpeexResamplerState*      resampler;
    int                       last_in_sr;
    int                       last_out_sr;

    /* Thread / lifecycle */
    std::atomic<bool>         ws_connected;
    std::atomic<bool>         stopping;
    std::atomic<bool>         cleanup_done;
    std::thread               reconnect_thread;

    /* Counters */
    std::atomic<uint64_t>     chunks_received;
    std::atomic<uint64_t>     bytes_injected;
    std::atomic<uint64_t>     chunks_skipped;
    std::atomic<uint64_t>     reconnect_count;
    uint64_t                  write_calls;
    uint64_t                  underruns;

    /* Timestamps */
    switch_time_t             last_playback_log;
    switch_time_t             last_status_log;
    switch_time_t             started_at;

    ExtInjector()
        : mutex(nullptr), inject_buffer(nullptr),
          call_sample_rate(8000), call_channels(1), bug(nullptr),
          scratch(nullptr), scratch_len(0),
          resampler(nullptr), last_in_sr(0), last_out_sr(0),
          ws_connected(false), stopping(false), cleanup_done(false),
          chunks_received(0), bytes_injected(0),
          chunks_skipped(0), reconnect_count(0),
          write_calls(0), underruns(0),
          last_playback_log(0), last_status_log(0), started_at(0) {}

    ~ExtInjector() {
        if (resampler) {
            speex_resampler_destroy(resampler);
            resampler = nullptr;
        }
    }
};

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Global registry  (guarded by g_registry_mutex)                           */
/* ═══════════════════════════════════════════════════════════════════════════ */

static std::mutex                                            g_registry_mutex;
static std::unordered_map<std::string,
                          std::shared_ptr<ExtInjector>>       g_injectors;

/* Forward declarations */
static void do_ws_connect(std::shared_ptr<ExtInjector> inj);
static void reconnect_loop(std::weak_ptr<ExtInjector> wp);

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Media bug callback  (WRITE_REPLACE — mixes inject_buffer into the call)  */
/* ═══════════════════════════════════════════════════════════════════════════ */

static switch_bool_t ext_inject_callback(switch_media_bug_t *bug,
                                         void *user_data,
                                         switch_abc_type_t type)
{
    auto* inj = static_cast<ExtInjector*>(user_data);
    if (!inj) return SWITCH_TRUE;

    switch (type) {

    /* ── INIT ── */
    case SWITCH_ABC_TYPE_INIT:
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                          "[%s] %s: media bug INIT — audio injection pipeline ACTIVE\n",
                          TAG, inj->uuid.c_str());
        break;

    /* ── CLOSE (call hangup) ── */
    case SWITCH_ABC_TYPE_CLOSE:
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                          "[%s] %s: media bug CLOSE — call ending\n"
                          "[%s]   chunks_received = %llu\n"
                          "[%s]   bytes_injected  = %llu\n"
                          "[%s]   total_underruns = %llu\n",
                          TAG, inj->uuid.c_str(),
                          TAG, (unsigned long long)inj->chunks_received.load(),
                          TAG, (unsigned long long)inj->bytes_injected.load(),
                          TAG, (unsigned long long)inj->underruns);

        /* Signal all threads to stop */
        inj->stopping.store(true);

        /* Remove from global registry */
        {
            std::lock_guard<std::mutex> lk(g_registry_mutex);
            g_injectors.erase(inj->uuid);
        }

        /* Disconnect WebSocket */
        if (inj->client) {
            inj->client->disconnect();
        }

        /* Wait for reconnect thread to finish */
        if (inj->reconnect_thread.joinable()) {
            inj->reconnect_thread.join();
        }

        /* Cleanup resampler */
        if (inj->resampler) {
            speex_resampler_destroy(inj->resampler);
            inj->resampler = nullptr;
        }

        inj->cleanup_done.store(true);

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                          "[%s] %s: cleanup COMPLETE\n", TAG, inj->uuid.c_str());
        break;
    }

    /* ── WRITE_REPLACE (play inject_buffer → caller's ear) ── */
    case SWITCH_ABC_TYPE_WRITE_REPLACE:
    {
        switch_core_session_t *session = switch_core_media_bug_get_session(bug);
        switch_frame_t *frame = switch_core_media_bug_get_write_replace_frame(bug);

        if (!frame || !frame->data || frame->datalen == 0) break;

        const switch_size_t need = frame->datalen;
        switch_size_t got = 0;

        /* Try-lock: never block the media thread */
        if (switch_mutex_trylock(inj->mutex) != SWITCH_STATUS_SUCCESS) {
            break;
        }

        if (!inj->inject_buffer) {
            switch_mutex_unlock(inj->mutex);
            break;
        }

        /* Ensure scratch buffer is big enough */
        if (!inj->scratch || inj->scratch_len < need) {
            inj->scratch = (uint8_t*)switch_core_session_alloc(session, need);
            if (!inj->scratch) {
                switch_mutex_unlock(inj->mutex);
                break;
            }
            inj->scratch_len = need;
        }

        uint8_t* tmp = inj->scratch;
        memset(tmp, 0, need);

        const switch_size_t avail = switch_buffer_inuse(inj->inject_buffer);
        const switch_size_t to_read = (avail > need) ? need : avail;

        if (to_read > 0) {
            got = switch_buffer_read(inj->inject_buffer, tmp, to_read);
        }

        if (got < need) inj->underruns++;
        inj->write_calls++;

        const unsigned long inuse_now = inj->inject_buffer
            ? (unsigned long)switch_buffer_inuse(inj->inject_buffer) : 0;

        switch_mutex_unlock(inj->mutex);

        /* Write audio into the frame */
        if (got > 0) {
            memcpy(frame->data, tmp, got);
            if (got < need) {
                memset((uint8_t*)frame->data + got, 0, need - got);
            }
            switch_core_media_bug_set_write_replace_frame(bug, frame);
        }

        /* ── Periodic playback stats log (every 30 seconds) ── */
        const switch_time_t now = switch_micro_time_now();
        if (inj->last_playback_log == 0) inj->last_playback_log = now;

        if ((now - inj->last_playback_log) >= PLAYBACK_LOG_INTERVAL_US) {
            const uint64_t snap_calls = inj->write_calls;
            const uint64_t snap_under = inj->underruns;
            const double loss_pct = snap_calls
                ? (100.0 * (double)snap_under / (double)snap_calls) : 0.0;

            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                "[%s] %s: PLAYBACK STATS (30s)\n"
                "[%s]   write_calls    = %llu\n"
                "[%s]   underruns      = %llu\n"
                "[%s]   loss%%          = %.1f%%\n"
                "[%s]   buffer_inuse   = %lu bytes\n"
                "[%s]   ws_connected   = %s\n"
                "[%s]   chunks_rx      = %llu\n"
                "[%s]   bytes_injected = %llu\n"
                "[%s]   reconnects     = %llu\n",
                TAG, inj->uuid.c_str(),
                TAG, (unsigned long long)snap_calls,
                TAG, (unsigned long long)snap_under,
                TAG, loss_pct,
                TAG, inuse_now,
                TAG, inj->ws_connected.load() ? "YES" : "NO",
                TAG, (unsigned long long)inj->chunks_received.load(),
                TAG, (unsigned long long)inj->bytes_injected.load(),
                TAG, (unsigned long long)inj->reconnect_count.load());

            /* Reset periodic counters */
            inj->write_calls = 0;
            inj->underruns = 0;
            inj->last_playback_log = now;
        }
        break;
    }

    default:
        break;
    }

    return SWITCH_TRUE;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  WebSocket message handler                                                */
/*                                                                           */
/*  Expected JSON from server:                                               */
/*  {                                                                        */
/*    "type"      : "audio_chunk",                                           */
/*    "callId"    : "<FreeSWITCH-UUID>",          ← MUST match our UUID     */
/*    "sampleRate": 8000,                                                    */
/*    "audioData" : "<base64 raw PCM-16-LE>"                                 */
/*    "seqNum"    : 0,               (optional)                              */
/*    "isFinal"   : false            (optional)                              */
/*  }                                                                        */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void on_ws_message(std::weak_ptr<ExtInjector> wp, const std::string& message) {
    auto inj = wp.lock();
    if (!inj || inj->stopping.load()) return;

    const char* uuid = inj->uuid.c_str();

    /* ── 1) Parse JSON ── */
    cJSON* root = cJSON_Parse(message.c_str());
    if (!root) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                          "[%s] %s: received non-JSON frame (len=%zu), IGNORING\n",
                          TAG, uuid, message.size());
        return;
    }

    /* ── 2) Validate type == "audio_chunk" ── */
    const char* type = cJSON_GetObjectCstr(root, "type");
    if (!type || std::strcmp(type, "audio_chunk") != 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                          "[%s] %s: ignoring message type='%s' (not audio_chunk)\n",
                          TAG, uuid, type ? type : "(null)");
        cJSON_Delete(root);
        return;
    }

    /* ── 3) Validate callId matches our UUID ── */
    const char* callId = cJSON_GetObjectCstr(root, "callId");
    if (!callId) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                          "[%s] %s: audio_chunk missing 'callId' field — SKIPPING\n",
                          TAG, uuid);
        cJSON_Delete(root);
        return;
    }
    if (inj->uuid != callId) {
        inj->chunks_skipped++;
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                          "[%s] %s: callId='%s' does NOT match our UUID — skipping\n",
                          TAG, uuid, callId);
        cJSON_Delete(root);
        return;
    }

    /* ── 4) Extract optional fields ── */
    int sampleRate = 0;
    {
        cJSON* sr = cJSON_GetObjectItem(root, "sampleRate");
        if (sr && cJSON_IsNumber(sr)) sampleRate = sr->valueint;
    }
    int seqNum = 0;
    {
        cJSON* sq = cJSON_GetObjectItem(root, "seqNum");
        if (sq && cJSON_IsNumber(sq)) seqNum = sq->valueint;
    }
    bool isFinal = false;
    {
        cJSON* fin = cJSON_GetObjectItem(root, "isFinal");
        if (fin) isFinal = cJSON_IsTrue(fin);
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                      "[%s] %s: audio_chunk — seq=%d sampleRate=%d isFinal=%d\n",
                      TAG, uuid, seqNum, sampleRate, (int)isFinal);

    /* ── 5) Decode base64 audioData → raw PCM ── */
    const char* audioB64 = cJSON_GetObjectCstr(root, "audioData");
    if (!audioB64 || !*audioB64) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                          "[%s] %s: audio_chunk seq=%d has EMPTY audioData\n",
                          TAG, uuid, seqNum);
        cJSON_Delete(root);
        return;
    }

    const size_t b64len = std::strlen(audioB64);
    if (b64len > MAX_AUDIO_B64) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "[%s] %s: audioData TOO LARGE (%zu bytes b64, max=%d)\n",
                          TAG, uuid, b64len, MAX_AUDIO_B64);
        cJSON_Delete(root);
        return;
    }

    std::string decoded;
    try {
        decoded = base64_decode(std::string(audioB64));
    } catch (const std::exception& e) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "[%s] %s: base64 decode FAILED: %s\n",
                          TAG, uuid, e.what());
        cJSON_Delete(root);
        return;
    }
    cJSON_Delete(root);

    if (decoded.size() < 2) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                          "[%s] %s: decoded audio too small (%zu bytes)\n",
                          TAG, uuid, decoded.size());
        return;
    }

    /* Align to 16-bit sample boundary */
    if (decoded.size() % 2 != 0)
        decoded.resize(decoded.size() - 1);

    /* Byte-swap if host is big-endian (PCM data is little-endian) */
    if (!host_is_little_endian())
        byteswap16(decoded);

    if (sampleRate <= 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "[%s] %s: invalid sampleRate=%d in seq=%d, cannot inject\n",
                          TAG, uuid, sampleRate, seqNum);
        return;
    }

    inj->chunks_received++;

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                      "[%s] %s: decoded %zu PCM bytes from seq=%d (sr=%d)\n",
                      TAG, uuid, decoded.size(), seqNum, sampleRate);

    /* ── 6) Resample to call's native sample rate ── */
    const int out_sr = inj->call_sample_rate;
    if (out_sr <= 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "[%s] %s: call_sample_rate is %d — cannot inject\n",
                          TAG, uuid, out_sr);
        return;
    }

    if (sampleRate != out_sr) {
        /* (Re-)create resampler if source rate changed */
        if (!inj->resampler || inj->last_in_sr != sampleRate || inj->last_out_sr != out_sr) {
            if (inj->resampler) {
                speex_resampler_destroy(inj->resampler);
                inj->resampler = nullptr;
            }
            int err = 0;
            inj->resampler = speex_resampler_init(1, sampleRate, out_sr,
                                                   RESAMPLE_QUALITY, &err);
            if (err != 0 || !inj->resampler) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                                  "[%s] %s: resampler init FAILED %d→%d: %s\n",
                                  TAG, uuid, sampleRate, out_sr,
                                  speex_resampler_strerror(err));
                return;
            }
            inj->last_in_sr = sampleRate;
            inj->last_out_sr = out_sr;
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                              "[%s] %s: Speex resampler created: %d → %d Hz (quality=%d)\n",
                              TAG, uuid, sampleRate, out_sr, RESAMPLE_QUALITY);
        }

        decoded = resample_mono((const uint8_t*)decoded.data(), decoded.size(),
                                sampleRate, out_sr, inj->resampler);
        if (decoded.empty()) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                              "[%s] %s: resample produced EMPTY output\n", TAG, uuid);
            return;
        }

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                          "[%s] %s: resampled %d→%d Hz → %zu bytes\n",
                          TAG, uuid, sampleRate, out_sr, decoded.size());
    }

    /* ── 7) Upmix mono → stereo if call is stereo ── */
    if (inj->call_channels == 2) {
        const size_t mono_frames = decoded.size() / 2;
        std::string stereo;
        stereo.resize(mono_frames * 4);
        for (size_t i = 0; i < mono_frames; ++i) {
            int16_t s;
            std::memcpy(&s, decoded.data() + i * 2, 2);
            std::memcpy(&stereo[i * 4],     &s, 2);
            std::memcpy(&stereo[i * 4 + 2], &s, 2);
        }
        decoded = std::move(stereo);
    }

    /* ── 8) Write decoded PCM into inject_buffer ── */
    switch_mutex_lock(inj->mutex);

    if (!inj->inject_buffer) {
        switch_mutex_unlock(inj->mutex);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                          "[%s] %s: inject_buffer is NULL — cannot inject audio\n",
                          TAG, uuid);
        return;
    }

    const switch_size_t inuse  = switch_buffer_inuse(inj->inject_buffer);
    const switch_size_t fspace = switch_buffer_freespace(inj->inject_buffer);
    const size_t max_bytes     = (size_t)inuse + (size_t)fspace;

    if (max_bytes > 0 && decoded.size() > max_bytes) {
        const size_t align = (size_t)inj->call_channels * 2u;
        size_t keep = max_bytes;
        if (align > 0)
            keep = (keep / align) * align;  /* align down to sample boundary */

        const size_t skip = decoded.size() - keep;
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                          "[%s] %s: incoming audio (%zu bytes) EXCEEDS buffer capacity (%zu bytes) "
                          "— truncating, skipping oldest %zu bytes\n",
                          TAG, uuid, decoded.size(), max_bytes, skip);
        decoded.erase(0, skip);
    }

    /* If buffer would overflow with existing + new data, drop oldest existing data */
    if (max_bytes > 0 && (size_t)inuse + decoded.size() > max_bytes) {
        size_t over = ((size_t)inuse + decoded.size()) - max_bytes;
        const size_t align = (size_t)inj->call_channels * 2u;
        if (align > 0)
            over = ((over + align - 1) / align) * align;
        if (over > (size_t)inuse) over = (size_t)inuse;
        if (over > 0) {
            drop_oldest(inj->inject_buffer, (switch_size_t)over);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                              "[%s] %s: buffer OVERFLOW — dropped %zu oldest bytes\n",
                              TAG, uuid, over);
        }
    }

    const switch_size_t written = switch_buffer_write(inj->inject_buffer, decoded.data(), (switch_size_t)decoded.size());
    const unsigned long inuse_after = (unsigned long)switch_buffer_inuse(inj->inject_buffer);

    if ((size_t)written != decoded.size()) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "[%s] %s: switch_buffer_write PARTIAL — wanted %zu, wrote %u\n",
                          TAG, uuid, decoded.size(), (unsigned)written);
    }

    switch_mutex_unlock(inj->mutex);

    inj->bytes_injected += decoded.size();

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                      "[%s] %s: INJECTED seq=%d %zu bytes → buffer_inuse=%lu\n",
                      TAG, uuid, seqNum, decoded.size(), inuse_after);

    if (isFinal) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                          "[%s] %s: isFinal=true on seq=%d — audio stream COMPLETE "
                          "(total: chunks=%llu bytes=%llu)\n",
                          TAG, uuid, seqNum,
                          (unsigned long long)inj->chunks_received.load(),
                          (unsigned long long)inj->bytes_injected.load());
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  WebSocket connect logic                                                  */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void do_ws_connect(std::shared_ptr<ExtInjector> inj) {
    if (inj->stopping.load()) return;

    const char* uuid = inj->uuid.c_str();
    const char* uri  = inj->ws_uri.c_str();

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                      "[%s] %s: CONNECTING to WebSocket %s ...\n",
                      TAG, uuid, uri);

    /* Create a fresh client (libwsc doesn't support reconnect on same object) */
    inj->client.reset(new WebSocketClient());
    inj->client->setUrl(inj->ws_uri);
    inj->client->setConnectionTimeout(5);

    /* Get weak_ptr from global registry for safe callbacks */
    std::weak_ptr<ExtInjector> wp;
    {
        std::lock_guard<std::mutex> lk(g_registry_mutex);
        auto it = g_injectors.find(inj->uuid);
        if (it != g_injectors.end()) {
            wp = it->second;
        }
    }

    /* ── on open ── */
    inj->client->setOpenCallback([wp, uri]() {
        auto self = wp.lock();
        if (!self) return;
        self->ws_connected.store(true);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                          "[%s] %s: ✓ WebSocket CONNECTED to %s\n",
                          TAG, self->uuid.c_str(), uri);

        /*
         * Send a "register" message so the server knows our call UUID.
         * The server will then auto-stream audio tagged with our callId.
         *
         * JSON: { "type": "register", "callId": "<UUID>" }
         */
        cJSON* reg = cJSON_CreateObject();
        if (reg) {
            cJSON_AddStringToObject(reg, "type", "register");
            cJSON_AddStringToObject(reg, "callId", self->uuid.c_str());
            char* json_str = cJSON_PrintUnformatted(reg);
            if (json_str) {
                self->client->sendMessage(json_str, std::strlen(json_str));
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                                  "[%s] %s: sent REGISTER message to server (callId=%s)\n",
                                  TAG, self->uuid.c_str(), self->uuid.c_str());
                std::free(json_str);
            }
            cJSON_Delete(reg);
        }
    });

    /* ── on message ── */
    inj->client->setMessageCallback([wp](const std::string& msg) {
        on_ws_message(wp, msg);
    });

    /* ── on error ── */
    inj->client->setErrorCallback([wp](int code, const std::string& errmsg) {
        auto self = wp.lock();
        if (!self) return;
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "[%s] %s: WebSocket ERROR (code=%d): %s\n",
                          TAG, self->uuid.c_str(), code, errmsg.c_str());
    });

    /* ── on close ── */
    inj->client->setCloseCallback([wp](int code, const std::string& reason) {
        auto self = wp.lock();
        if (!self) return;
        self->ws_connected.store(false);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                          "[%s] %s: ✗ WebSocket DISCONNECTED (code=%d reason='%s')\n"
                          "[%s]   Will retry in %d seconds...\n",
                          TAG, self->uuid.c_str(), code, reason.c_str(),
                          TAG, RECONNECT_INTERVAL_MS / 1000);
    });

    inj->client->connect();
}


static void reconnect_loop(std::weak_ptr<ExtInjector> wp) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                      "[%s] reconnect_loop: background thread STARTED\n", TAG);

    switch_time_t last_status_print = switch_micro_time_now();
    switch_time_t last_reconnect_attempt = 0;

    while (true) {
        /* Sleep between checks */
        std::this_thread::sleep_for(std::chrono::milliseconds(RECONNECT_CHECK_MS));

        auto inj = wp.lock();
        if (!inj || inj->stopping.load()) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                              "[%s] reconnect_loop: EXITING (injector stopping or released)\n",
                              TAG);
            return;
        }

        const switch_time_t now = switch_micro_time_now();
        const bool connected = inj->ws_connected.load();

        /* ── Log WebSocket status every 30 seconds ── */
        if ((now - last_status_print) >= STATUS_LOG_INTERVAL_US) {
            const double uptime_secs = inj->started_at > 0
                ? (double)(now - inj->started_at) / 1000000.0 : 0.0;

            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                "[%s] %s: ═══ STATUS (every 30s) ═══\n"
                "[%s]   ws_connected   = %s\n"
                "[%s]   ws_uri         = %s\n"
                "[%s]   uptime         = %.0f seconds\n"
                "[%s]   chunks_rx      = %llu\n"
                "[%s]   bytes_injected = %llu\n"
                "[%s]   chunks_skipped = %llu\n"
                "[%s]   reconnects     = %llu\n",
                TAG, inj->uuid.c_str(),
                TAG, connected ? "YES ✓" : "NO ✗",
                TAG, inj->ws_uri.c_str(),
                TAG, uptime_secs,
                TAG, (unsigned long long)inj->chunks_received.load(),
                TAG, (unsigned long long)inj->bytes_injected.load(),
                TAG, (unsigned long long)inj->chunks_skipped.load(),
                TAG, (unsigned long long)inj->reconnect_count.load());

            last_status_print = now;
        }

        /* ── If connected, nothing to do ── */
        if (connected) continue;

        /* ── Check if enough time has passed since last reconnect attempt ── */
        const switch_time_t elapsed_since_last = now - last_reconnect_attempt;
        if (last_reconnect_attempt > 0 &&
            elapsed_since_last < (switch_time_t)RECONNECT_INTERVAL_MS * 1000LL) {
            continue;  /* not yet 10 seconds, keep waiting */
        }

        /* ── Time to reconnect ── */
        last_reconnect_attempt = now;
        inj->reconnect_count++;

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                          "[%s] %s: RECONNECTING to %s (attempt #%llu, every %ds)...\n",
                          TAG, inj->uuid.c_str(), inj->ws_uri.c_str(),
                          (unsigned long long)inj->reconnect_count.load(),
                          RECONNECT_INTERVAL_MS / 1000);

        /* Disconnect old client if any */
        if (inj->client) {
            inj->client->disconnect();
            inj->client.reset();
        }

        do_ws_connect(inj);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Public C API                                                             */
/* ═══════════════════════════════════════════════════════════════════════════ */

extern "C" {

/**
 * Start external audio injection for a given call UUID.
 *
 * Usage from fs_cli:
 *   uuid_ext_audio_inject <UUID> start ws://server:8777
 */
switch_status_t external_audio_inject_start(const char *uuid, const char *ws_uri) {
    /* ── Validate inputs ── */
    if (!uuid || !*uuid) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "[%s] START: uuid is NULL or empty!\n", TAG);
        return SWITCH_STATUS_FALSE;
    }
    if (!ws_uri || !*ws_uri) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "[%s] START: ws_uri is NULL or empty!\n", TAG);
        return SWITCH_STATUS_FALSE;
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                      "[%s] START: uuid=%s ws_uri=%s\n", TAG, uuid, ws_uri);

    /* ── Check for duplicate ── */
    {
        std::lock_guard<std::mutex> lk(g_registry_mutex);
        if (g_injectors.count(std::string(uuid))) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                              "[%s] START: injector ALREADY running for uuid=%s\n",
                              TAG, uuid);
            return SWITCH_STATUS_FALSE;
        }
    }

    /* ── Locate FreeSWITCH session ── */
    switch_core_session_t* session = switch_core_session_locate(uuid);
    if (!session) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "[%s] START: session NOT FOUND for uuid=%s\n", TAG, uuid);
        return SWITCH_STATUS_FALSE;
    }

    switch_channel_t* channel = switch_core_session_get_channel(session);
    if (!channel) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "[%s] START: no channel for uuid=%s\n", TAG, uuid);
        switch_core_session_rwunlock(session);
        return SWITCH_STATUS_FALSE;
    }

    /* Check if media bug already exists */
    if (switch_channel_get_private(channel, EXT_INJ_BUG_NAME)) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                          "[%s] START: media bug '%s' already exists on uuid=%s\n",
                          TAG, EXT_INJ_BUG_NAME, uuid);
        switch_core_session_rwunlock(session);
        return SWITCH_STATUS_FALSE;
    }

    /* Get codec info for the call */
    switch_codec_t* read_codec = switch_core_session_get_read_codec(session);
    if (!read_codec || !read_codec->implementation) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "[%s] START: no read codec for uuid=%s — is the call active?\n",
                          TAG, uuid);
        switch_core_session_rwunlock(session);
        return SWITCH_STATUS_FALSE;
    }

    const int native_sr = read_codec->implementation->actual_samples_per_second;
    const int channels  = 1; /* mono */

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                      "[%s] START: call native_sample_rate=%d channels=%d\n",
                      TAG, native_sr, channels);

    /* ── Create injector state ── */
    auto inj = std::make_shared<ExtInjector>();
    inj->uuid              = uuid;
    inj->ws_uri            = ws_uri;
    inj->call_sample_rate  = native_sr;
    inj->call_channels     = channels;
    inj->started_at        = switch_micro_time_now();

    /* Allocate mutex from session pool */
    switch_memory_pool_t* pool = switch_core_session_get_pool(session);
    switch_mutex_init(&inj->mutex, SWITCH_MUTEX_NESTED, pool);

    /* Allocate injection ring buffer */
    const size_t buf_bytes = pcm16_bytes_per_ms(native_sr, channels) * INJECT_BUFFER_MS;
    const size_t buf_size  = std::max<size_t>(buf_bytes, 3200u);
    if (switch_buffer_create(pool, &inj->inject_buffer, buf_size) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "[%s] START: FAILED to create inject_buffer\n", TAG);
        switch_core_session_rwunlock(session);
        return SWITCH_STATUS_FALSE;
    }

    /* Allocate scratch buffer for frame processing */
    const size_t scratch_sz = FRAME_SIZE_8000 * channels;
    inj->scratch     = (uint8_t*)switch_core_session_alloc(session, scratch_sz);
    inj->scratch_len = scratch_sz;
    if (!inj->scratch) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "[%s] START: FAILED to alloc scratch buffer\n", TAG);
        switch_core_session_rwunlock(session);
        return SWITCH_STATUS_FALSE;
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                      "[%s] START: inject_buffer=%zu bytes, scratch=%zu bytes\n",
                      TAG, buf_size, scratch_sz);

    /* ── Add WRITE_REPLACE media bug ── */
    switch_media_bug_t* bug = nullptr;
    switch_status_t bug_status = switch_core_media_bug_add(
        session,
        EXT_INJ_BUG_NAME,
        NULL,
        ext_inject_callback,
        inj.get(),       /* raw ptr; shared_ptr in g_injectors keeps it alive */
        0,
        SMBF_WRITE_REPLACE,
        &bug
    );

    if (bug_status != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "[%s] START: FAILED to add media bug for uuid=%s\n", TAG, uuid);
        switch_core_session_rwunlock(session);
        return SWITCH_STATUS_FALSE;
    }

    inj->bug = bug;
    switch_channel_set_private(channel, EXT_INJ_BUG_NAME, bug);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                      "[%s] START: media bug '%s' added successfully\n",
                      TAG, EXT_INJ_BUG_NAME);

    /* ── Register BEFORE starting WS (so callbacks can find us) ── */
    {
        std::lock_guard<std::mutex> lk(g_registry_mutex);
        g_injectors[inj->uuid] = inj;
    }

    /* ── Connect WebSocket ── */
    do_ws_connect(inj);

    /* ── Start reconnect monitor thread ── */
    std::weak_ptr<ExtInjector> wp = inj;
    inj->reconnect_thread = std::thread(reconnect_loop, wp);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
        "[%s] ══════════════════════════════════════════════════\n"
        "[%s]  EXTERNAL AUDIO INJECTOR STARTED\n"
        "[%s]    uuid        = %s\n"
        "[%s]    ws_uri      = %s\n"
        "[%s]    sample_rate = %d\n"
        "[%s]    channels    = %d\n"
        "[%s]    buffer_ms   = %d\n"
        "[%s]    reconnect   = every %d seconds (unlimited)\n"
        "[%s]    status_log  = every 30 seconds\n"
        "[%s] ══════════════════════════════════════════════════\n",
        TAG, TAG, TAG, uuid, TAG, ws_uri, TAG, native_sr,
        TAG, channels, TAG, INJECT_BUFFER_MS,
        TAG, RECONNECT_INTERVAL_MS / 1000, TAG, TAG);

    switch_core_session_rwunlock(session);
    return SWITCH_STATUS_SUCCESS;
}

/**
 * Stop external audio injection for a given call UUID.
 */
switch_status_t external_audio_inject_stop(const char *uuid) {
    if (!uuid || !*uuid) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "[%s] STOP: uuid is NULL or empty\n", TAG);
        return SWITCH_STATUS_FALSE;
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                      "[%s] STOP: uuid=%s\n", TAG, uuid);

    std::shared_ptr<ExtInjector> inj;
    {
        std::lock_guard<std::mutex> lk(g_registry_mutex);
        auto it = g_injectors.find(std::string(uuid));
        if (it == g_injectors.end()) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                              "[%s] STOP: no injector running for uuid=%s\n", TAG, uuid);
            return SWITCH_STATUS_FALSE;
        }
        inj = it->second;
        g_injectors.erase(it);
    }

    inj->stopping.store(true);

    /* Disconnect WebSocket */
    if (inj->client) {
        inj->client->disconnect();
        inj->client.reset();
    }

    /* Wait for reconnect thread */
    if (inj->reconnect_thread.joinable()) {
        inj->reconnect_thread.join();
    }

    /* Remove the media bug from the channel */
    switch_core_session_t* session = switch_core_session_locate(uuid);
    if (session) {
        switch_channel_t* channel = switch_core_session_get_channel(session);
        if (channel) {
            switch_channel_set_private(channel, EXT_INJ_BUG_NAME, nullptr);
        }
        if (inj->bug) {
            switch_core_media_bug_remove(session, &inj->bug);
            inj->bug = nullptr;
        }
        switch_core_session_rwunlock(session);
    }

    /* Cleanup resampler */
    if (inj->resampler) {
        speex_resampler_destroy(inj->resampler);
        inj->resampler = nullptr;
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
        "[%s] ══════════════════════════════════════════════════\n"
        "[%s]  EXTERNAL AUDIO INJECTOR STOPPED\n"
        "[%s]    uuid            = %s\n"
        "[%s]    chunks_received = %llu\n"
        "[%s]    bytes_injected  = %llu\n"
        "[%s]    chunks_skipped  = %llu\n"
        "[%s]    reconnect_count = %llu\n"
        "[%s] ══════════════════════════════════════════════════\n",
        TAG, TAG, TAG, uuid,
        TAG, (unsigned long long)inj->chunks_received.load(),
        TAG, (unsigned long long)inj->bytes_injected.load(),
        TAG, (unsigned long long)inj->chunks_skipped.load(),
        TAG, (unsigned long long)inj->reconnect_count.load(),
        TAG);

    return SWITCH_STATUS_SUCCESS;
}

/**
 * Get live status of an active injector.
 */
switch_status_t external_audio_inject_status(const char *uuid, switch_stream_handle_t *stream) {
    if (!uuid || !*uuid || !stream) {
        return SWITCH_STATUS_FALSE;
    }

    std::shared_ptr<ExtInjector> inj;
    {
        std::lock_guard<std::mutex> lk(g_registry_mutex);
        auto it = g_injectors.find(std::string(uuid));
        if (it == g_injectors.end()) {
            stream->write_function(stream,
                "[%s] No injector running for uuid=%s\n", TAG, uuid);
            return SWITCH_STATUS_FALSE;
        }
        inj = it->second;
    }

    const switch_time_t now = switch_micro_time_now();
    const double uptime_secs = inj->started_at > 0
        ? (double)(now - inj->started_at) / 1000000.0 : 0.0;

    unsigned long inuse = 0;
    if (inj->mutex && inj->inject_buffer) {
        switch_mutex_lock(inj->mutex);
        inuse = (unsigned long)switch_buffer_inuse(inj->inject_buffer);
        switch_mutex_unlock(inj->mutex);
    }

    stream->write_function(stream,
        "═══════════════════════════════════════════════\n"
        " External Audio Injector Status\n"
        "═══════════════════════════════════════════════\n"
        " UUID:             %s\n"
        " WS URI:           %s\n"
        " WS Connected:     %s\n"
        " Sample Rate:      %d Hz\n"
        " Channels:         %d\n"
        " Uptime:           %.1f seconds\n"
        " Chunks Received:  %llu\n"
        " Bytes Injected:   %llu\n"
        " Chunks Skipped:   %llu\n"
        " Reconnect Count:  %llu\n"
        " Buffer Inuse:     %lu bytes\n"
        " Reconnect Every:  %d seconds\n"
        "═══════════════════════════════════════════════\n",
        inj->uuid.c_str(),
        inj->ws_uri.c_str(),
        inj->ws_connected.load() ? "YES ✓" : "NO ✗",
        inj->call_sample_rate,
        inj->call_channels,
        uptime_secs,
        (unsigned long long)inj->chunks_received.load(),
        (unsigned long long)inj->bytes_injected.load(),
        (unsigned long long)inj->chunks_skipped.load(),
        (unsigned long long)inj->reconnect_count.load(),
        inuse,
        RECONNECT_INTERVAL_MS / 1000);

    return SWITCH_STATUS_SUCCESS;
}

/**
 * Shutdown ALL running injectors (called at module unload).
 */
void external_audio_inject_shutdown(void) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                      "[%s] SHUTDOWN: stopping ALL injectors...\n", TAG);

    std::unordered_map<std::string, std::shared_ptr<ExtInjector>> snapshot;
    {
        std::lock_guard<std::mutex> lk(g_registry_mutex);
        snapshot.swap(g_injectors);
    }

    for (auto& kv : snapshot) {
        auto& inj = kv.second;
        inj->stopping.store(true);

        if (inj->client) {
            inj->client->disconnect();
            inj->client.reset();
        }
        if (inj->reconnect_thread.joinable()) {
            inj->reconnect_thread.join();
        }
        if (inj->resampler) {
            speex_resampler_destroy(inj->resampler);
            inj->resampler = nullptr;
        }

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                          "[%s] SHUTDOWN: stopped injector uuid=%s "
                          "(chunks=%llu bytes=%llu reconnects=%llu)\n",
                          TAG, kv.first.c_str(),
                          (unsigned long long)inj->chunks_received.load(),
                          (unsigned long long)inj->bytes_injected.load(),
                          (unsigned long long)inj->reconnect_count.load());
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                      "[%s] SHUTDOWN: all injectors stopped ✓\n", TAG);
}

} /* extern "C" */
