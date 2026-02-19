
import os
import csv
import requests

# ==========================
# CONFIG
# ==========================
ELEVEN_API_KEY = "sk_ca9d324b19769026c0ba589436422d51d065b9c8ccfd5521"
VOICE_ID = "SIHMomTAOjOPeaiJ43D0"

METADATA_FILE = "/content/piper/dataset/metadata.csv"
OUTPUT_DIR = "/content/piper/dataset/wavs"

MODEL_ID = "eleven_multilingual_v2"
# ==========================

if not ELEVEN_API_KEY:
    raise ValueError("ELEVEN_API_KEY environment variable not set")

os.makedirs(OUTPUT_DIR, exist_ok=True)

headers = {
    "xi-api-key": ELEVEN_API_KEY,
    "Content-Type": "application/json"
}

with open(METADATA_FILE, "r", encoding="utf-8") as f:
    reader = csv.reader(f, delimiter="|")

    for row in reader:
        if len(row) != 2:
            continue

        file_id, text = row
        out_path = os.path.join(OUTPUT_DIR, f"{file_id}.wav")

        print(f"Generating {out_path}")

        payload = {
            "text": text,
            "model_id": MODEL_ID,
            "voice_settings": {
                "stability": 0.5,
                "similarity_boost": 0.75
            }
        }

        url = f"https://api.elevenlabs.io/v1/text-to-speech/{VOICE_ID}"

        response = requests.post(url, json=payload, headers=headers)

        if response.status_code != 200:
            print("ERROR:", response.text)
            continue

        with open(out_path, "wb") as audio_file:
            audio_file.write(response.content)

print("Done generating WAV files.")
