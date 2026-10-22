# Voice to LLM

A self-hosted, offline voice assistant running on an ESP32 microcontroller + a local PC server. Say a wake word or press a button, speak a command, and hear a synthesized response — all without any cloud services. Link to more hardware specification information: https://drive.google.com/drive/folders/10N4LTek3beh6XN6BWK4eYBNzVOlwYM5R

---

## How It Works

```
[INMP441 Mic] → [ESP32] → [Wi-Fi TCP/HTTP] → [PC Server: voice.py]
                                                    │
                                        ┌───────────┼───────────┐
                                   [Whisper STT] [Gemma LLM] [Piper TTS]
                                                    │
                    [MAX98357A Speaker] ← [ESP32] ← [WAV Audio Response]
```

1. **ESP32** captures audio from the INMP441 I2S microphone
2. Audio is streamed to the PC server over Wi-Fi
3. **Faster-Whisper** (`whisper_online`) transcribes the speech to text
4. **Gemma 4** (via Ollama) generates a response with 4-turn conversation memory
5. **Piper TTS** synthesizes the reply into speech
6. The WAV audio is sent back to the ESP32 and played through the MAX98357A amplifier

Smart home commands (lights, etc.) are intercepted before the LLM and handled via the **Home Assistant** REST API.

---

## Hardware

| Component | Part | Notes |
|---|---|---|
| Microcontroller | ESP32 DevKit | Dual-core 240MHz, Wi-Fi built-in |
| Microphone | INMP441 | I2S digital mic, L/R pin tied to GND (Left channel) |
| Speaker Amp | MAX98357A | I2S Class-D amplifier, 3W mono |
| Button | Momentary pushbutton | GPIO 5, active LOW with internal pull-up |

### Wiring

**INMP441 Microphone → ESP32**
| INMP441 | ESP32 GPIO |
|---|---|
| VDD | 3.3V |
| GND | GND |
| SD (Data) | 15 |
| SCK (Clock) | 2 |
| WS (Word Select) | 4 |
| L/R | GND ← tied to ground (Left channel) |

**MAX98357A Amplifier → ESP32**
| MAX98357A | ESP32 GPIO |
|---|---|
| DIN | 14 |
| BCLK | 27 |
| LRC | 26 |
| SD (Mute) | 13 |
| VIN | 5V |
| GND | GND |

**Push Button**
| Button | ESP32 GPIO |
|---|---|
| Pin A | 5 |
| Pin B | GND |

---

## PC Server Setup

### Prerequisites

