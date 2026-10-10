#pragma once
// ---------------------------------------------------------------------------
// Non-blocking SHT30 / SHT3x driver (single-shot, high repeatability).
// service() only sends a command or reads the result when it is due, so the
// 5 ms PPG / IMU polling loop is never held up by the 15 ms conversion.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <Wire.h>

class Sht3x {
 public:
  bool begin(TwoWire& wire, uint8_t addr, uint32_t periodMs = 2000) {
    wire_ = &wire;
    addr_ = addr;
    periodMs_ = periodMs;
    if (!cmd(0x30A2)) return false;  // soft reset
    delay(3);
    present_ = true;
    state_ = Idle;
    tStart_ = millis() - periodMs_;  // first measurement immediately
    return true;
  }

  // Call often (every loop pass). Cheap when nothing is due.
  void service(uint32_t now) {
    if (!present_) return;
    if (state_ == Idle) {
      if (now - tStart_ >= periodMs_) {
        tStart_ = now;
        if (cmd(0x2400)) state_ = Wait;  // single shot, high repeatability, no clock stretching
        else ++fails_;
      }
    } else if (now - tStart_ >= 16) {  // max conversion time 15 ms
      state_ = Idle;
      readResult(now);
    }
  }

  bool present() const { return present_; }
  // Valid = a good reading within the last 15 s
  bool valid(uint32_t now) const { return haveGood_ && (now - lastGood_) < 15000; }
  float tempC() const { return t_; }
  float rh() const { return rh_; }
  uint32_t failures() const { return fails_; }

 private:
  enum State : uint8_t { Idle, Wait };

  bool cmd(uint16_t c) {
    wire_->beginTransmission(addr_);
    wire_->write((uint8_t)(c >> 8));
    wire_->write((uint8_t)(c & 0xFF));
    return wire_->endTransmission() == 0;
  }
  static uint8_t crc8(const uint8_t* d, int n) {
    uint8_t crc = 0xFF;
    for (int i = 0; i < n; ++i) {
      crc ^= d[i];
      for (int b = 0; b < 8; ++b) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
  }
  void readResult(uint32_t now) {
    if (wire_->requestFrom((int)addr_, 6) != 6) { ++fails_; return; }
    uint8_t b[6];
    for (int i = 0; i < 6; ++i) b[i] = wire_->read();
    if (crc8(b, 2) != b[2] || crc8(b + 3, 2) != b[5]) { ++fails_; return; }
    const uint16_t rt = ((uint16_t)b[0] << 8) | b[1];
    const uint16_t rh = ((uint16_t)b[3] << 8) | b[4];
    t_ = -45.0f + 175.0f * rt / 65535.0f;
    rh_ = 100.0f * rh / 65535.0f;
    if (rh_ > 100.0f) rh_ = 100.0f;
    haveGood_ = true;
    lastGood_ = now;
  }

  TwoWire* wire_ = nullptr;
  uint8_t addr_ = 0x44;
  State state_ = Idle;
  bool present_ = false, haveGood_ = false;
  uint32_t periodMs_ = 2000, tStart_ = 0, lastGood_ = 0, fails_ = 0;
  float t_ = 0, rh_ = 0;
};
