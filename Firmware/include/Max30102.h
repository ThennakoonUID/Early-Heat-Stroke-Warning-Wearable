#pragma once
// ---------------------------------------------------------------------------
// Minimal register-level MAX30102 driver (Arduino / ESP32).
//
// Configuration chosen for a reflectance PPG on the upper back (C7):
//   - SpO2 mode (Red + IR). Only IR is used for the PPG; Red is kept low.
//   - LED pulse width 411 us  -> longest integration, 18-bit ADC resolution
//   - ADC full-scale 16384 nA -> largest range, so a high LED current does not
//                                saturate the photodiode
//   - 400 samples/s internally, on-chip averaging of 4 -> 100 Hz output.
//     Averaging 4 samples cuts white noise by ~2x (6 dB) for free.
//   - IR LED current starts at the maximum (0xFF = 51 mA). The processing
//     code backs it off only if the ADC approaches saturation.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <Wire.h>

class Max30102 {
 public:
  static constexpr uint8_t kAddr = 0x57;
  static constexpr uint32_t kFullScale = 262143;  // 18-bit

  // Registers
  enum : uint8_t {
    INT_STAT1 = 0x00, INT_STAT2 = 0x01, INT_EN1 = 0x02, INT_EN2 = 0x03,
    FIFO_WR = 0x04, FIFO_OVF = 0x05, FIFO_RD = 0x06, FIFO_DATA = 0x07,
    FIFO_CFG = 0x08, MODE_CFG = 0x09, SPO2_CFG = 0x0A,
    LED1_PA = 0x0C,  // Red
    LED2_PA = 0x0D,  // IR
    PART_ID = 0xFF
  };

  bool begin(TwoWire& wire, uint8_t irPa, uint8_t redPa) {
    wire_ = &wire;
    if (read8(PART_ID) != 0x15) return false;

    write8(MODE_CFG, 0x40);  // soft reset
    const uint32_t t0 = millis();
    while ((read8(MODE_CFG) & 0x40) && millis() - t0 < 100) delay(1);
    read8(INT_STAT1);
    read8(INT_STAT2);
    write8(INT_EN1, 0x20);  // only ALC_OVF (ambient-light overflow) flag; INT pin unused, we poll
    write8(INT_EN2, 0x00);

    // FIFO: SMP_AVE = 4 (010), roll-over enabled, almost-full = 15
    write8(FIFO_CFG, (0b010 << 5) | (1 << 4) | 0x0F);
    // SpO2: ADC range 16384 nA (11), 400 sps (011), 411 us / 18 bit (11)
    write8(SPO2_CFG, (0b11 << 5) | (0b011 << 2) | 0b11);
    setRedPa(redPa);
    setIrPa(irPa);
    write8(MODE_CFG, 0x03);  // SpO2 mode: Red + IR
    clearFifo();
    return true;
  }

  void setIrPa(uint8_t pa) { irPa_ = pa; write8(LED2_PA, pa); }
  void setRedPa(uint8_t pa) { write8(LED1_PA, pa); }
  uint8_t irPa() const { return irPa_; }
  static float paToMilliAmp(uint8_t pa) { return pa * 0.2f; }
  uint32_t overflows() const { return overflows_; }

  // True if the ambient-light-cancellation circuit overflowed since the last call
  // (strong sunlight / lamp on the photodiode). Reading INT_STAT1 clears the flag.
  bool alcOverflow() { return (read8(INT_STAT1) & 0x20) != 0; }

  void clearFifo() {
    write8(FIFO_WR, 0);
    write8(FIFO_OVF, 0);
    write8(FIFO_RD, 0);
  }

  // Reads all new samples (up to maxN). Returns the number read.
  int readSamples(uint32_t* red, uint32_t* ir, int maxN) {
    uint8_t p[3];  // FIFO_WR, FIFO_OVF, FIFO_RD are consecutive
    if (!readBurst(FIFO_WR, p, 3)) return 0;
    int n = (p[0] - p[2]) & 0x1F;
    if (p[1] != 0) { n = 32; ++overflows_; }  // FIFO was full: we fell behind
    if (n > maxN) n = maxN;

    int got = 0;
    while (got < n) {
      const int chunk = min(n - got, 20);  // 20 * 6 = 120 bytes < 128 B Wire buffer
      uint8_t b[120];
      if (!readBurst(FIFO_DATA, b, chunk * 6)) break;
      for (int i = 0; i < chunk; ++i) {
        const uint8_t* s = &b[i * 6];
        red[got + i] = (((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2]) & 0x3FFFF;
        ir[got + i] = (((uint32_t)s[3] << 16) | ((uint32_t)s[4] << 8) | s[5]) & 0x3FFFF;
      }
      got += chunk;
    }
    return got;
  }

 private:
  void write8(uint8_t reg, uint8_t v) {
    wire_->beginTransmission(kAddr);
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
    wire_->beginTransmission(kAddr);
    wire_->write(reg);
    if (wire_->endTransmission(false) != 0) return false;
    if (wire_->requestFrom((int)kAddr, len) != len) return false;
    for (int i = 0; i < len; ++i) dst[i] = wire_->read();
    return true;
  }

  TwoWire* wire_ = nullptr;
  uint8_t irPa_ = 0xFF;
  uint32_t overflows_ = 0;
};
