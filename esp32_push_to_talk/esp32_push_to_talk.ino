#include <Arduino.h>
#include "driver/i2s.h"
#include <WiFi.h>

// ===== PIN MAPPING =====
#define MIC_SD  15
#define MIC_SCK 2
#define MIC_WS  4
#define SPK_DIN  14
#define SPK_BCLK 27
#define SPK_LRC  26
#define SPK_SD   13
#define BTN_PIN  5

// ===== CONFIG =====
#define MIC_I2S_PORT    I2S_NUM_0
#define SPK_I2S_PORT    I2S_NUM_1
#define MIC_SAMPLE_RATE 16000
#define RECORD_SECONDS  5

const char* ssid       = "YOUR_WIFI_SSID";
const char* password   = "YOUR_WIFI_PASSWORD";
const char* serverHost = "YOUR_VOICE_SERVER_HOST";
const int   serverPort = 0000; //Change as needed

// Global persistent TCP client — allocated once, never freed.
// Avoids heap fragmentation from repeated create/destroy cycles
// which degrades audio quality after several prompts.
WiFiClient client;

// ===== STATIC BUFFERS — all off the stack =====
// Total: 1KB + 256B + 4KB + 1KB = ~6.3KB in BSS, 0 on stack.
static int32_t  dmaBuf[256];         // stereo I2S DMA block (1KB)
static int16_t  pcmBuf[128];         // mono output per DMA block (256B)
static uint8_t  preBuf[4096];        // playback pre-buffer (4KB)
static uint8_t  spkBuf[1024];        // playback streaming buffer (1KB)

// ===== MICROPHONE — full reinstall each time for clean DMA state =====
void reinstallMic() {
  i2s_driver_uninstall(MIC_I2S_PORT); // tear down cleanly

  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = MIC_SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = 256,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins;
  pins.bck_io_num   = MIC_SCK;
  pins.ws_io_num    = MIC_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = MIC_SD;
  #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  #endif

  i2s_driver_install(MIC_I2S_PORT, &cfg, 0, NULL);
  i2s_set_pin(MIC_I2S_PORT, &pins);
  i2s_start(MIC_I2S_PORT);
}

// ===== SPEAKER =====
void setupSpeaker(int sampleRate) {
  i2s_driver_uninstall(SPK_I2S_PORT);

  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate          = sampleRate,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = 512,
    .use_apll             = false,
    .tx_desc_auto_clear   = true,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins;
  pins.bck_io_num   = SPK_BCLK;
  pins.ws_io_num    = SPK_LRC;
  pins.data_out_num = SPK_DIN;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;
  #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  #endif

  i2s_driver_install(SPK_I2S_PORT, &cfg, 0, NULL);
  i2s_set_pin(SPK_I2S_PORT, &pins);
  i2s_zero_dma_buffer(SPK_I2S_PORT);
}

// ===== BEEP =====
void playBeep() {
  // Stop mic cleanly before using the speaker
  i2s_stop(MIC_I2S_PORT);

  setupSpeaker(16000);
  digitalWrite(SPK_SD, HIGH);
  delay(10);

  static int16_t beepBuf[256];
  for (int i = 0; i < 256; i++) {
    beepBuf[i] = (int16_t)(sin(2.0 * PI * 880.0 * i / 16000.0) * 12000.0);
  }
  size_t bw;
  for (int j = 0; j < 8; j++) {
    i2s_write(SPK_I2S_PORT, beepBuf, sizeof(beepBuf), &bw, portMAX_DELAY);
  }
  delay(10);
  i2s_zero_dma_buffer(SPK_I2S_PORT);
  digitalWrite(SPK_SD, LOW);

  // Full reinstall of mic driver for a completely clean DMA state.
  // Simple i2s_start() after repeated stop/start causes I2S clock drift.
  reinstallMic();
}

// ===== WIFI =====
void connectWiFi() {
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected.");
}

// ===== WAV HEADER =====
void buildWavHeader(uint8_t* buf, uint32_t pcmDataSize) {
  uint32_t fileSize   = pcmDataSize + 36;
  uint16_t channels   = 1;
  uint32_t sr         = MIC_SAMPLE_RATE;
  uint16_t bps        = 16;
  uint32_t byteRate   = sr * 2;
  uint16_t blockAlign = 2;
  uint32_t fmtSize    = 16;
  uint16_t audioFmt   = 1;

  memcpy(buf,      "RIFF", 4); memcpy(buf+4,  &fileSize,   4);
  memcpy(buf+8,    "WAVEfmt ", 8);
  memcpy(buf+16,   &fmtSize,   4); memcpy(buf+20, &audioFmt,   2);
  memcpy(buf+22,   &channels,  2); memcpy(buf+24, &sr,         4);
  memcpy(buf+28,   &byteRate,  4); memcpy(buf+32, &blockAlign, 2);
  memcpy(buf+34,   &bps,       2);
  memcpy(buf+36,   "data",     4); memcpy(buf+40, &pcmDataSize,4);
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(SPK_SD, OUTPUT);
  digitalWrite(SPK_SD, LOW);

  Serial.println("Connecting to WiFi...");
  connectWiFi();
  Serial.println("Installing mic driver...");
  reinstallMic();

  Serial.println("=== PUSH-TO-TALK READY ===");
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
}

// ===== MAIN LOOP =====
void loop() {
  if (digitalRead(BTN_PIN) == LOW) {
    delay(50);
    if (digitalRead(BTN_PIN) == LOW) {
      recordAndProcess();
      while (digitalRead(BTN_PIN) == LOW) delay(10);
    }
  }
}

