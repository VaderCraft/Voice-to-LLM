#include <Arduino.h>
#include "driver/i2s.h"
#include <WiFi.h>

// ===== PIN MAPPING =====
// Microphone (INMP441) - I2S Port 0
#define MIC_SD  15
#define MIC_SCK 2
#define MIC_WS  4

// Speaker Amplifier (MAX98357A) - I2S Port 1
#define SPK_DIN  14
#define SPK_BCLK 27
#define SPK_LRC  26
#define SPK_SD   13

// ===== CONFIG =====
#define MIC_I2S_PORT  I2S_NUM_0
#define SPK_I2S_PORT  I2S_NUM_1
#define MIC_SAMPLE_RATE 16000

// --- Wi-Fi & PC Server Config ---
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* streamServerHost = "YOUR_VOICE_SERVER_HOST"; 
const int streamServerPort = 5001; 

WiFiClient tcpClient;

// ===== MICROPHONE SETUP (I2S_NUM_0 - Input) =====
void setupMic() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = MIC_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, // INMP441 uses 32-bit slots
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT, // Stereo capture for stable I2S clocks
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config;
  pin_config.bck_io_num = MIC_SCK;
  pin_config.ws_io_num = MIC_WS;
  pin_config.data_out_num = I2S_PIN_NO_CHANGE;
  pin_config.data_in_num = MIC_SD;
  #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
  pin_config.mck_io_num = I2S_PIN_NO_CHANGE;
  #endif

  i2s_driver_install(MIC_I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(MIC_I2S_PORT, &pin_config);
  i2s_start(MIC_I2S_PORT);
}

// ===== SPEAKER SETUP (I2S_NUM_1 - Output) =====
void setupSpeaker(int sampleRate) {
  i2s_driver_uninstall(SPK_I2S_PORT);
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = sampleRate,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config;
  pin_config.bck_io_num = SPK_BCLK;
  pin_config.ws_io_num = SPK_LRC;
  pin_config.data_out_num = SPK_DIN;
  pin_config.data_in_num = I2S_PIN_NO_CHANGE;
  #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
  pin_config.mck_io_num = I2S_PIN_NO_CHANGE;
  #endif

  i2s_driver_install(SPK_I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(SPK_I2S_PORT, &pin_config);
  i2s_zero_dma_buffer(SPK_I2S_PORT);
}

// ===== PLAY NOTIFICATION BEEP (LOCAL SINE WAVE) =====
void playBeep() {
  Serial.println("Wake Word Heard! Beeping...");
  i2s_stop(MIC_I2S_PORT);
  setupSpeaker(16000);
  digitalWrite(SPK_SD, HIGH);
  delay(10);

  int16_t beepBuf[256];
  for (int i = 0; i < 256; i++) {
    beepBuf[i] = (int16_t)(sin(2.0 * PI * 880.0 * i / 16000.0) * 12000.0);
  }
  size_t bytesWritten;
  for (int j = 0; j < 8; j++) {
    i2s_write(SPK_I2S_PORT, beepBuf, sizeof(beepBuf), &bytesWritten, portMAX_DELAY);
  }

  delay(10);
  i2s_zero_dma_buffer(SPK_I2S_PORT);
  digitalWrite(SPK_SD, LOW);
  Serial.println("Beep finished. Listening for command...");
  i2s_start(MIC_I2S_PORT);
}

// ===== PLAY AUDIO RESPONSE FROM STREAM =====
void playStreamResponse() {
  i2s_stop(MIC_I2S_PORT);

  uint32_t wavLen = 0;
  size_t lenBytesRead = 0;
  while (lenBytesRead < 4 && tcpClient.connected()) {
    if (tcpClient.available()) {
      lenBytesRead += tcpClient.read(((uint8_t*)&wavLen) + lenBytesRead, 4 - lenBytesRead);
    } else { delay(1); }
  }

  Serial.printf("Receiving streamed WAV response of size: %d bytes\n", wavLen);
  if (wavLen <= 44) {
    Serial.println("Error: Stream response length too short.");
    i2s_start(MIC_I2S_PORT);
    return;
  }

  uint8_t header[44];
  size_t headerRead = 0;
  while (headerRead < 44 && tcpClient.connected()) {
    if (tcpClient.available()) {
      headerRead += tcpClient.read(header + headerRead, 44 - headerRead);
    } else { delay(1); }
  }

  if (headerRead == 44 && header[0] == 'R' && header[1] == 'I') {
    uint32_t sampleRate = *(uint32_t*)(header + 24);
    Serial.printf("Playing stream response: %dHz\n", sampleRate);
    setupSpeaker(sampleRate);
    digitalWrite(SPK_SD, HIGH);
    delay(10);

    uint8_t chunkBuf[512];
    size_t totalBytesPlayed = 0;
    size_t pcmTargetBytes = wavLen - 44;

    while (totalBytesPlayed < pcmTargetBytes && tcpClient.connected()) {
      if (tcpClient.available()) {
        size_t bytesToRead = sizeof(chunkBuf);
        size_t remaining = pcmTargetBytes - totalBytesPlayed;
        if (remaining < bytesToRead) bytesToRead = remaining;
        size_t bytesRead = tcpClient.read(chunkBuf, bytesToRead);
        if (bytesRead > 0) {
          size_t bytesWritten;
          i2s_write(SPK_I2S_PORT, chunkBuf, bytesRead, &bytesWritten, portMAX_DELAY);
          totalBytesPlayed += bytesRead;
        }
      } else { delay(1); }
    }

    Serial.printf("Playback complete. Streamed %d bytes.\n", totalBytesPlayed);
    delay(200);
    i2s_zero_dma_buffer(SPK_I2S_PORT);
    digitalWrite(SPK_SD, LOW);
  } else {
    Serial.println("Error: Invalid WAV header received.");
  }

  i2s_start(MIC_I2S_PORT);
}

// ===== WIFI CONNECTION =====
void connectWiFi() {
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected.");
}

// ===== TCP SERVER CONNECTION =====
void connectTCP() {
  Serial.printf("Connecting to TCP streaming server at %s:%d...\n", streamServerHost, streamServerPort);
  while (!tcpClient.connect(streamServerHost, streamServerPort)) {
    Serial.print(".");
    delay(1000);
  }
  Serial.println("\nTCP Connected! Now streaming...");
}

// DC offset filter state
float dc_offset = 0;

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  pinMode(SPK_SD, OUTPUT);
  digitalWrite(SPK_SD, LOW);

  Serial.println("Connecting to WiFi...");
  connectWiFi();
  Serial.println("Initializing Mic...");
  setupMic();
  connectTCP();
  Serial.println("System Ready. Say 'Alexa' or 'Hey Jarvis' to wake!");
}

