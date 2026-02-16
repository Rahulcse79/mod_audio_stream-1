import sys
import ssl
import time
import threading
import sounddevice as sd
from websocket import create_connection, WebSocketBadStatusException, ABNF

WS_URL = "wss://eggs-profits-chronicle-small.trycloudflare.com/ws"

def main():
	stop_event = threading.Event()

	# Try to connect with basic SSL options (disable verification if required for testing).
	try:
		ws = create_connection(
			WS_URL,
			sslopt={
				"cert_reqs": ssl.CERT_NONE,
				"check_hostname": False,
			},
		)
	except WebSocketBadStatusException as e:
		print("WebSocket handshake failed:", e)
		print("This is a server-side error (HTTP/WS handshake returned non-101).")
		sys.exit(1)
	except Exception as e:
		print("Failed to connect to WebSocket:", e)
		sys.exit(1)

	print("✅ Connected to server")

	# Define audio callback that sends binary frames to the websocket
	def audio_callback(indata, frames, time_info, status):
		if status:
			print("InputStream status:", status)
		try:
			ws.send(indata.tobytes(), opcode=ABNF.OPCODE_BINARY)
		except Exception as e:
			print("Send error:", e)
			stop_event.set()

	print("🎤 Speak now, press ENTER to stop recording...")

	try:
		with sd.InputStream(samplerate=16000, channels=1, dtype="int16", callback=audio_callback):
			# block until user presses Enter
			try:
				input()
			except KeyboardInterrupt:
				pass
	except Exception as e:
		print("Audio input error:", e)
		sys.exit(1)

	# --- Send STOP signal (text) so server knows audio is done ---
	print("⏹  Stopped recording. Sending STOP signal...")
	try:
		ws.send("STOP", opcode=ABNF.OPCODE_TEXT)
	except Exception as e:
		print("Error sending STOP signal:", e)
		sys.exit(1)

	# --- Wait for AI response ---
	print("⏳ Waiting for AI response...")
	try:
		ws.settimeout(120)  # allow up to 2 minutes for STT + LLM processing
		response = ws.recv()
		if response:
			print("\n🤖 AI response:")
			print(response)
		else:
			print("⚠️ Empty response from server")
	except Exception as e:
		print("Error receiving response:", e)

	# --- Clean up ---
	try:
		ws.close()
	except Exception:
		pass

	print("✅ Client stopped.")


if __name__ == "__main__":
	main()

