#!/usr/bin/env python3
"""
TTS-Engine Bridge — WebSocket Server (runs on Google Colab)

Receives raw microphone audio from the client over WebSocket,
runs the full STT → LLM → TTS pipeline, and sends back audio.

Protocol:
  Client → Server:  { "type": "audio", "audio": "<base64 WAV>" }
  Client → Server:  { "type": "ping" }
  Server → Client:  { "type": "status", "stage": "...", "message": "..." }
  Server → Client:  { "type": "transcript", "text": "..." }
  Server → Client:  { "type": "llm_reply", "text": "..." }
  Server → Client:  { "type": "audio", "audio": "<base64 WAV>", "size": N }
  Server → Client:  { "type": "error", "message": "..." }
  Server → Client:  { "type": "pong" }

Usage (Colab):
  1. Install deps:  !pip install websockets
  2. Run this file in a cell (see bottom for Colab launcher)
  3. Expose via cloudflared or ngrok
"""

import asyncio
import base64
import json
import logging
import os
import re
import shutil
import subprocess
import tempfile
import threading
import time
import signal
import sys

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
HOST = "0.0.0.0"
PORT = 8765

PIPER_BIN = "./tools/piper/piper"
VOICE_MODEL = "./models/voices/en_US-lessac-medium.onnx"
OLLAMA_MODEL = "llama3:8b"
LLM_TIMEOUT = 120
MAX_REPLY_CHARS = 800  # Limit TTS input to keep audio short

WHISPER_MODEL_SIZE = "small"  # "tiny", "base", "small", "medium", "large"

# Cloudflare tunnel — auto-launch when running on Colab
AUTO_TUNNEL = True  # Set False to disable

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("bridge-server")

# ---------------------------------------------------------------------------
# Lazy-loaded globals
# ---------------------------------------------------------------------------
_whisper_model = None


def get_whisper_model():
    """Load Whisper model once, reuse across requests."""
    global _whisper_model
    if _whisper_model is None:
        import whisper

        log.info("Loading Whisper model '%s' (first request)...", WHISPER_MODEL_SIZE)
        _whisper_model = whisper.load_model(WHISPER_MODEL_SIZE)
        log.info("Whisper model loaded.")
    return _whisper_model


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def clean_ansi(text: str) -> str:
    """Remove ANSI escape codes from Ollama output."""
    return re.compile(r"\x1b\[[0-9;]*[a-zA-Z]|\x1b\].*?\x07").sub("", text)


