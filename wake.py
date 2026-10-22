import sounddevice as sd
import numpy as np
import subprocess
from openwakeword.model import Model

# Load model (built-in or custom)
model = Model(inference_framework="onnx")

SAMPLE_RATE = 16000
CHUNK = 1024

def wait_for_wake():
    print("Listening for wake word...")

    with sd.InputStream(
        samplerate=SAMPLE_RATE,
        channels=1,
        dtype="int16"
    ) as stream:

        while True:
            audio, _ = stream.read(CHUNK)
            audio = audio.flatten().astype(np.float32) / 32768.0

            prediction = model.predict(audio)

            # built-in example wakewords
            for key, score in prediction.items():
                if score > 0.5:
                    print(f"Wake word detected: {key}")
                    return

def main():
    while True:
        wait_for_wake()
        subprocess.run(["python", "voice.py"])

if __name__ == "__main__":
    main()