// ===== RECORD → POST → PLAY =====
void recordAndProcess() {
  const int totalSamples = MIC_SAMPLE_RATE * RECORD_SECONDS;
  const int pcmBytes     = totalSamples * 2;
  const int wavBytes     = pcmBytes + 44;

  Serial.printf("[Heap before record] %d bytes free\n", ESP.getFreeHeap());

  // --- Beep (mic is reinstalled fresh inside playBeep) ---
  playBeep();
  Serial.println(">> RECORDING 5 seconds...");

  // --- Connect (reuse global client; reconnect only if needed) ---
  if (!client.connected()) {
    client.stop();
    Serial.println("Connecting to server...");
    if (!client.connect(serverHost, serverPort)) {
      Serial.println("ERROR: Cannot connect to server!");
      reinstallMic();
      return;
    }
    client.setNoDelay(true); // flush TCP immediately, no Nagle batching
  }

  // --- HTTP POST headers ---
  client.printf("POST /voice HTTP/1.1\r\n");
  client.printf("Host: %s:%d\r\n", serverHost, serverPort);
  client.printf("Content-Type: audio/wav\r\n");
  client.printf("Content-Length: %d\r\n", wavBytes);
  client.printf("Connection: keep-alive\r\n"); // keep socket open for next prompt
  client.printf("\r\n");

  // --- WAV header ---
  uint8_t wavHeader[44];
  buildWavHeader(wavHeader, pcmBytes);
  client.write(wavHeader, 44);

  // --- Record + stream: fresh DC filter each call ---
  int   recorded = 0;
  float dc       = 0.0f; // reset per recording — do NOT make this static

  while (recorded < totalSamples) {
    size_t bytesRead = 0;
    i2s_read(MIC_I2S_PORT, dmaBuf, sizeof(dmaBuf), &bytesRead, portMAX_DELAY);

    int slots = bytesRead / 4;
    int idx   = 0;

    for (int i = 0; i < slots - 1 && recorded < totalSamples; i += 2) {
      // RIGHT_LEFT format: buffer = [R, L, R, L, ...]  — L/R=GND means Left channel
      int32_t s    = dmaBuf[i + 1] >> 14;
      dc           = 0.995f * dc + 0.005f * (float)s;
      int32_t clean = s - (int32_t)dc;
      if (clean >  32767) clean =  32767;
      if (clean < -32768) clean = -32768;
      pcmBuf[idx++] = (int16_t)clean;
      recorded++;
    }

    if (idx > 0) {
      client.write((uint8_t*)pcmBuf, idx * 2);
    }
  }

  Serial.printf("Recording done: %d samples\n", recorded);

  // --- Wait for HTTP response ---
  unsigned long timeout = millis() + 120000;
  while (!client.available() && millis() < timeout) delay(10);

  if (!client.available()) {
    Serial.println("ERROR: Server timeout!");
    client.stop();
    return;
  }

  // --- Parse response headers ---
  int contentLength = -1;
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.startsWith("Content-Length:") || line.startsWith("content-length:")) {
      contentLength = line.substring(line.indexOf(':') + 1).toInt();
    }
    if (line.length() == 0) break;
  }

  Serial.printf("Response Content-Length: %d\n", contentLength);

  if (contentLength <= 44) {
    String body = client.readString();
    Serial.printf("Server text reply: %s\n", body.c_str());
    return;
  }

  // --- Read WAV header from response ---
  uint8_t respHdr[44];
  size_t hdrRead = 0;
  unsigned long hdrTimeout = millis() + 5000;
  while (hdrRead < 44 && (client.connected() || client.available()) && millis() < hdrTimeout) {
    if (client.available()) {
      hdrRead += client.read(respHdr + hdrRead, 44 - hdrRead);
    } else { delay(1); }
  }

  if (hdrRead == 44 && respHdr[0] == 'R' && respHdr[1] == 'I') {
    uint32_t respRate = *(uint32_t*)(respHdr + 24);
    Serial.printf("Playing response at %dHz\n", respRate);

    setupSpeaker(respRate);
    digitalWrite(SPK_SD, HIGH);
    delay(10);

    // Pre-buffer 4KB before starting playback to cushion WiFi stalls
    int preFilled = 0;
    int remaining = contentLength - 44;
    unsigned long pbTimeout = millis() + 8000;
    while (preFilled < (int)sizeof(preBuf) && preFilled < remaining
           && (client.connected() || client.available())
           && millis() < pbTimeout) {
      if (client.available()) {
        int got = client.read(preBuf + preFilled, sizeof(preBuf) - preFilled);
        if (got > 0) preFilled += got;
      } else { delay(1); }
    }
    size_t bw;
    i2s_write(SPK_I2S_PORT, preBuf, preFilled, &bw, portMAX_DELAY);
    remaining -= preFilled;

    // Stream the rest
    while (remaining > 0 && (client.connected() || client.available())) {
      if (client.available()) {
        int toRead = min(remaining, (int)sizeof(spkBuf));
        int got    = client.read(spkBuf, toRead);
        if (got > 0) {
          i2s_write(SPK_I2S_PORT, spkBuf, got, &bw, portMAX_DELAY);
          remaining -= got;
        }
      } else { delay(1); }
    }

    Serial.println("Playback done.");
    delay(200);
    i2s_zero_dma_buffer(SPK_I2S_PORT);
    digitalWrite(SPK_SD, LOW);
  } else {
    Serial.println("Error: Invalid WAV header in response.");
    client.stop();
  }

  Serial.printf("[Heap after] %d bytes free\n", ESP.getFreeHeap());
  Serial.println("=== Ready for next command ===");
}