def ensure_ollama_running(max_wait: int = 30):
    """Make sure Ollama server is running. Start it if not."""
    try:
        subprocess.run(
            ["ollama", "list"],
            capture_output=True, text=True, timeout=5, check=True,
        )
        log.info("Ollama is already running.")
        return
    except (subprocess.CalledProcessError, FileNotFoundError, subprocess.TimeoutExpired):
        pass

    log.info("Starting Ollama server in background...")
    proc = subprocess.Popen(
        ["ollama", "serve"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    for i in range(max_wait):
        try:
            r = subprocess.run(
                ["ollama", "list"],
                capture_output=True, text=True, timeout=5,
            )
            if r.returncode == 0:
                log.info("Ollama ready (took %ds).", i + 1)
                return
        except (subprocess.TimeoutExpired, FileNotFoundError):
            pass
        time.sleep(1)

    proc.kill()
    raise RuntimeError(f"Ollama failed to start after {max_wait}s")


# ---------------------------------------------------------------------------
# Cloudflare tunnel
# ---------------------------------------------------------------------------
_tunnel_proc = None


def start_cloudflared(port: int) -> subprocess.Popen:
    """Launch cloudflared tunnel and print the public URL to console."""
    global _tunnel_proc

    cf_bin = shutil.which("cloudflared")
    if not cf_bin:
        log.warning("cloudflared not found in PATH — skipping tunnel.")
        log.info("Install it:  https://developers.cloudflare.com/cloudflare-one/connections/connect-networks/downloads/")
        return None

    log.info("Launching cloudflared tunnel → http://localhost:%d ...", port)

    _tunnel_proc = subprocess.Popen(
        [cf_bin, "tunnel", "--url", f"http://localhost:{port}"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,  # cloudflared logs to stderr; merge into stdout
    )

    url_found = threading.Event()

    def _tail():
        """Read cloudflared output, detect the public URL, log everything."""
        for raw_line in iter(_tunnel_proc.stdout.readline, b""):
            line = raw_line.decode("utf-8", errors="replace").rstrip()
            # cloudflared prints: "... https://xxx.trycloudflare.com ..."
            match = re.search(r"https://[a-zA-Z0-9_-]+\.trycloudflare\.com", line)
            if match and not url_found.is_set():
                public_url = match.group(0)
                ws_url = public_url.replace("https://", "wss://")
                log.info("=" * 60)
                log.info("🌐 TUNNEL URL:  %s", public_url)
                log.info("🔌 CLIENT CMD:  python3 client.py --url %s", ws_url)
                log.info("=" * 60)
                url_found.set()
            else:
                log.debug("[cloudflared] %s", line)

    t = threading.Thread(target=_tail, daemon=True)
    t.start()

    # Wait up to 15s for URL to appear
    if url_found.wait(timeout=15):
        log.info("Cloudflare tunnel is live.")
    else:
        log.warning("cloudflared started but tunnel URL not detected yet. Check output.")

    return _tunnel_proc


def stop_cloudflared():
    """Kill the tunnel process if running."""
    global _tunnel_proc
    if _tunnel_proc and _tunnel_proc.poll() is None:
        log.info("Stopping cloudflared tunnel...")
        _tunnel_proc.terminate()
        try:
            _tunnel_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            _tunnel_proc.kill()
        _tunnel_proc = None


# ---------------------------------------------------------------------------
# Pipeline stages
# ---------------------------------------------------------------------------
async def stt(audio_bytes: bytes, ws) -> str:
    """Speech-to-Text via Whisper."""
    await send_status(ws, "stt", "Transcribing your audio...")

    # Write incoming audio to a temp file
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
        f.write(audio_bytes)
        tmp_path = f.name

    try:
        model = get_whisper_model()
        result = model.transcribe(tmp_path)
        text = result["text"].strip()
        log.info("STT result: %s", text)
        return text
    finally:
        os.unlink(tmp_path)


async def llm(user_text: str, ws) -> str:
    """Generate LLM response via Ollama."""
    await send_status(ws, "llm", "Thinking...")

    prompt = f"Reply concisely in 2-3 sentences: {user_text}"

    proc = await asyncio.create_subprocess_exec(
        "ollama", "run", OLLAMA_MODEL, prompt,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )

    try:
        stdout, stderr = await asyncio.wait_for(
            proc.communicate(), timeout=LLM_TIMEOUT
        )
    except asyncio.TimeoutError:
        proc.kill()
        raise RuntimeError(f"LLM timed out after {LLM_TIMEOUT}s")

    if proc.returncode != 0:
        raise RuntimeError(f"Ollama error: {stderr.decode().strip()}")

    reply = clean_ansi(stdout.decode()).strip()
    if not reply:
        raise RuntimeError("LLM returned empty response")

    # Truncate to keep TTS audio short
    reply = reply[:MAX_REPLY_CHARS]
    log.info("LLM reply: %s", reply[:100] + ("..." if len(reply) > 100 else ""))
    return reply


async def tts(text: str, ws) -> bytes:
    """Text-to-Speech via Piper."""
    await send_status(ws, "tts", "Generating speech...")

    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
        out_path = f.name

    try:
        proc = await asyncio.create_subprocess_exec(
            PIPER_BIN, "--model", VOICE_MODEL, "--output_file", out_path,
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        stdout, stderr = await asyncio.wait_for(
            proc.communicate(input=text.encode()), timeout=60
        )

        if proc.returncode != 0:
            raise RuntimeError(f"Piper failed: {stderr.decode().strip()}")

        if not os.path.isfile(out_path):
            raise RuntimeError("Piper did not produce output file")

        with open(out_path, "rb") as f:
            audio_bytes = f.read()

        log.info("TTS output: %d bytes", len(audio_bytes))
        return audio_bytes
    finally:
        if os.path.exists(out_path):
            os.unlink(out_path)


# ---------------------------------------------------------------------------
# WebSocket message helpers
# ---------------------------------------------------------------------------
async def send_status(ws, stage: str, message: str):
    """Send a status update to the client."""
    await ws.send(json.dumps({
        "type": "status",
        "stage": stage,
        "message": message,
    }))


async def send_error(ws, message: str):
    """Send an error to the client."""
    await ws.send(json.dumps({
        "type": "error",
        "message": message,
    }))


# ---------------------------------------------------------------------------
# Main handler
# ---------------------------------------------------------------------------
async def handle_client(ws):
    """Handle a single WebSocket client connection."""
    import websockets

    remote = ws.remote_address
    log.info("Client connected: %s", remote)
    await send_status(ws, "connected", "Connected to TTS-Engine Bridge. Send audio to begin.")

    try:
        async for raw_message in ws:
            try:
                msg = json.loads(raw_message)
            except json.JSONDecodeError:
                await send_error(ws, "Invalid JSON")
                continue

            msg_type = msg.get("type", "")

            if msg_type == "ping":
                await ws.send(json.dumps({"type": "pong"}))
                continue

            if msg_type != "audio":
                await send_error(ws, f"Unknown message type: {msg_type}")
                continue

            # --- Decode audio ---
            audio_b64 = msg.get("audio", "")
            if not audio_b64:
                await send_error(ws, "Missing 'audio' field")
                continue

            try:
                audio_bytes = base64.b64decode(audio_b64)
            except Exception:
                await send_error(ws, "Invalid base64 audio data")
                continue

            log.info("Received audio: %d bytes from %s", len(audio_bytes), remote)

            # --- Run pipeline ---
            pipeline_start = time.time()

            # 1. STT
            user_text = await stt(audio_bytes, ws)
            if not user_text:
                await send_error(ws, "Could not transcribe audio. Please try again.")
                continue

            await ws.send(json.dumps({
                "type": "transcript",
                "text": user_text,
            }))

            # 2. LLM
            reply_text = await llm(user_text, ws)
            await ws.send(json.dumps({
                "type": "llm_reply",
                "text": reply_text,
            }))

            # 3. TTS
            audio_out = await tts(reply_text, ws)

            # 4. Send audio back
            audio_b64_out = base64.b64encode(audio_out).decode("ascii")
            await ws.send(json.dumps({
                "type": "audio",
                "audio": audio_b64_out,
                "size": len(audio_out),
                "format": "wav",
            }))

            elapsed = time.time() - pipeline_start
            await send_status(ws, "done", f"Pipeline complete in {elapsed:.1f}s")
            log.info("Pipeline complete: %.1fs", elapsed)

    except Exception as e:
        log.exception("Error handling client %s", remote)
        try:
            await send_error(ws, str(e))
        except Exception:
            pass
    finally:
        log.info("Client disconnected: %s", remote)


# ---------------------------------------------------------------------------
# Server startup
# ---------------------------------------------------------------------------
async def main():
    import websockets

    # Pre-flight checks
    log.info("=== TTS-Engine Bridge Server ===")

    # Kill any previous server still holding the port
    try:
        result = subprocess.run(
            ["fuser", f"{PORT}/tcp"],
            capture_output=True, text=True, timeout=5,
        )
        pids = result.stdout.strip().split()
        for pid in pids:
            pid = pid.strip()
            if pid.isdigit():
                log.info("Killing old process on port %d (PID %s)...", PORT, pid)
                os.kill(int(pid), signal.SIGKILL)
                time.sleep(0.5)
    except (FileNotFoundError, subprocess.TimeoutExpired, Exception):
        pass  # fuser may not exist on all systems; that's fine

    missing = []
    if not os.path.isfile(PIPER_BIN):
        missing.append(f"Piper binary: {PIPER_BIN}")
    if not os.path.isfile(VOICE_MODEL):
        missing.append(f"Voice model: {VOICE_MODEL}")
    if missing:
        for m in missing:
            log.error("Missing: %s", m)
        sys.exit(1)

    # Ensure Ollama is running
    ensure_ollama_running()

    # Pre-load Whisper (so first request isn't slow)
    get_whisper_model()

    log.info("Starting WebSocket server on ws://%s:%d", HOST, PORT)

    stop = asyncio.get_event_loop().create_future()

    async with websockets.serve(
        handle_client,
        HOST,
        PORT,
        max_size=50 * 1024 * 1024,  # 50 MB max message (for large audio)
        ping_interval=30,
        ping_timeout=10,
    ):
        log.info("✅ Server ready — ws://%s:%d", HOST, PORT)

        # Auto-launch cloudflared tunnel
        if AUTO_TUNNEL:
            start_cloudflared(PORT)
        else:
            log.info("Expose with:  cloudflared tunnel --url http://localhost:%d", PORT)

        try:
            await stop  # Run forever
        finally:
            stop_cloudflared()


def run():
    """Entry point — works in both regular Python and Colab/Jupyter."""
    try:
        loop = asyncio.get_running_loop()
    except RuntimeError:
        loop = None

    if loop and loop.is_running():
        # Inside Jupyter/Colab — nest the event loop
        import nest_asyncio
        nest_asyncio.apply()
        loop.run_until_complete(main())
    else:
        asyncio.run(main())


# ---------------------------------------------------------------------------
# Direct execution
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    run()


# ═══════════════════════════════════════════════════════════════════════════
# COLAB USAGE — paste these in cells:
# ═══════════════════════════════════════════════════════════════════════════
#
# # Cell 1: Install deps
# !pip install websockets nest_asyncio openai-whisper
# !wget -q https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-amd64 -O /usr/local/bin/cloudflared && chmod +x /usr/local/bin/cloudflared
#
# # Cell 2: Start server + tunnel (all-in-one, runs forever)
# !python server.py
#
# The server will:
#   1. Check Piper binary + voice model
#   2. Start Ollama if needed
#   3. Load Whisper model
#   4. Start WebSocket server on port 8765
#   5. Auto-launch cloudflared tunnel and print the wss:// URL
#
# Then on your laptop run:
#   python3 client.py --url wss://xxxxx.trycloudflare.com
#
# ═══════════════════════════════════════════════════════════════════════════
