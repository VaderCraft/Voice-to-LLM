
import requests
import tempfile
import os
import struct
import asyncio
import io
import socket
import threading
from flask import Flask, request, Response

app = Flask(__name__)

conversation = []
MAX_TURNS = 4

WHISPER_URL = "http://localhost:9000/asr?encode=true&task=transcribe&language=en"
OLLAMA_URL = "http://localhost:11434/api/generate"
MODEL = "gemma4:e2b"

PIPER_HOST = "localhost"
PIPER_PORT = 10200

HA_URL = "http://localhost:8123/api"
HA_TOKEN = "YOUR_HOME_ASSISTANT_TOKEN"

def ha_call(domain, service, data):
    headers = {
        "Authorization": f"Bearer {HA_TOKEN}",
        "Content-Type": "application/json"
    }
    url = f"{HA_URL}/services/{domain}/{service}"
    requests.post(url, headers=headers, json=data)

def transcribe(filename):
    with open(filename, "rb") as f:
        r = requests.post(WHISPER_URL, files={"audio_file": f})
    try:
        data = r.json()
        return data.get("text", "")
    except:
        return r.text.strip()

def generate(prompt):
    global conversation

    # Format the conversation history as chat messages
    messages = [
        {
            "role": "system",
            "content": "You are Cassian, a calm and efficient voice assistant. Rules: Respond in 1–2 sentences. Be direct and confident. No filler or repetition."
        }
    ]
    
    # Add history
    for role, content in conversation:
        messages.append({"role": role, "content": content})
        
    # Add new user prompt
    messages.append({"role": "user", "content": prompt})

    r = requests.post(
        "http://localhost:11434/api/chat",
        json={
            "model": MODEL,
            "messages": messages,
            "stream": False
        },
        timeout=60
    )
    
    reply = r.json().get("message", {}).get("content", "").strip()
    
    # Update conversation history
    conversation.append(("user", prompt))
    conversation.append(("assistant", reply))
    conversation = conversation[-MAX_TURNS:]
    
    return reply

def handle_intent(text):
    t = text.lower()
    if "turn on" in t and "light" in t:
        ha_call("light", "turn_on", {"entity_id": "light.living_room"})
        return "Light turned on"
    if "turn off" in t and "light" in t:
        ha_call("light", "turn_off", {"entity_id": "light.living_room"})
        return "Light turned off"
    return None

