#pragma once
// ---------------------------------------------------------------------------
// IMU signal chain (pure C++, PC-testable).
//
//  ImuFrontEnd   : runs at the MPU6050's 1 kHz rate.
//                  - vibration path: 15 Hz high-pass on each accel axis ->
//                    vector magnitude -> 100 ms moving RMS  (catches buzzing,
//                    tapping, vehicle / motor vibration up to ~180 Hz)
//                  - 35 Hz anti-alias low-pass on all 6 axes, then decimate
//                    1 kHz -> 100 Hz to match the PPG sample rate.
//
//  MotionDetector: runs at 100 Hz.
//                  - body motion  : 0.3 Hz high-pass (removes gravity) on each
//                    accel axis -> |a_dyn| -> 0.5 s RMS
//                  - rotation     : 0.1 Hz high-pass (removes gyro bias) on each
//                    gyro axis  -> |w| -> 0.5 s RMS
//                  - vibration    : from ImuFrontEnd
//                  Using vector magnitudes makes it direction-independent.
//                  A 5 s calibration while the subject sits still measures the
//                  "normal" level (breathing, heartbeat recoil, sensor noise);
//                  thresholds are set relative to that baseline.
//                  Workers cannot always sit still on demand, so:
//                  - after maxCalRestarts failed attempts calibration falls
//                    back to the absolute floors (calFallback() == true)
//                  - while the wearer is quiet, the baseline keeps adapting
//                    slowly (autoTauSec), within [floor, autoMaxGrow x the
//                    calibrated threshold], so a changing environment
//                    (vehicle idling, new strap tension) does not leave the
//                    thresholds stale.
// ---------------------------------------------------------------------------
#include "Filters.h"

class ImuFrontEnd {
 public:
  void begin(float fsIn = 1000.0f, int decim = 10) {
    decim_ = decim;
    for (int i = 0; i < 3; ++i) vibHp_[i].highpass(fsIn, 15.0f, 0.7071f);
    for (int i = 0; i < 6; ++i) aa_[i].lowpass(fsIn, 35.0f, 0.7071f);
    vibRms_.reset();
    k_ = 0;
    primed_ = false;
  }

  // a in g, g in deg/s. Returns true when a new 100 Hz sample is ready.
  bool push(const float a[3], const float g[3]) {
    if (!primed_) {  // run the filters to steady state on the first sample
      for (int r = 0; r < 500; ++r)
        for (int i = 0; i < 3; ++i) {
          vibHp_[i].process(a[i]);
          aa_[i].process(a[i]);
          aa_[i + 3].process(g[i]);
        }
      primed_ = true;
    }
    float e = 0.0f;
    for (int i = 0; i < 3; ++i) {
      const float h = vibHp_[i].process(a[i]);
      e += h * h;
    }
    vib_ = vibRms_.process(sqrtf(e));
    float f[6];
    for (int i = 0; i < 3; ++i) {
      f[i] = aa_[i].process(a[i]);
      f[i + 3] = aa_[i + 3].process(g[i]);
    }
    if (++k_ < decim_) return false;
    k_ = 0;
    for (int i = 0; i < 3; ++i) { acc_[i] = f[i]; gyro_[i] = f[i + 3]; }
    return true;
  }

  const float* acc() const { return acc_; }    // g, 100 Hz
  const float* gyro() const { return gyro_; }  // deg/s, 100 Hz
  float vib() const { return vib_; }           // g RMS of >15 Hz content

 private:
  dsp::Biquad vibHp_[3], aa_[6];
  dsp::MovingRms<100> vibRms_;
  float acc_[3] = {0, 0, 1}, gyro_[3] = {0}, vib_ = 0;
  int decim_ = 10, k_ = 0;
  bool primed_ = false;
};

struct MotionFeatures {
  float acc = 0;    // g   RMS of dynamic acceleration (0.3 Hz+)
  float gyro = 0;   // dps RMS of angular rate
  float vib = 0;    // g   RMS of vibration (>15 Hz)
  float score = 0;  // max(feature / threshold); >1 means motion
  float bodyRatio = 0;  // max(acc, gyro) / threshold: in-band body motion only
  bool motion = true;
  bool calibrating = true;
};

