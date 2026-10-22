#include <Arduino.h>
#include "driver/i2s.h"

// I2S Pins for Seeed XIAO ESP32-C6
#define I2S_WS 5  // D4
#define I2S_SD 4  // D3
#define I2S_SCK 6 // D2

#define I2S_PORT I2S_NUM_0
#define SAMPLE_RATE 16000
#define BITS I2S_BITS_PER_SAMPLE_32BIT

void setupI2SMic() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = BITS,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    ; // wait for serial port to connect
  }
  Serial.println("Starting I2S Mic Test...");
  setupI2SMic();
  Serial.println("I2S Mic Initialized.");
}

void loop() {
  int32_t sample = 0;
  size_t bytes_read = 0;
  
  // Read 1 sample (32 bits)
  i2s_read(I2S_PORT, &sample, sizeof(sample), &bytes_read, portMAX_DELAY);
  
  if (bytes_read > 0) {
    // Shift right to get the actual 24-bit data from the INMP441
    sample >>= 8; 
    Serial.println(sample);
  }
}
