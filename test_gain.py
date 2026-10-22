import wave
import numpy as np
from openwakeword.model import Model

def test_gains():
    with wave.open("stream_test.wav", "rb") as wav:
        params = wav.getparams()
        raw_bytes = wav.readframes(params.nframes)
        
    samples_16 = np.frombuffer(raw_bytes, dtype=np.int16)
    float_samples = samples_16.astype(np.float32) / 32768.0
    
    print("Testing different digital gains on stream_test.wav...")
    oww_model = Model(inference_framework="onnx")
    chunk_size = 1280
    
    for gain in [1.0, 6.0, 12.0, 24.0, 48.0, 96.0]:
        oww_model.reset()
        max_scores = {}
        for i in range(0, len(float_samples) - chunk_size, chunk_size):
            chunk = float_samples[i:i+chunk_size]
            boosted = np.clip(chunk * gain, -1.0, 1.0)
            prediction = oww_model.predict(boosted)
            for w, s in prediction.items():
                max_scores[w] = max(max_scores.get(w, 0.0), float(s))
        
        print(f"\nGain: {gain}x")
        # Print top 3 wake words
        sorted_scores = sorted(max_scores.items(), key=lambda x: x[1], reverse=True)
        for w, s in sorted_scores[:3]:
            print(f"  {w}: {s:.4f}")

if __name__ == "__main__":
    test_gains()