class MotionDetector {
 public:
  // Tunables ----------------------------------------------------------------
  float warmupSec = 1.5f;     // let the high-pass filters settle first
  float calSec = 5.0f;        // subject must sit still for this long
  float kMul = 2.5f;          // threshold >= kMul * baseline mean
  float kStd = 8.0f;          // threshold >= mean + kStd * baseline std
  float accFloor = 0.012f;    // g   (absolute minimum thresholds)
  float gyroFloor = 1.5f;     // dps
  float vibFloor = 0.012f;    // g
  float calMaxAcc = 0.05f;    // g - if baseline is above this, the subject was moving
  float exitRatio = 0.7f;     // hysteresis: leave motion when score < exitRatio ...
  float exitHoldSec = 0.3f;   // ... for this long
  int maxCalRestarts = 4;     // then accept the floors instead of insisting on stillness
  float autoTauSec = 60.0f;   // baseline adaptation time constant while quiet
  float autoQuietSec = 3.0f;  // quiet for this long before adapting
  float autoQuietRatio = 0.6f;  // "quiet" = score below this
  float autoMaxGrow = 2.0f;   // thresholds never exceed this x calibrated value

  void begin(float fs) {
    fs_ = fs;
    for (int i = 0; i < 3; ++i) {
      accHp_[i].highpass(fs, 0.3f, 0.7071f);   // double precision: pole near z = 1
      gyroHp_[i].highpass(fs, 0.1f, 0.7071f);
    }
    startCalibration();
  }

  void startCalibration() {
    n_ = 0;
    memset(sum_, 0, sizeof(sum_));
    memset(sq_, 0, sizeof(sq_));
    f_.calibrating = true;
    f_.motion = true;
    calRestarts_ = 0;
    calFallback_ = false;
    autoQuiet_ = 0;
  }

  const MotionFeatures& update(const float a[3], const float g[3], float vib) {
    // Subtract the first sample (gravity + gyro bias) so the high-pass filters
    // do not see a 1 g step at power-up and settle immediately.
    if (!haveOffset_) {
      for (int i = 0; i < 3; ++i) { aOff_[i] = a[i]; gOff_[i] = g[i]; }
      haveOffset_ = true;
    }
    float ea = 0, eg = 0;
    for (int i = 0; i < 3; ++i) {
      const float ha = (float)accHp_[i].process((double)(a[i] - aOff_[i]));
      const float hg = (float)gyroHp_[i].process((double)(g[i] - gOff_[i]));
      ea += ha * ha;
      eg += hg * hg;
    }
    f_.acc = accRms_.process(sqrtf(ea));
    f_.gyro = gyroRms_.process(sqrtf(eg));
    f_.vib = vib;

    if (f_.calibrating) {
      calibrateStep();
      f_.score = 0;
      f_.motion = true;  // gate the PPG until we know what "still" looks like
      return f_;
    }

    f_.bodyRatio = fmaxf(f_.acc / thr_[0], f_.gyro / thr_[1]);
    f_.score = fmaxf(f_.bodyRatio, f_.vib / thr_[2]);
    if (f_.score > 1.0f) {
      f_.motion = true;
      quiet_ = 0;
    } else if (f_.motion) {
      quiet_ = (f_.score < exitRatio) ? quiet_ + 1 : 0;
      if (quiet_ >= (int)(exitHoldSec * fs_)) f_.motion = false;
    }
    adaptBaseline();
    return f_;
  }

  const MotionFeatures& features() const { return f_; }
  float threshold(int i) const { return thr_[i]; }
  float baseline(int i) const { return base_[i]; }
  int calRestarts() const { return calRestarts_; }
  bool calFallback() const { return calFallback_; }

