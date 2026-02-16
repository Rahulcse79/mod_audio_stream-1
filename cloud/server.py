%%writefile server.py
import numpy as np
import whisper
import torch
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from transformers import AutoTokenizer, AutoModelForCausalLM

app = FastAPI()

print("🔁 Loading STT model...")
stt = whisper.load_model("base")

print("🔁 Loading LLM model...")
tokenizer = AutoTokenizer.from_pretrained("microsoft/phi-2")
tokenizer.pad_token = tokenizer.eos_token

llm = AutoModelForCausalLM.from_pretrained(
    "microsoft/phi-2",
    device_map="auto",
    torch_dtype=torch.float16
)
llm.config.pad_token_id = tokenizer.pad_token_id

print("✅ Models loaded successfully")

@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket):
    await ws.accept()
    print("🔌 WebSocket connected")
    audio_chunks = []

    stop_received = False
    try:
        while True:
            msg = await ws.receive()
            msg_type = msg.get("type", "")

            # Client disconnected before sending STOP
            if msg_type == "websocket.disconnect":
                print("🔌 Client disconnected before sending STOP")
                return

            # Client sent STOP text signal — done recording
            if msg_type == "websocket.receive" and msg.get("text") == "STOP":
                print("📩 Received STOP signal from client")
                stop_received = True
                break

            # Binary audio data
            if msg_type == "websocket.receive" and msg.get("bytes"):
                data = msg["bytes"]
                audio = np.frombuffer(data, dtype=np.int16).astype(np.float32) / 32768.0
                audio_chunks.append(audio)

    except WebSocketDisconnect:
        print("🔌 Client disconnected unexpectedly")
        return
    except Exception as e:
        print(f"❌ Error receiving data: {e}")
        return

    if not stop_received:
        return

    # --- Process audio after STOP signal (connection is still open) ---

    if not audio_chunks:
        print("⚠️ No audio received")
        await ws.send_text("ERROR: No audio received")
        await ws.close()
        return

    audio = np.concatenate(audio_chunks)
    print(f"🎧 Received audio samples: {len(audio)}")

    # ---------- STT ----------
    result = stt.transcribe(
        audio,
        fp16=torch.cuda.is_available()
    )
    user_text = result.get("text", "").strip()

    if not user_text:
        print("⚠️ Empty transcription")
        await ws.send_text("ERROR: Could not transcribe audio")
        await ws.close()
        return

    print("🗣 User:", user_text)

    # ---------- LLM ----------
    prompt = f"User: {user_text}\nAI:"
    inputs = tokenizer(prompt, return_tensors="pt").to(llm.device)

    output = llm.generate(
        **inputs,
        max_new_tokens=150,
        temperature=0.7,
        top_p=0.9,
        repetition_penalty=1.1
    )

    ai_text = tokenizer.decode(output[0], skip_special_tokens=True)
    ai_text = ai_text.split("AI:")[-1].strip()

    print("🤖 AI:", ai_text)

    # Send response back (connection is still open!)
    try:
        await ws.send_text(ai_text)
        await ws.close()
        print("✅ Response sent, connection closed")
    except Exception as e:
        print(f"⚠️ Could not send response (client may have disconnected): {e}")