def synthesize_speech(text):
    """
    Connects to Piper via the Wyoming protocol over raw TCP.
    Sends a Synthesize request, receives audio chunks, and returns a complete WAV file as bytes.
    """
    import json
    import socket

    audio_chunks = []
    sample_rate = 22050  # Piper's default
    sample_width = 2     # 16-bit
    channels = 1         # mono

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(30)
        sock.connect((PIPER_HOST, PIPER_PORT))

        # Wyoming protocol: send a "synthesize" event
        synth_event = {
            "type": "synthesize",
            "data": {
                "text": text,
            }
        }
        event_json = json.dumps(synth_event)
        # Wyoming wire format: JSON line + newline
        header_line = json.dumps({
            "type": "synthesize",
            "data_length": 0,
            "payload_length": 0
        })
        # Actually, Wyoming protocol uses a specific binary framing.
        # Let's use the wyoming library if available, otherwise fall back.
        sock.close()
    except:
        pass

    # Use the wyoming Python library for proper protocol handling
    try:
        from wyoming.client import AsyncTcpClient
        from wyoming.tts import Synthesize, SynthesizeVoice
        from wyoming.audio import AudioChunk, AudioStart, AudioStop
        from wyoming.event import async_read_event, async_write_event

        async def _synthesize():
            nonlocal audio_chunks, sample_rate, sample_width, channels
            
            reader, writer = await asyncio.open_connection(PIPER_HOST, PIPER_PORT)

            # Send synthesize event
            synth = Synthesize(text=text)
            await async_write_event(synth.event(), writer)

            # Read audio events until we get audio-stop
            while True:
                event = await async_read_event(reader)
                if event is None:
                    break
                
                if AudioStart.is_type(event.type):
                    audio_start = AudioStart.from_event(event)
                    sample_rate = audio_start.rate
                    sample_width = audio_start.width
                    channels = audio_start.channels
                    
                elif AudioChunk.is_type(event.type):
                    chunk = AudioChunk.from_event(event)
                    audio_chunks.append(chunk.audio)
                    
                elif AudioStop.is_type(event.type):
                    break

            writer.close()

        asyncio.run(_synthesize())

    except ImportError:
        print("ERROR: 'wyoming' Python package not installed!")
        print("Install it with: pip install wyoming")
        return None

    if not audio_chunks:
        print("WARNING: Piper returned no audio chunks.")
        return None

    # Combine all audio chunks into one PCM buffer
    pcm_data = b"".join(audio_chunks)
    


    # Build a WAV file in memory
    wav_buf = io.BytesIO()
    data_size = len(pcm_data)
    file_size = data_size + 36

    # WAV header (44 bytes)
    wav_buf.write(b"RIFF")
    wav_buf.write(struct.pack("<I", file_size))
    wav_buf.write(b"WAVEfmt ")
    wav_buf.write(struct.pack("<I", 16))                    # chunk size
    wav_buf.write(struct.pack("<H", 1))                     # PCM format
    wav_buf.write(struct.pack("<H", channels))              # channels
    wav_buf.write(struct.pack("<I", sample_rate))            # sample rate
    wav_buf.write(struct.pack("<I", sample_rate * channels * sample_width))  # byte rate
    wav_buf.write(struct.pack("<H", channels * sample_width))  # block align
    wav_buf.write(struct.pack("<H", sample_width * 8))      # bits per sample
    wav_buf.write(b"data")
    wav_buf.write(struct.pack("<I", data_size))
    wav_buf.write(pcm_data)
    
    print(f"Piper TTS: {len(pcm_data)} bytes of audio, {sample_rate}Hz, {sample_width*8}-bit, {channels}ch")
    
    return wav_buf.getvalue()

@app.route("/voice", methods=["POST"])
def voice_endpoint():
    audio_data = request.data
    
    # Save the incoming WAV file to a debug file we can listen to
    audio_file = "debug.wav"
    with open(audio_file, "wb") as f:
        f.write(audio_data)
        
    print(f"\n--- NEW REQUEST ---")
    print(f"Received {len(audio_data)} bytes of audio from ESP32.")
        
    # Transcribe the audio
    print("Sending to Whisper...")
    text = transcribe(audio_file)
    print(f"Whisper raw output: '{text}'")
    
    if not text.strip() or len(text.strip()) < 2:
        print("Audio was silent or invalid. Skipping.")
        return ""
        
    print("You:", text)
    
    # Check for smart home commands first
    intent_result = handle_intent(text)
    if intent_result:
        print("Action:", intent_result)
        reply_text = intent_result
    else:
        # Otherwise, pass to the LLM
        print("Sending to Ollama...")
        reply_text = generate(text)
        print("AI:", reply_text)
    
    # Synthesize speech with Piper
    print("Sending to Piper TTS...")
    wav_data = synthesize_speech(reply_text)
    
    if wav_data:
        with open("debug_tts.wav", "wb") as f:
            f.write(wav_data)
        print("Saved TTS output to debug_tts.wav")
            
        print(f"Returning {len(wav_data)} bytes of TTS audio to ESP32.")
        return Response(wav_data, mimetype="audio/wav")
    else:
        print("TTS failed, returning text only.")
        return reply_text

# ==========================================
# ===== BACKGROUND TCP STREAMING SERVER =====
# ==========================================
from openwakeword.model import Model
import numpy as np

oww_model = None
active_conn = None