 private:
  void calibrateStep() {
    ++n_;
    const int warm = (int)(warmupSec * fs_);
    if (n_ <= warm) return;
    const float v[3] = {f_.acc, f_.gyro, f_.vib};
    for (int i = 0; i < 3; ++i) { sum_[i] += v[i]; sq_[i] += v[i] * v[i]; }
    const int m = n_ - warm;
    if (m < (int)(calSec * fs_)) return;

    const float floors[3] = {accFloor, gyroFloor, vibFloor};
    for (int i = 0; i < 3; ++i) {
      const float mean = sum_[i] / m;
      const float sd = sqrtf(fmaxf(sq_[i] / m - mean * mean, 0.0f));
      base_[i] = mean;
      sdCal_[i] = sd;
      thr_[i] = fmaxf(fmaxf(kMul * mean, mean + kStd * sd), floors[i]);
    }
    if (base_[0] > calMaxAcc && calRestarts_ >= maxCalRestarts) {
      // The wearer never kept still (e.g. standing on a vibrating platform).
      // Use the absolute floors: more of the time counts as "motion", which
      // is the safe direction (spectral tracking, no clean-PPG assumptions).
      for (int i = 0; i < 3; ++i) { base_[i] = 0; sdCal_[i] = 0; thr_[i] = floors[i]; thr0_[i] = floors[i]; }
      calFallback_ = true;
      f_.calibrating = false;
      f_.motion = false;
      quiet_ = 0;
      initAuto();
      return;
    }
    if (base_[0] > calMaxAcc) {  // subject was moving -> try again
      const int r = calRestarts_ + 1;
      startCalibration();
      calRestarts_ = r;
      n_ = warm;
      return;
    }
    f_.calibrating = false;
    f_.motion = false;
    quiet_ = 0;
    for (int i = 0; i < 3; ++i) thr0_[i] = thr_[i];
    initAuto();
  }

  void initAuto() {
    for (int i = 0; i < 3; ++i) {
      // Running mean / mean-square used by the slow baseline adaptation.
      // After calibration they equal the calibration statistics.
      aMean_[i] = base_[i];
      aSq_[i] = base_[i] * base_[i] + sdCal_[i] * sdCal_[i];
    }
    autoQuiet_ = 0;
  }

  // While quiet, track the baseline slowly so the thresholds follow a changing
  // environment. Bounded so a slow drift into real motion cannot hide it.
  void adaptBaseline() {
    if (f_.motion || f_.score >= autoQuietRatio) { autoQuiet_ = 0; return; }
    if (++autoQuiet_ < (int)(autoQuietSec * fs_)) return;
    const float floors[3] = {accFloor, gyroFloor, vibFloor};
    const float al = 1.0f / (autoTauSec * fs_);
    const float v[3] = {f_.acc, f_.gyro, f_.vib};
    for (int i = 0; i < 3; ++i) {
      aMean_[i] += al * (v[i] - aMean_[i]);
      aSq_[i] += al * (v[i] * v[i] - aSq_[i]);
      const float sd = sqrtf(fmaxf(aSq_[i] - aMean_[i] * aMean_[i], 0.0f));
      float t = fmaxf(fmaxf(kMul * aMean_[i], aMean_[i] + kStd * sd), floors[i]);
      const float hi = autoMaxGrow * thr0_[i];
      thr_[i] = fminf(t, fmaxf(hi, floors[i]));
    }
  }

  float fs_ = 100;
  dsp::BiquadD accHp_[3], gyroHp_[3];
  dsp::MovingRms<50> accRms_, gyroRms_;
  MotionFeatures f_;
  float thr_[3] = {1, 1, 1}, thr0_[3] = {1, 1, 1}, base_[3] = {0, 0, 0};
  float aMean_[3] = {0}, aSq_[3] = {0};
  float sdCal_[3] = {0};
  bool calFallback_ = false;
  int autoQuiet_ = 0;
  float aOff_[3] = {0}, gOff_[3] = {0};
  bool haveOffset_ = false;
  double sum_[3] = {0}, sq_[3] = {0};
  int n_ = 0, quiet_ = 0, calRestarts_ = 0;
};
