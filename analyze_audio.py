import wave
import numpy as np
from openwakeword.model import Model

def analyze():
    # 1. Read WAV file metadata
    with wave.open("stream_test.wav", "rb") as wav:
        params = wav.getparams()
        print(f"--- WAV Metadata ---")
        print(f"Channels: {params.nchannels}")
        print(f"Sample Width (bytes): {params.sampwidth}")
        print(f"Frame Rate (Hz): {params.framerate}")
        print(f"Number of Frames: {params.nframes}")
        
        # Read raw bytes
        raw_bytes = wav.readframes(params.nframes)
        
    # 2. Convert to numpy array
    samples_16 = np.frombuffer(raw_bytes, dtype=np.int16)
    print(f"\n--- Numeric Analysis ---")
    print(f"Total 16-bit samples: {len(samples_16)}")
    print(f"Min sample value: {np.min(samples_16)}")
    print(f"Max sample value: {np.max(samples_16)}")
    print(f"Mean sample value: {np.mean(samples_16):.4f}")
    
    # Calculate RMS
    float_samples = samples_16.astype(np.float32) / 32768.0
    rms = np.sqrt(np.mean(float_samples**2))
    print(f"RMS Volume: {rms:.6f}")
    
    # Print a few samples to see if they're constant or zero or look like actual data
    print(f"First 20 samples: {samples_16[:20]}")
    
    # Check for silence/zeros
    zero_count = np.sum(samples_16 == 0)
    print(f"Zero sample count: {zero_count} ({zero_count / len(samples_16) * 100:.2f}%)")
    
    # 3. Run through openwakeword
    print(f"\n--- running openwakeword locally ---")
    oww_model = Model(inference_framework="onnx")
    oww_model.reset()
    
    chunk_size = 1280  # samples
    max_scores = {}
    
    # Step through in 1280 sample windows (which is 2560 bytes)
    for i in range(0, len(float_samples) - chunk_size, chunk_size):
        chunk = float_samples[i:i+chunk_size]
        # Apply 6x digital gain boost like in voice.py
        boosted = np.clip(chunk * 6.0, -1.0, 1.0)
        prediction = oww_model.predict(boosted)
        for w, s in prediction.items():
            max_scores[w] = max(max_scores.get(w, 0.0), float(s))
            
    print(f"Peak Scores detected in stream_test.wav:")
    for w, s in max_scores.items():
        if s > 0.01:
            print(f"  {w}: {s:.4f}")
        else:
            print(f"  {w}: {s:.4f} (very low/none)")

if __name__ == "__main__":
    analyze()
