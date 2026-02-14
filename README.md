# mod_audio_stream — External WebSocket Audio Injector for FreeSWITCH

A FreeSWITCH module that connects to an external WebSocket server, receives audio data, and injects it into an active phone call — so the **caller hears** the audio in their phone.

## How It Works

```
┌──────────┐     WS connect      ┌──────────────┐     HTTP /send      ┌──────────┐
│FreeSWITCH│ ←──────────────────→ │  WS Server   │ ←──────────────── │ Your App │
│  (call)  │   audio_chunk JSON   │ (index.js)   │   callId=<UUID>   │ (curl)   │
└──────────┘                      └──────────────┘                    └──────────┘
     │
     ▼
  Caller hears
  injected audio
```

1. A caller dials extension **1234** → FreeSWITCH answers
2. `uuid_ext_audio_inject <UUID> start ws://server:8777` connects to your WS server
3. Your app sends `GET /send?callId=<UUID>` → the WS server broadcasts `audio_chunk` JSON
4. FreeSWITCH matches `callId` to the call UUID, decodes the base64 PCM audio, and **injects it into the caller's ear**
5. If the WebSocket disconnects, it **retries every 10 seconds** automatically
6. Connection status is **logged every 30 seconds** in FreeSWITCH console

## JSON Protocol (WebSocket → FreeSWITCH)

```json
{
  "type": "audio_chunk",
  "callId": "<FreeSWITCH-channel-UUID>",
  "sampleRate": 8000,
  "audioData": "<base64-encoded raw PCM 16-bit signed little-endian>",
  "seqNum": 0,
  "isFinal": false
}
```

- `callId` — **REQUIRED**: Must match the FreeSWITCH channel UUID
- `sampleRate` — **REQUIRED**: Sample rate of the PCM audio (8000, 16000, 22050, etc.)
- `audioData` — **REQUIRED**: Base64-encoded raw PCM-16-LE audio
- `seqNum` — Optional sequence number
- `isFinal` — Optional: set `true` to indicate end of audio stream

## Build

```bash
mkdir build && cd build
cmake ..
make
sudo cp mod_audio_stream.so /usr/local/freeswitch/mod/
```

## FreeSWITCH Configuration

### 1. Load the module

Add to `/usr/local/freeswitch/conf/autoload_configs/modules.conf.xml`:
```xml
<load module="mod_audio_stream"/>
```

### 2. Add dialplan (optional)

Copy `corrected_dialplan.xml` to your dialplan directory:
```bash
cp corrected_dialplan.xml /usr/local/freeswitch/conf/dialplan/default/90_audio_inject.xml
```

This sets up extension **1234** — when someone calls it, the audio injector starts automatically.

## Usage

### From fs_cli (manual):

```
# Start audio injection for a call
uuid_ext_audio_inject <UUID> start ws://localhost:8777

# Check status
uuid_ext_audio_inject <UUID> status

# Stop
uuid_ext_audio_inject <UUID> stop
```

### From your application:

```bash
# Start the WebSocket server
cd server && npm install && node index.js

# Send audio to a specific call
curl "http://localhost:8766/send?callId=<UUID>"

# Check server status
curl "http://localhost:8766/status"
```

## Features

| Feature | Details |
|---------|---------|
| **Auto-reconnect** | Every **10 seconds** if WebSocket disconnects (unlimited retries) |
| **Status logging** | Every **30 seconds** logs connection state, chunks received, bytes injected |
| **Error logging** | Every WS error, disconnect, decode failure logged to FreeSWITCH console |
| **Resampling** | Automatic Speex resampling if audio sample rate ≠ call sample rate |
| **Buffer management** | 5-second PCM ring buffer with overflow protection |
| **Thread-safe** | Lock-free media path, mutex-guarded buffer access |
| **Clean shutdown** | Graceful cleanup on call hangup or module unload |

## Logging

The module produces detailed logs at these intervals:

- **On every event**: connection, disconnection, error, chunk injection
- **Every 30 seconds**: WebSocket connected/disconnected status, uptime, stats
- **Every 30 seconds**: Playback stats (write calls, underruns, buffer usage)

Example log output:
```
[EXT_INJECT] abc-123: ═══ STATUS (every 30s) ═══
[EXT_INJECT]   ws_connected   = YES ✓
[EXT_INJECT]   ws_uri         = ws://localhost:8777
[EXT_INJECT]   uptime         = 60 seconds
[EXT_INJECT]   chunks_rx      = 42
[EXT_INJECT]   bytes_injected = 13440
```