def tcp_streaming_server():
    global oww_model, active_conn
    print("Loading openwakeword model...")
    oww_model = Model(inference_framework="onnx")
    print("openwakeword model loaded successfully!")

    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_sock.bind(("0.0.0.0", 0000))
    server_sock.listen(5)
    print("TCP Audio Streaming Server is listening on port 0000...")

    while True:
        try:
            conn, addr = server_sock.accept()
            print(f"\nESP32 Client connected from {addr[0]}")
            
            # Close previous connection if active to allow clean takeover
            if active_conn is not None:
                try:
                    active_conn.close()
                    print("Closed previous orphaned connection.")
                except:
                    pass
            
            active_conn = conn
            
            # Spawn a thread to handle the stream so that accept() remains non-blocking
            client_thread = threading.Thread(target=handle_esp32_stream, args=(conn,), daemon=True)
            client_thread.start()
        except Exception as e:
            print(f"TCP server error: {e}")

def handle_esp32_stream(conn):
    global oww_model, active_conn
    oww_model.reset() # Reset state
    
    pcm_buffer = b""
    chunk_size = 2560 # 1280 samples * 2 bytes
    
    print("Streaming audio started...")
    print(f"Loaded wake-word models: {list(oww_model.models.keys())}")
    
    save_test_stream = True
    test_stream_bytes = b""
    test_stream_target = 16000 * 2 * 5 # 5 seconds
    
    diagnostic_counter = 0
    max_scores = {}
    
    while True:
        try:
            # If a newer connection has taken over, exit this thread
            if active_conn != conn:
                print("Newer connection active. Exiting old stream thread.")
                break
                
            data = conn.recv(1024)
            if not data:
                print("ESP32 disconnected.")
                break
                
            if save_test_stream:
                test_stream_bytes += data
                if len(test_stream_bytes) >= test_stream_target:
                    save_test_stream = False
                    data_size = len(test_stream_bytes)
                    file_size = data_size + 36
                    import struct
                    wav_header = b"RIFF" + \
                                 struct.pack("<I", file_size) + \
                                 b"WAVEfmt " + \
                                 struct.pack("<I", 16) + \
                                 struct.pack("<H", 1) + \
                                 struct.pack("<H", 1) + \
                                 struct.pack("<I", 16000) + \
                                 struct.pack("<I", 32000) + \
                                 struct.pack("<H", 2) + \
                                 struct.pack("<H", 16) + \
                                 b"data" + \
                                 struct.pack("<I", data_size)
                    with open("stream_test.wav", "wb") as f:
                        f.write(wav_header)
                        f.write(test_stream_bytes)
                    print("\n[AUDIT] Saved 5 seconds of raw mic stream to stream_test.wav! Please listen to this file on your PC to check the audio speed and quality.")
                
            pcm_buffer += data
            
            # Process as many chunks as available in buffer
            while len(pcm_buffer) >= chunk_size:
                chunk = pcm_buffer[:chunk_size]
                pcm_buffer = pcm_buffer[chunk_size:]
                
                # Convert to float32 numpy array normalized
                # ESP32 now applies 16x gain on-device (>>10 shift), no server-side boost needed
                samples = np.frombuffer(chunk, dtype=np.int16).astype(np.float32) / 32768.0
                
                # Predict
                prediction = oww_model.predict(samples)
                
                # Track max scores in this window
                for w, s in prediction.items():
                    max_scores[w] = max(max_scores.get(w, 0.0), float(s))
                
                # Print periodic diagnostics (approx every 3-4 seconds)
                diagnostic_counter += 1
                if diagnostic_counter >= 40:
                    diagnostic_counter = 0
                    raw_rms = np.sqrt(np.mean(samples**2))
                    scores_str = ", ".join([f"{w}: {s:.2f}" for w, s in max_scores.items()])
                    max_scores = {} # Reset for next window
                    print(f"[Mic Diagnostics] Raw RMS Vol: {raw_rms:.4f} | Hex Chunks: {chunk[:24].hex()} | Peak Scores -> {scores_str}")
                    
                    if raw_rms < 0.001:
                        print("  [WARNING] Mic volume is near zero! The microphone might be physically disconnected or wired to the wrong pins.")
                    elif raw_rms > 0.6:
                        print("  [WARNING] Mic volume is extremely high! Check for static, a loose wire, or bad ground.")
                
                # Check prediction scores
                wake_word_detected = False
                for wakeword, score in prediction.items():
                    if score > 0.3:  # Lowered from 0.5 to 0.3 for easier activation
                        print(f"\n--- WAKE WORD DETECTED: {wakeword} ({score:.2f}) ---")
                        wake_word_detected = True
                        break
                        
                if wake_word_detected:
                    process_wake_event(conn)
                    # Clear buffer after trigger to prevent echo loop
                    pcm_buffer = b""
                    oww_model.reset()
                    diagnostic_counter = 0
                    break
        except Exception as e:
            print(f"Error handling stream: {e}")
            break
    conn.close()