// ===== MAIN LOOP =====
void loop() {
  if (!tcpClient.connected()) {
    connectTCP();
  }

  // Continuous Microphone Capture & Stream
  // Read stereo (L+R interleaved), extract LEFT channel only (L/R pin is GND)
  int32_t raw_samples[256];
  int16_t pcm_samples[128];
  size_t bytesRead = 0;

  i2s_read(MIC_I2S_PORT, raw_samples, sizeof(raw_samples), &bytesRead, portMAX_DELAY);

  int totalSlots = bytesRead / 4;
  int pcmIdx = 0;

  for (int i = 0; i < totalSlots - 1; i += 2) { // step by 2: [Right, Left, Right, Left...]
    if (pcmIdx >= 128) break;

    int32_t raw = raw_samples[i + 1]; // LEFT channel at ODD index (RIGHT_LEFT = [R,L,R,L...])
    int32_t sample = raw >> 10;   // 16x louder than >>14

    // DC offset removal
    dc_offset = 0.995f * dc_offset + 0.005f * (float)sample;
    int32_t clean = sample - (int32_t)dc_offset;
    if (clean > 32767) clean = 32767;
    if (clean < -32768) clean = -32768;

    pcm_samples[pcmIdx++] = (int16_t)clean;
  }

  if (pcmIdx > 0) {
    tcpClient.write((uint8_t*)pcm_samples, pcmIdx * 2);
  }

  // Diagnostics
  static int diagCount = 0;
  diagCount++;
  if (diagCount >= 100) {
    diagCount = 0;
    int16_t minVal = 32767, maxVal = -32768;
    for (int i = 0; i < pcmIdx; i++) {
      if (pcm_samples[i] < minVal) minVal = pcm_samples[i];
      if (pcm_samples[i] > maxVal) maxVal = pcm_samples[i];
    }
    Serial.printf("[Mic] Min:%d Max:%d Range:%d\n", minVal, maxVal, maxVal - minVal);
    if ((maxVal - minVal) < 5) {
      Serial.println("  [!!] Signal near-zero. Check INMP441 wiring.");
    }
  }

  // Listen for Server-Side Trigger Commands
  if (tcpClient.available()) {
    uint8_t cmd = tcpClient.read();
    if (cmd == 0x01) {
      playBeep();
    } else if (cmd == 0x02) {
      playStreamResponse();
    }
  }
}