- Python 3.10+
- [Ollama](https://ollama.com/) with `gemma4:e2b` pulled
- [faster-whisper-server](https://github.com/fedirz/faster-whisper-server) running on port 9000
- [Piper TTS](https://github.com/rhasspy/piper) server running on port 10200
- [Home Assistant](https://www.home-assistant.io/) (optional, for smart home commands)

### Install Python dependencies

```bash
python -m venv .venv
.venv\Scripts\activate       # Windows
pip install flask requests numpy openwakeword
```

### Configure `voice.py`

Edit the top of `voice.py` to match your setup:

```python
MODEL        = "gemma4:e2b"           # Ollama model name
WHISPER_URL  = "http://localhost:9000/asr?..."
PIPER_HOST   = "localhost"
PIPER_PORT   = 10200
HA_URL       = "http://localhost:8123/api"  # Home Assistant URL
HA_TOKEN     = "YOUR_HOME_ASSISTANT_TOKEN"
```

### Start the server

```bash
.venv\Scripts\python voice.py
```

The server starts two listeners:
- **HTTP on port 5000** — `/voice` endpoint for push-to-talk POST requests
- **TCP on port 5001** — streaming socket for the wake-word sketch

---

## ESP32 Sketches

Open any sketch folder in the Arduino IDE. All sketches share the same pin mapping and Wi-Fi credentials (edit at the top of each `.ino`).

### `esp32_push_to_talk/`
**→ Recommended for daily use**

Press the button → hear a beep → speak for 5 seconds → hear the response.

- Records audio then POSTs a WAV file to `http://<server>:5000/voice`
- Uses a persistent `WiFiClient` to avoid heap fragmentation across repeated presses
- Reinstalls the I2S mic driver after each beep to prevent clock drift
- Pre-buffers 4KB of audio before playback begins to prevent stuttering

### `esp32_mic_test/`
**→ Hands-free wake word mode**

Continuously streams audio to the PC over TCP. The server runs `openwakeword` in real time. When "Alexa" or "Hey Jarvis" is detected:
1. ESP32 plays a beep
2. ESP32 records the next 3 seconds
3. Server transcribes, generates response, sends audio back

Requires the TCP server on port 5001 to be running (started automatically by `voice.py`).

### `esp32_mic_diagnostic/`
**→ Hardware debugging only**

No Wi-Fi. No server. Reads raw I2S data and prints per-channel statistics to the Serial Monitor every ~320ms. Use this to:
- Confirm the mic is wired correctly
- Identify which I2S channel (even/odd index) contains signal
- Verify the INMP441 is powered and responding

### `esp32_tone_test/`
**→ Speaker debugging only**

Outputs a continuous 440Hz sine wave through the MAX98357A. Use this to confirm the amplifier and speaker are working before testing the full pipeline.

---

## Python Utilities

| File | Purpose |
|---|---|
| `voice.py` | Main server — Whisper STT, Gemma LLM, Piper TTS, Home Assistant, Flask HTTP + TCP streaming |
| `wake.py` | Standalone PC-only wake word test using the system microphone (not the ESP32) |
| `analyze_audio.py` | Load and analyze a WAV file — prints RMS volume, peak, and plots waveform |
| `test_gain.py` | Test microphone gain levels and sensitivity thresholds |
| `test_wav.py` | Quick WAV file validity checker |

### Debug audio files (auto-generated by server)

| File | Contains |
|---|---|
| `debug.wav` | Last recorded voice command sent to Whisper |
| `debug_tts.wav` | Last TTS response generated by Piper |
| `stream_test.wav` | First 5 seconds of the last TCP mic stream (wake word mode) |

---

## Troubleshooting

### Audio is completely silent / all zeros
- Verify `L/R` pin on INMP441 is tied to **GND** (not floating)
- Check VDD = **3.3V** (not 5V — will damage the mic)
- Flash `esp32_mic_diagnostic` and read the Serial Monitor
- The I2S buffer layout is `[Right, Left, Right, Left, ...]` — signal is at **odd indices**

### Wake word never triggers
- Open Serial Monitor and watch `[Mic Diagnostics] Raw RMS Vol:` — should be > 0.05 when speaking
- Lower the detection threshold in `voice.py`: `if score > 0.3` (currently set)
- Supported wake words: `alexa`, `hey_jarvis`, `hey_mycroft`, `hey_rhasspy`, and timer phrases

### Playback stutters / pauses mid-sentence
- The sketch pre-buffers 4KB before starting — if it still stutters, your LAN may be congested
- Check that `WiFi.setSleep(false)` is present in the sketch (it is — disables modem sleep)

### Audio degrades / becomes static after several prompts
- The sketch uses a global persistent `WiFiClient` to avoid heap fragmentation
- Watch the Serial Monitor for `[Heap after] X bytes free` — should stay stable above ~100KB
- If heap keeps dropping, press reset and it will reconnect cleanly

### ESP32 crashes with "Interrupt WDT timeout"
- Caused by large arrays allocated on the stack (8KB limit)
- All large buffers in the sketches are declared `static` to live in BSS instead

### STT transcription is wrong / garbled
- `setNoDelay(true)` is set on the TCP client — this prevents Nagle's algorithm from stalling I2S DMA reads
- Listen to `debug.wav` on your PC after a bad transcription to confirm whether the mic captured clean audio or noise

---

## Architecture Notes

- **I2S config**: `RIGHT_LEFT` stereo capture at 32-bit/slot, 16kHz. The INMP441 outputs 24-bit audio MSB-aligned in a 32-bit container. Shift right by 14 bits to get 16-bit PCM.
- **LLM context**: 4-turn rolling conversation window stored in `voice.py`. The assistant persona is "Cassian" — calm, direct, 1–2 sentence replies.
- **Home Assistant**: Intent matching is done with simple keyword detection before the LLM. Edit `handle_intent()` in `voice.py` to add your own devices.
- **TTS**: Piper is called via TCP socket. The WAV response is returned directly to the ESP32 without saving to disk (except `debug_tts.wav` for verification).

## Public release configuration

The checked-in Wi-Fi, voice server host, and Home Assistant token values are placeholders. Replace them locally before running or flashing. Loopback addresses in the PC examples refer to services on the same computer. Configure your own Home Assistant entity IDs in `handle_intent()`. Keep real credentials and private network addresses out of commits.

Generated firmware/build directories, Python caches, debug recordings, local environment files, logs, and backups are excluded by `.gitignore`. Debug audio files are generated locally and are not included in this release.
