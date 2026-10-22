import requests
import sys

if len(sys.argv) < 2:
    print("Usage: python test_wav.py <path_to_wav_file>")
    print("Example: python test_wav.py test.wav")
    sys.exit(1)

wav_path = sys.argv[1]

with open(wav_path, "rb") as f:
    audio_data = f.read()

print(f"Sending {wav_path} ({len(audio_data)} bytes) to voice server...")

r = requests.post(
    "http://localhost:5000/voice",
    data=audio_data,
    headers={"Content-Type": "audio/wav"},
    timeout=120
)

print(f"Status: {r.status_code}")
print(f"Reply: {r.text}")
