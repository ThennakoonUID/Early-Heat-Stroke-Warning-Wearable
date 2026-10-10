#pragma once
// ---------------------------------------------------------------------------
// GSR analog front end driver + NTC reader for the ESP32 (DevKit V1).
//
// Runs as ONE FreeRTOS task pinned to core 0, so the PPG / IMU / SHT30 code in
// loop() (core 1) is never disturbed. Everything that touches ADC1 lives in
// this task (GSR on GPIO32, NTCs on GPIO34 / GPIO35), so ADC1 is never used
// from two cores at once.
//
//   Excitation : 1 kHz sine from the 8-bit DAC (GPIO25), N samples per cycle.
//   Sampling   : the ADC (GPIO32) is read right after every DAC update, driven
//                by the CPU cycle counter, so the sine and the samples share
//                one clock and the lock-in phase is repeatable.
//                N = 20 -> 20 kS/s (50 us per sample), N = 10 -> 10 kS/s fallback
//                if this chip cannot keep up (decided by a benchmark at start).
//   Lock-in    : whole cycles only (Gsr.h). A 10k reference measurement
//                (SW1) is repeated every kGsrRefIntervalMs and cancels the TIA
//                phase lag, DAC/ADC gain and DAC->ADC delay.
//   Schedule   : [ref window (every 20 s)] -> skin window (~230 ms) ->
//                [NTC read (~every 1 s)] -> yield -> repeat
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <driver/adc.h>
#include <driver/dac.h>
#include <esp_adc_cal.h>

#include <complex>

#include "Config.h"
#include "Gsr.h"

struct GsrSnapshot {
  uint32_t seq = 0;          // increments with every new GSR value
  bool valid = false;        // reference OK, signal not clipped
  bool contact = false;      // valid and conductance above kGsrContactMinUs
  bool saturated = false;
  bool refOk = false;
  float gUs = 0;             // skin admittance magnitude (microsiemens)
  float zKohm = 0;           // skin impedance magnitude (kohm)
  float phaseDeg = 0;        // phase of the skin impedance
  float ampCounts = 0;       // 1 kHz amplitude in the skin path (ADC counts)
  float snrDb = 0;           // fundamental vs everything else
  float refAmp = 0;          // 1 kHz amplitude in the reference path (ADC counts)
  uint32_t refAgeMs = 0;
  uint32_t ntcSeq = 0;       // increments with every new NTC reading
  int ntcRaw[2] = {0, 0};    // raw 12-bit analogRead() at GPIO34 / GPIO35
  uint8_t samplesPerCycle = 20;
  uint32_t overruns = 0;     // samples that missed their time slot (should stay ~0)
  float benchUs = 0;         // measured time of one DAC write + ADC read
};

