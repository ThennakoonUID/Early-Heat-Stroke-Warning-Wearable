#pragma once
// ---------------------------------------------------------------------------
// Minimal register-level MPU6050 driver using the on-chip FIFO at 1 kHz.
//
// Why 1 kHz? Vibration (motors, vehicles, tapping) is often 20-200 Hz. If the
// IMU were read at 100 Hz those frequencies would alias; sampling at 1 kHz
// with the 184 Hz on-chip low-pass lets us measure vibration energy properly,
// and the firmware then low-passes and decimates to 100 Hz for body motion.
//
//   DLPF_CFG = 1 : accel 184 Hz / gyro 188 Hz bandwidth, 1 kHz internal rate
//   SMPLRT_DIV = 0 -> 1 kHz output
//   Accel +/-4 g (8192 LSB/g), gyro +/-500 dps (65.5 LSB/dps)
//   FIFO holds accel XYZ + gyro XYZ = 12 bytes per sample (85 samples max)
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <Wire.h>

class Mpu6050 {
 public:
  static constexpr float kAccLsbPerG = 8192.0f;
  static constexpr float kGyroLsbPerDps = 65.5f;

  enum : uint8_t {
    SMPLRT_DIV = 0x19, CONFIG = 0x1A, GYRO_CONFIG = 0x1B, ACCEL_CONFIG = 0x1C,
    FIFO_EN = 0x23, INT_ENABLE = 0x38, INT_STATUS = 0x3A, SIGNAL_PATH_RESET = 0x68,
    USER_CTRL = 0x6A, PWR_MGMT_1 = 0x6B, FIFO_COUNTH = 0x72, FIFO_R_W = 0x74,
    WHO_AM_I = 0x75
  };

  bool begin(TwoWire& wire, uint8_t addr = 0x68) {
    wire_ = &wire;
    addr_ = addr;
    whoAmI_ = read8(WHO_AM_I);  // 0x68 genuine; clones report 0x70/0x72/0x98
    if (whoAmI_ == 0x00 || whoAmI_ == 0xFF) return false;

    write8(PWR_MGMT_1, 0x80);  // device reset
    delay(100);
    write8(SIGNAL_PATH_RESET, 0x07);
    delay(100);
    write8(PWR_MGMT_1, 0x01);  // wake, clock = X-gyro PLL (more stable)
    delay(10);
    write8(CONFIG, 0x01);        // DLPF 184 Hz, 1 kHz internal
    write8(SMPLRT_DIV, 0x00);    // 1 kHz
    write8(GYRO_CONFIG, 0x08);   // +/-500 dps
    write8(ACCEL_CONFIG, 0x08);  // +/-4 g
    write8(INT_ENABLE, 0x00);
    resetFifo();
    return true;
  }

  uint8_t whoAmI() const { return whoAmI_; }
  uint32_t overflows() const { return overflows_; }

  void resetFifo() {
    write8(FIFO_EN, 0x00);
    write8(USER_CTRL, 0x04);  // FIFO reset
    delay(1);
    write8(USER_CTRL, 0x40);  // FIFO enable
    write8(FIFO_EN, 0x78);    // XG, YG, ZG, ACCEL
  }

  // raw[i] = {ax, ay, az, gx, gy, gz}. Returns samples read (0 on overflow).
  int readFifo(int16_t (*raw)[6], int maxN) {
    if (read8(INT_STATUS) & 0x10) {  // FIFO overflow: data misaligned, restart
      ++overflows_;
      resetFifo();
      return 0;
    }
    uint8_t c[2];
    if (!readBurst(FIFO_COUNTH, c, 2)) return 0;
    const int count = ((int)c[0] << 8) | c[1];
    int n = count / 12;
    if (n > maxN) n = maxN;

    int got = 0;
    while (got < n) {
      const int chunk = min(n - got, 10);  // 120 bytes per I2C read
      uint8_t b[120];
      if (!readBurst(FIFO_R_W, b, chunk * 12)) break;
      for (int i = 0; i < chunk; ++i)
        for (int k = 0; k < 6; ++k)
          raw[got + i][k] = (int16_t)((b[i * 12 + 2 * k] << 8) | b[i * 12 + 2 * k + 1]);
      got += chunk;
    }
    return got;
  }

 private:
  void write8(uint8_t reg, uint8_t v) {
    wire_->beginTransmission(addr_);
    wire_->write(reg);
    wire_->write(v);
    wire_->endTransmission();
  }
  uint8_t read8(uint8_t reg) {
    uint8_t v = 0;
    readBurst(reg, &v, 1);
    return v;
  }
  bool readBurst(uint8_t reg, uint8_t* dst, int len) {
    wire_->beginTransmission(addr_);
    wire_->write(reg);
    if (wire_->endTransmission(false) != 0) return false;
    if (wire_->requestFrom((int)addr_, len) != len) return false;
    for (int i = 0; i < len; ++i) dst[i] = wire_->read();
    return true;
  }

  TwoWire* wire_ = nullptr;
  uint8_t addr_ = 0x68, whoAmI_ = 0;
  uint32_t overflows_ = 0;
};