def process_wake_event(conn):
    # 1. Send trigger byte 0x01 to tell ESP32 to beep
    print("Sending trigger byte 0x01 to ESP32...")
    conn.sendall(b"\x01")
    
    # 2. Collect 3 seconds of audio command
    print("Recording command audio...")
    command_bytes = b""
    target_bytes = 16000 * 2 * 3 # 16kHz, 16-bit mono, 3 seconds = 96,000 bytes
    
    # Set a timeout for safety
    conn.settimeout(5.0)
    
    while len(command_bytes) < target_bytes:
        data = conn.recv(1024)
        if not data:
            print("Connection lost while recording command.")
            return
        command_bytes += data
        
    # Reset timeout
    conn.settimeout(None)
    
    # Apply digital gain boost (4x) to the recorded command audio so Whisper hears it clearly
    try:
        audio_samples = np.frombuffer(command_bytes, dtype=np.int16).astype(np.float32)
        amplified_samples = np.clip(audio_samples * 4.0, -32768, 32767).astype(np.int16)
        command_bytes = amplified_samples.tobytes()
        print("Whisper command audio digital gain applied (4.0x)")
    except Exception as e:
        print(f"Failed to apply digital gain to Whisper command: {e}")
        
    # Save the recorded command to debug.wav with a 44-byte WAV header
    audio_file = "debug.wav"
    
    # WAV header
    data_size = len(command_bytes)
    file_size = data_size + 36
    sample_rate = 16000
    channels = 1
    sample_width = 2
    
    wav_header = b"RIFF" + \
                 struct.pack("<I", file_size) + \
                 b"WAVEfmt " + \
                 struct.pack("<I", 16) + \
                 struct.pack("<H", 1) + \
                 struct.pack("<H", channels) + \
                 struct.pack("<I", sample_rate) + \
                 struct.pack("<I", sample_rate * channels * sample_width) + \
                 struct.pack("<H", channels * sample_width) + \
                 struct.pack("<H", sample_width * 8) + \
                 b"data" + \
                 struct.pack("<I", data_size)
                 
    with open(audio_file, "wb") as f:
        f.write(wav_header)
        f.write(command_bytes)
        
    print("Saved command audio to debug.wav")
    
    # 3. Transcribe speech
    print("Transcribing...")
    text = transcribe(audio_file)
    print(f"You: '{text}'")
    
    if not text.strip() or len(text.strip()) < 2:
        print("Empty command. Canceling.")
        return
        
    # 4. Generate response from LLM
    intent_result = handle_intent(text)
    if intent_result:
        reply_text = intent_result
    else:
        print("Generating response from Gemma 4...")
        reply_text = generate(text)
        print(f"Cassian: '{reply_text}'")
        
    # 5. Synthesize speech
    print("Synthesizing response with Piper...")
    wav_data = synthesize_speech(reply_text)
    
    if wav_data:
        # Save to debug_tts.wav for verification
        with open("debug_tts.wav", "wb") as f:
            f.write(wav_data)
            
        # Send play trigger 0x02, followed by the length of the WAV file, followed by the WAV file itself
        print(f"Sending {len(wav_data)} bytes of audio response to ESP32...")
        conn.sendall(b"\x02")
        conn.sendall(struct.pack("<I", len(wav_data)))
        conn.sendall(wav_data)
        print("Audio response sent successfully!")
    else:
        print("TTS failed.")

if __name__ == "__main__":
    # Start the TCP streaming server in a background thread
    tcp_thread = threading.Thread(target=tcp_streaming_server, daemon=True)
    tcp_thread.start()

    print("Voice Server is running! Waiting for ESP32...")
    app.run(host="0.0.0.0", port=0000)
