#!/usr/bin/env python3
import argparse
import asyncio
import base64
import io
import json
import logging
import os
import struct
import sys
import time
import wave

import pyaudio
import websockets

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("bridge-client")

# ---------------------------------------------------------------------------
# Audio config (must match what server/whisper can handle)
# We'll record PCM WAV 16kHz mono 16-bit.
# ---------------------------------------------------------------------------
RATE = 16000
CHANNELS = 1
SAMPLE_WIDTH_BYTES = 2  # 16-bit
CHUNK = 1024

# Simple VAD params
SILENCE_RMS = 400
SILENCE_SECONDS_TO_STOP = 1.2
MAX_RECORD_SECONDS = 20


def rms16(pcm: bytes) -> float:
    """RMS for 16-bit little-endian PCM."""
    if len(pcm) < 2:
        return 0.0
    n = len(pcm) // 2
    samples = struct.unpack("<%dh" % n, pcm[: n * 2])
    ssum = 0
    for s in samples:
        ssum += s * s
    return (ssum / max(1, n)) ** 0.5


def frames_to_wav(frames: list[bytes], rate: int, channels: int) -> bytes:
    bio = io.BytesIO()
    with wave.open(bio, "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(SAMPLE_WIDTH_BYTES)
        wf.setframerate(rate)
        wf.writeframes(b"".join(frames))
    return bio.getvalue()


def play_wav_bytes(wav_bytes: bytes):
    bio = io.BytesIO(wav_bytes)
    with wave.open(bio, "rb") as wf:
        pa = pyaudio.PyAudio()
        stream = pa.open(
            format=pa.get_format_from_width(wf.getsampwidth()),
            channels=wf.getnchannels(),
            rate=wf.getframerate(),
            output=True,
        )
        data = wf.readframes(CHUNK)
        while data:
            stream.write(data)
            data = wf.readframes(CHUNK)
        stream.stop_stream()
        stream.close()
        pa.terminate()


def record_vad() -> bytes | None:
    """Record from mic until silence for N seconds."""
    pa = pyaudio.PyAudio()
    stream = pa.open(
        format=pyaudio.paInt16,
        channels=CHANNELS,
        rate=RATE,
        input=True,
        frames_per_buffer=CHUNK,
    )

    log.info("🎙️ Speak now (auto-stop on silence)...")
    frames: list[bytes] = []

    start = time.time()
    last_voice = time.time()

    try:
        while True:
            pcm = stream.read(CHUNK, exception_on_overflow=False)
            frames.append(pcm)

            level = rms16(pcm)
            if level >= SILENCE_RMS:
                last_voice = time.time()

            if time.time() - start > MAX_RECORD_SECONDS:
                log.info("⏱️ Max record time reached.")
                break

            if time.time() - last_voice > SILENCE_SECONDS_TO_STOP and (time.time() - start) > 0.8:
                break

        if not frames:
            return None

        wav_bytes = frames_to_wav(frames, RATE, CHANNELS)
        log.info("✅ Recorded %.2fs (%d bytes wav)", (time.time() - start), len(wav_bytes))
        return wav_bytes
    finally:
        stream.stop_stream()
        stream.close()
        pa.terminate()


async def ws_roundtrip(url: str):
    # Cloudflare provides https URL; you must use wss:// for WS over TLS
    log.info("Connecting: %s", url)

    async with websockets.connect(
        url,
        max_size=50 * 1024 * 1024,
        ping_interval=20,
        ping_timeout=20,
    ) as ws:
        log.info("✅ Connected. Waiting server greeting...")
        # Server sends initial status after connect
        try:
            msg = await asyncio.wait_for(ws.recv(), timeout=10)
            log.info("Server: %s", msg)
        except Exception:
            pass

        while True:
            wav_in = record_vad()
            if not wav_in:
                log.info("No audio captured. Retry...")
                continue

            payload = {
                "type": "audio",
                "audio": base64.b64encode(wav_in).decode("ascii"),
            }
            await ws.send(json.dumps(payload))
            log.info("➡️ Sent audio")

            # Receive messages until we get response audio
            reply_audio = None
            while True:
                raw = await ws.recv()
                msg = json.loads(raw)

                t = msg.get("type")
                if t == "status":
                    stage = msg.get("stage", "")
                    log.info("ℹ️ %s: %s", stage, msg.get("message"))
                    if stage == "done":
                        break
                elif t == "transcript":
                    log.info("📝 You: %s", msg.get("text"))
                elif t == "llm_reply":
                    log.info("🤖 AI: %s", msg.get("text"))
                elif t == "audio":
                    b64 = msg.get("audio", "")
                    reply_audio = base64.b64decode(b64) if b64 else None
                    log.info("⬅️ Got audio (%s bytes)", msg.get("size"))
                    break
                elif t == "error":
                    log.error("Server error: %s", msg.get("message"))
                    break
                elif t == "pong":
                    pass
                else:
                    log.info("Unknown msg: %s", msg)

            if reply_audio:
                log.info("🔊 Playing reply...")
                # Save debug file
                with open("reply.wav", "wb") as f:
                    f.write(reply_audio)
                play_wav_bytes(reply_audio)
            else:
                log.warning("No reply audio received. Trying again...")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True, help="WebSocket URL, e.g. wss://xxxx.trycloudflare.com")
    args = ap.parse_args()

    # quick hint for users entering https instead of wss
    if args.url.startswith("https://"):
        args.url = "wss://" + args.url[len("https://") :]
    if args.url.startswith("http://"):
        args.url = "ws://" + args.url[len("http://") :]

    asyncio.run(ws_roundtrip(args.url))


if __name__ == "__main__":
    main()