class GsrFrontEnd {
 public:
  bool begin() {
    pinMode(kGsrSw1Pin, OUTPUT);
    pinMode(kGsrSw2Pin, OUTPUT);
    digitalWrite(kGsrSw1Pin, LOW);
    digitalWrite(kGsrSw2Pin, LOW);

    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_4, ADC_ATTEN_DB_11);  // GPIO32
    adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);  // GPIO34
    adc1_config_channel_atten(ADC1_CHANNEL_7, ADC_ATTEN_DB_11);  // GPIO35
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &chars_);

    dac_output_enable(DAC_CHANNEL_1);  // GPIO25
    dac_output_voltage(DAC_CHANNEL_1, kDacMid);

    // Benchmark: how long do one DAC write + one ADC read take on this board?
    const uint32_t t0 = micros();
    volatile int sink = 0;
    for (int i = 0; i < 400; ++i) {
      dac_output_voltage(DAC_CHANNEL_1, kDacMid + (i & 1));
      sink = sink + adc1_get_raw(ADC1_CHANNEL_4);
    }
    benchUs_ = (micros() - t0) / 400.0f;
    dac_output_voltage(DAC_CHANNEL_1, kDacMid);

    if (benchUs_ <= 36.0f) { n_ = 20; periodUs_ = 50; }
    else if (benchUs_ <= 85.0f) { n_ = 10; periodUs_ = 100; }
    else { n_ = 10; periodUs_ = 100; timingOk_ = false; }
    stepCycles_ = (uint32_t)getCpuFrequencyMhz() * periodUs_;

    for (int i = 0; i < n_; ++i)
      dacTab_[i] = (uint8_t)(kDacMid + lroundf((float)kGsrDacAmp * sinf(2.0f * (float)M_PI * i / n_)));
    li_.begin(n_);

    skinParams_.rs = kGsrRs;
    skinParams_.rref = kGsrRref;
    skinParams_.ron = kGsrRon;
    skinParams_.ccouple = kGsrCcouple;
    skinParams_.freq = kGsrFreq;

    snap_.samplesPerCycle = (uint8_t)n_;
    snap_.benchUs = benchUs_;

    xTaskCreatePinnedToCore(&GsrFrontEnd::trampoline, "gsr", 6144, this, 10, nullptr, 0);
    return timingOk_;
  }

  // Copy of the latest results (thread-safe). Returns false until the first GSR value exists.
  bool get(GsrSnapshot& out) {
    portENTER_CRITICAL(&mux_);
    out = snap_;
    portEXIT_CRITICAL(&mux_);
    return out.seq > 0;
  }

  int samplesPerCycle() const { return n_; }
  float benchUs() const { return benchUs_; }
  bool timingOk() const { return timingOk_; }

 private:
  static constexpr uint8_t kDacMid = 128;  // 1.65 V; the AC coupling removes it anyway

  static void trampoline(void* self) { static_cast<GsrFrontEnd*>(self)->run(); }

  // Break-before-make: never both switches closed.
  void setPath(bool reference) {
    if (reference) {
      digitalWrite(kGsrSw2Pin, LOW);
      delayMicroseconds(2);
      digitalWrite(kGsrSw1Pin, HIGH);
    } else {
      digitalWrite(kGsrSw1Pin, LOW);
      delayMicroseconds(2);
      digitalWrite(kGsrSw2Pin, HIGH);
    }
  }

  // Excites and samples (settle + measure) cycles; the lock-in only sees the measure cycles.
  // Returns true when (almost) every sample met its time slot.
  bool runWindow(bool reference, int measureCycles) {
    setPath(reference);
    li_.reset();
    const int settleN = kGsrSettleCycles * n_;
    const int total = settleN + measureCycles * n_;
    uint32_t late = 0;
    int idx = 0;
    uint32_t next = ESP.getCycleCount() + stepCycles_;
    for (int n = 0; n < total; ++n) {
      while ((int32_t)(ESP.getCycleCount() - next) < 0) {}
      if ((int32_t)(ESP.getCycleCount() - next) > (int32_t)(stepCycles_ / 2)) {
        ++late;  // interrupted for too long: re-sync the time base
        next = ESP.getCycleCount();
      }
      dac_output_voltage(DAC_CHANNEL_1, dacTab_[idx]);
      const int v = adc1_get_raw(ADC1_CHANNEL_4);
      if (n >= settleN) li_.add(v);
      next += stepCycles_;
      if (++idx >= n_) idx = 0;
    }
    dac_output_voltage(DAC_CHANNEL_1, kDacMid);
    overruns_ += late;
    return late * 200 < (uint32_t)total;  // < 0.5 % late
  }

  // thermistor.cpp logic: plain 12-bit analogRead() of the NTC pin
  int readNtcRaw(int pin) { return analogRead(pin); }

  void run() {
    std::complex<double> mRef(0, 0);
    bool haveRef = false;
    uint32_t refAt = 0, ntcAt = 0;
    float refAmp = 0;
    int ntc[2] = {0, 0};
    uint32_t ntcSeq = 0;
    uint32_t seq = 0;

    for (;;) {
      uint32_t now = millis();

      // 1) reference resistor (self-calibration of phase and gain)
      if (!haveRef || now - refAt >= kGsrRefIntervalMs) {
        const bool timed = runWindow(true, kGsrRefCycles);
        const std::complex<double> m = li_.phasor();
        refAmp = (float)std::abs(m);
        const bool clip = li_.minSample() <= 20 || li_.maxSample() >= 4075;
        haveRef = timed && !clip && refAmp >= kGsrRefAmpMin && refAmp <= kGsrRefAmpMax;
        if (haveRef) mRef = m;
        refAt = millis();
      }

      // 2) skin path
      const bool timedSkin = runWindow(false, kGsrMeasCycles);
      const std::complex<double> mSkin = li_.phasor();
      const bool clipSkin = li_.minSample() <= 20 || li_.maxSample() >= 4075;
      const double noise = li_.residualRms();
      const double amp = std::abs(mSkin);

      GsrSnapshot s;
      s.refOk = haveRef;
      s.saturated = clipSkin;
      s.ampCounts = (float)amp;
      s.refAmp = refAmp;
      s.snrDb = (float)(20.0 * log10((amp / 1.41421356 + 1e-9) / (noise + 0.01)));
      if (haveRef && timedSkin && !clipSkin) {
        const gsr::SkinResult r = gsr::skinFromPhasors(mRef, mSkin, skinParams_);
        if (r.ok) {
          s.valid = true;
          s.gUs = (float)r.gUs;
          s.zKohm = (float)(r.zMag / 1000.0);
          s.phaseDeg = (float)r.phaseDeg;
          s.contact = r.gUs >= kGsrContactMinUs;
        }
      }
      s.refAgeMs = millis() - refAt;

      // 3) NTCs, about once per second (between GSR windows, same task = no ADC1 conflict)
      now = millis();
      if (now - ntcAt >= 1000) {
        ntc[0] = readNtcRaw(kNtc1Pin);
        ntc[1] = readNtcRaw(kNtc2Pin);
        ntcAt = now;
        ++ntcSeq;
      }
      s.ntcSeq = ntcSeq;
      s.ntcRaw[0] = ntc[0];
      s.ntcRaw[1] = ntc[1];
      s.samplesPerCycle = (uint8_t)n_;
      s.overruns = overruns_;
      s.benchUs = benchUs_;
      s.seq = ++seq;

      portENTER_CRITICAL(&mux_);
      snap_ = s;
      portEXIT_CRITICAL(&mux_);

      vTaskDelay(1);  // lets the idle task run (task watchdog) and other core-0 work happen
    }
  }

  gsr::LockIn li_;
  gsr::SkinParams skinParams_;
  esp_adc_cal_characteristics_t chars_;
  uint8_t dacTab_[gsr::kMaxN] = {0};
  int n_ = 20;
  uint32_t periodUs_ = 50, stepCycles_ = 12000, overruns_ = 0;
  float benchUs_ = 0;
  bool timingOk_ = true;
  GsrSnapshot snap_;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};
