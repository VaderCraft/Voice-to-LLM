#include <Arduino.h>
#include "driver/i2s.h"

// Microphone (INMP441)
#define MIC_SD  15
#define MIC_SCK 2
#define MIC_WS  4
#define MIC_I2S_PORT I2S_NUM_0

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=== INMP441 RAW DIAGNOSTIC ===");
  Serial.println("No WiFi. No server. Just raw I2S data.\n");

  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
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

  esp_err_t err = i2s_driver_install(MIC_I2S_PORT, &i2s_config, 0, NULL);
  Serial.printf("i2s_driver_install: %s\n", esp_err_to_name(err));
  
  err = i2s_set_pin(MIC_I2S_PORT, &pin_config);
  Serial.printf("i2s_set_pin: %s\n", esp_err_to_name(err));
  
  i2s_start(MIC_I2S_PORT);
  Serial.println("I2S started. Reading...\n");

  // Discard first 500ms of data (startup transient)
  for (int i = 0; i < 50; i++) {
    int32_t trash[256];
    size_t br;
    i2s_read(MIC_I2S_PORT, trash, sizeof(trash), &br, portMAX_DELAY);
  }
  Serial.println("Startup transient flushed. Now sampling:\n");
}

void loop() {
  int32_t raw[256]; // 128 stereo pairs
  size_t bytesRead = 0;
  
  i2s_read(MIC_I2S_PORT, raw, sizeof(raw), &bytesRead, portMAX_DELAY);
  int slots = bytesRead / 4;

  // Analyze EVEN indices (slot 0, 2, 4...) and ODD indices (1, 3, 5...)
  int32_t evenMin = 2147483647, evenMax = -2147483647;
  int32_t oddMin  = 2147483647, oddMax  = -2147483647;
  int32_t evenSum = 0, oddSum = 0;
  int evenCount = 0, oddCount = 0;
  bool evenAllZero = true, oddAllZero = true;

  for (int i = 0; i < slots; i++) {
    if (i % 2 == 0) {
      if (raw[i] != 0) evenAllZero = false;
      int32_t shifted = raw[i] >> 14;
      if (shifted < evenMin) evenMin = shifted;
      if (shifted > evenMax) evenMax = shifted;
      evenSum += shifted;
      evenCount++;
    } else {
      if (raw[i] != 0) oddAllZero = false;
      int32_t shifted = raw[i] >> 14;
      if (shifted < oddMin) oddMin = shifted;
      if (shifted > oddMax) oddMax = shifted;
      oddSum += shifted;
      oddCount++;
    }
  }

  static int printCount = 0;
  printCount++;
  if (printCount >= 20) { // Print every ~320ms
    printCount = 0;

    int32_t evenRange = evenMax - evenMin;
    int32_t oddRange  = oddMax - oddMin;

    Serial.println("--------------------------------------------");
    Serial.printf("Slots read: %d  (expecting 256)\n", slots);
    Serial.printf("EVEN [0,2,4..] >> 14 Min:%6d Max:%6d Range:%6d %s\n",
                  evenMin, evenMax, evenRange, evenAllZero ? "ALL ZERO!" : "has data");
    Serial.printf("ODD  [1,3,5..] >> 14 Min:%6d Max:%6d Range:%6d %s\n",
                  oddMin, oddMax, oddRange, oddAllZero ? "ALL ZERO!" : "has data");

    // Print a few raw 32-bit hex values so we can see the actual bits
    Serial.printf("Raw hex [0]=%08X [1]=%08X [2]=%08X [3]=%08X\n",
                  (unsigned int)raw[0], (unsigned int)raw[1],
                  (unsigned int)raw[2], (unsigned int)raw[3]);

    // Verdict
    if (evenAllZero && oddAllZero) {
      Serial.println(">>> BOTH CHANNELS DEAD. Check VDD/GND/SD/SCK/WS wiring!");
    } else if (evenRange > oddRange * 3) {
      Serial.println(">>> EVEN slots have signal. Use raw_samples[i] (i+=2, i=0,2,4...)");
    } else if (oddRange > evenRange * 3) {
      Serial.println(">>> ODD slots have signal. Use raw_samples[i+1] (i+=2, i=1,3,5...)");
    } else {
      Serial.println(">>> Both channels have similar energy. Possible crosstalk.");
    }
    Serial.println();
  }
}
