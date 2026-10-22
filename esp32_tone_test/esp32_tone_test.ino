/*
  MAX98357A DEFINITIVE TEST - 32-bit mode
  BCLK=26, LRC=25, DIN=33, SD=18
*/
#include <driver/i2s.h>
#include <math.h>

#define SPK_BCLK  27
#define SPK_LRC   26
#define SPK_DIN   33
#define SPK_SD    13  // Moved from D18 to D13
#define I2S_PORT  I2S_NUM_1

float phase = 0.0f;

void setup() {
  Serial.begin(115200);
  delay(500);

  // Enable amp BEFORE touching I2S
  pinMode(SPK_SD, OUTPUT);
  digitalWrite(SPK_SD, HIGH);
  Serial.println("SD=HIGH, amp on");
  delay(200);

  i2s_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.mode               = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate        = 44100;
  cfg.bits_per_sample    = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format     = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags   = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count      = 8;
  cfg.dma_buf_len        = 512;
  cfg.use_apll           = false;
  cfg.tx_desc_auto_clear = true;

  esp_err_t err = i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  Serial.printf("install: %s\n", esp_err_to_name(err));

  i2s_pin_config_t pins;
  memset(&pins, 0, sizeof(pins));
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  pins.bck_io_num   = SPK_BCLK;
  pins.ws_io_num    = SPK_LRC;
  pins.data_out_num = SPK_DIN;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;

  err = i2s_set_pin(I2S_PORT, &pins);
  Serial.printf("set_pin: %s\n", esp_err_to_name(err));

  Serial.println("Playing 440Hz...");
}

void loop() {
  // 32-bit stereo buffer — 128 frames = 512 int32 values
  int32_t buf[256];
  const float step = (2.0f * PI * 440.0f) / 44100.0f;

  for (int i = 0; i < 128; i++) {
    // Scale to 32-bit range, place audio in upper 16 bits
    int32_t s = (int32_t)(sinf(phase) * 2147418112.0f); // 0x7FFF << 16
    buf[i * 2]     = s; // Left
    buf[i * 2 + 1] = s; // Right
    phase += step;
    if (phase >= 2.0f * PI) phase -= 2.0f * PI;
  }

  size_t written = 0;
  i2s_write(I2S_PORT, buf, sizeof(buf), &written, portMAX_DELAY);

  static int n = 0;
  if (n++ % 500 == 0) Serial.printf("loop %d written=%d\n", n, written);
}
