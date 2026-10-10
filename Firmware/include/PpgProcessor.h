#pragma once
// ---------------------------------------------------------------------------
// Complete PPG + motion processing chain (pure C++, PC-testable).
// Call process() once per PPG sample (100 Hz) with the latest 100 Hz IMU data.
//
//   IR raw ─► Hampel spike filter ─► DC removal (EMA, 1 s) ─► 4th-order
//   Butterworth band-pass 0.5–4 Hz (double precision) ─► NLMS motion canceller
//   (references: 3 accel axes + 3 gyro axes [+ red channel, optional])
//   ─► invert ─► clean PPG
//
//   Three motion levels decide how heart rate is measured:
//     Still          : time-domain HR (beat peaks + autocorrelation), accurate
//     MotionTracking : moderate / periodic work -> spectral HR with the
//                      accelerometer frequencies removed (SpectralHr.h)
//     HeavyMotion    : PPG destroyed (lifting, bumps, pressure jumps) -> no
//                      measurement, the Kalman tracker predicts (holds) the HR
//                      for up to hrHoldSec, then reports "unknown" (0)
//   All estimates feed one Kalman tracker (HrTracker) -> final heart rate.
//   Per-beat template matching (BeatQuality) sets the tracker's measurement
//   noise when still; hrConf (0..100) combines every quality indicator and is
//   halved while the HR sits on the step cadence (cadenceLock).
//
//   DC level ─► automatic IR LED current control (start at maximum current,
//               back off only if the ADC gets close to saturation)
// ---------------------------------------------------------------------------
#include "Filters.h"
#include "HeartRate.h"
#include "MotionDetector.h"
#include "SpectralHr.h"

enum class PpgState : uint8_t {
  Calibrating = 0,     // learning the "still" motion baseline
  NoContact = 1,       // IR DC too low: sensor not on skin
  HeavyMotion = 2,     // PPG unusable -> HR held by the tracker
  Settling = 3,        // heavy motion just ended / LED changed -> filters settling
  Still = 4,           // time-domain HR (most accurate) + clean PPG waveform
  MotionTracking = 5   // moderate motion -> spectral HR, PPG is best-effort
};

struct PpgConfig {
  float fs = 100.0f;
  float bpLow = 0.5f, bpHigh = 4.0f;   // 30 .. 240 bpm pass-band
  float minBpm = 40.0f, maxBpm = 200.0f;
  float dcTauSec = 1.0f;
  float settleSec = 1.5f;              // hold-off after heavy motion / contact loss
  float ledSettleSec = 1.0f;           // hold-off after an LED current change
  float adaptRatio = 0.4f;             // NLMS adapts only when body-motion ratio > this
  float nlmsMu = 0.02f;
  // Heavy motion = PPG can no longer be used, even with the spectral method
  float heavyOpticalFrac = 0.03f;      // in-band PPG RMS > 3 % of DC (pulse is ~0.1 %)
  float heavyAccG = 0.7f;              // dynamic acceleration RMS > 0.7 g
  float opticalStepFrac = 0.05f;       // sudden 5 % jump in IR level (pressure change)
  float opticalHoldSec = 0.5f;
  float hrHoldSec = 20.0f;             // keep showing predicted HR this long without data
  float restMarginBpm = 10.0f;         // during motion HR >= resting HR - this
  // LED / ADC
  float fullScale = 262143.0f;         // 18-bit
  float ledHighFrac = 0.80f;           // decrease current above this DC level
  float ledSatFrac = 0.93f;            // ... or if any sample gets this high
  float ledLowFrac = 0.25f;            // increase current below this DC level
  uint8_t paMin = 0x14, paMax = 0xFF;  // 4 mA .. 51 mA
  float contactDcAtMax = 25000.0f;     // IR DC needed to call it "on skin" at 51 mA
  float acfMinSec = 4.0f;              // autocorrelation needs >= 4 s of data
  float sqiMin = 0.35f;
  // Motion references for the NLMS canceller
  bool useGyroRef = false;             // gyro axes as extra NLMS references (rotation changes sensor
                                       // angle / pressure). In simulation: more coverage, slightly more
                                       // error. The sim has no rotation->optics coupling, so decide on real data.
  float gyroRefScale = 10.0f;          // dps -> comparable magnitude to accel in mg
  bool useRedRef = false;              // EXPERIMENTAL: red channel as extra reference.
                                       // The pulse is also in red, so this can attenuate the
                                       // pulse itself: enable only after checking on real data.
  float redRefScale = 1.0e5f;          // red band-pass / red DC  ->  ~10 ppm units
  // Confidence / cadence guard
  float cadenceBandBpm = 12.0f;        // HR within this of cadence (or harmonics) = suspect
  float cadenceHoldSec = 6.0f;         // cadence estimate valid this long after last window
  float dataGapSec = 2.5f;             // hold-off after a FIFO overflow / ADC overload
};

struct PpgOutput {
  float irRaw = 0;       // 18-bit counts
  float dc = 0;          // counts
  float ppgFiltered = 0; // band-passed, before motion cancellation (always runs)
  float ppg = 0;         // PPG waveform (0 when unusable)
  bool ppgValid = false;
  bool beat = false;
  bool opticalArtifact = false;
  float hr = 0;          // final heart rate (Kalman tracker)
  bool hrValid = false;
  float hrTd = 0;        // time-domain estimate (Still only)
  float hrSpec = 0, specConf = 0;  // last spectral estimate and its confidence
  float hrPeak = 0, hrAcf = 0, sqi = 0, pi = 0;  // pi = perfusion index %
  float hrConf = 0;      // 0..100: trust in `hr` (0 when hrValid is false)
  float beatQ = 0;       // 0..1 per-beat template correlation quality (still only)
  bool cadenceLock = false;  // HR within a few bpm of the step cadence or a harmonic
  float cadenceBpm = 0;
  bool secondTick = false;  // true once per second (HR/LED updated)
  int ledRequest = -1;      // >=0: please set IR LED PA register to this
  PpgState state = PpgState::Calibrating;
  MotionFeatures motion;
};

class PpgProcessor {
 public:
  PpgConfig cfg;

  void begin(uint8_t irPa) {
    const float fs = cfg.fs;
    irPa_ = irPa;
    dcEma_.setTau(fs, cfg.dcTauSec);
    dcSlow_.setTau(fs, 3.0f);
    level_.setTau(fs, 0.03f);  // fast level for contact detection
    bp_.design(fs, cfg.bpLow, cfg.bpHigh);
    for (int i = 0; i < 3; ++i) refBp_[i].design(fs, cfg.bpLow, cfg.bpHigh);
    for (int i = 0; i < 3; ++i) refBpG_[i].design(fs, cfg.bpLow, cfg.bpHigh);
    bpRed_.design(fs, cfg.bpLow, cfg.bpHigh);
    redDc_.setTau(fs, cfg.dcTauSec);
    nlms_.configure(cfg.nlmsMu, 0.9999f, 0.995f, 1.0f);
    bq_.begin(fs);
    motion_.begin(fs);
    peaks_.begin(fs);
    acf_.reset();
    spec_.minBpm = cfg.minBpm;
    spec_.maxBpm = cfg.maxBpm;
    spec_.begin(fs);
    tracker_.reset();
    resetFrontEnd();
    n_ = 0;
    settleUntil_ = 0;
  }

  void recalibrateMotion() { motion_.startCalibration(); }

  // Call after the firmware has written the new LED current to the sensor.
  void notifyLedChanged(uint8_t pa) {
    irPa_ = pa;
    resetFrontEnd();
    nlms_.reset();  // optical gain changed -> motion coupling changed
    dcSlow_.reset();
    const long until = n_ + (long)(cfg.ledSettleSec * cfg.fs);
    if (until > settleUntil_) settleUntil_ = until;
  }

  // Call when samples may have been lost (FIFO overflow) or the optical front end
  // was overloaded (ambient light ADC overflow): the frequency axis / waveform is
  // corrupted, so the next dataGapSec are excluded from every estimator.
  void notifyGap() {
    const long until = n_ + (long)(cfg.dataGapSec * cfg.fs);
    if (until > settleUntil_) settleUntil_ = until;
    bq_.softReset();
  }

  const MotionDetector& motion() const { return motion_; }
  uint8_t irPa() const { return irPa_; }

  // acc in g, gyro in deg/s, vib in g (from ImuFrontEnd)
  PpgOutput process(uint32_t irRaw, const float acc[3], const float gyro[3], float vib) {
    return process(irRaw, 0, acc, gyro, vib);
  }

  PpgOutput process(uint32_t irRaw, uint32_t redRaw, const float acc[3], const float gyro[3],
                    float vib) {
    PpgOutput o;
    ++n_;
    o.irRaw = (float)irRaw;
    o.motion = motion_.update(acc, gyro, vib);

    // --- PPG front end -------------------------------------------------------
    const float x = hampel_.process(o.irRaw);
    const float dc = dcEma_.process(x);
    o.dc = dcSlow_.process(x);
    const float level = level_.process(o.irRaw);
    o.ppgFiltered = bp_.process(x - dc);
    const float ppgRms = ppgRms_.process(o.ppgFiltered);
    if (dc > 0 && fabsf(x - dc) > cfg.opticalStepFrac * dc)
      opticalUntil_ = n_ + (long)(cfg.opticalHoldSec * cfg.fs);
    o.opticalArtifact = n_ < opticalUntil_;
    if (o.irRaw > secMax_) secMax_ = o.irRaw;

    // --- motion reference (accelerometer, same band as the PPG) -------------
    float ref[kRefCh] = {0};
    if (n_ == 1) for (int i = 0; i < 3; ++i) { accOff_[i] = acc[i]; gyroOff_[i] = gyro[i]; }
    for (int i = 0; i < 3; ++i) ref[i] = refBp_[i].process((acc[i] - accOff_[i]) * 1000.0f);  // mg
    if (cfg.useGyroRef)  // optional references: filters only run when enabled (double precision = CPU)
      for (int i = 0; i < 3; ++i)
        ref[3 + i] = refBpG_[i].process((gyro[i] - gyroOff_[i]) * cfg.gyroRefScale);
    if (cfg.useRedRef && redRaw > 0) {
      const float rdc = redDc_.process((float)redRaw);
      const float rbp = bpRed_.process((float)redRaw - rdc);
      if (rdc > 1000.0f) ref[6] = rbp / rdc * cfg.redRefScale;
    }
    const bool contact = updateContact(level);
    // Adapt only on real in-band body motion (not vibration / sensor noise), and
    // never on a huge input (contact change, LED step) that would wreck the weights.
    const bool adapt = contact && !o.motion.calibrating &&
                       o.motion.bodyRatio > cfg.adaptRatio &&
                       fabsf(o.ppgFiltered) < 0.02f * cfg.fullScale;
    const float e = nlms_.process(o.ppgFiltered, ref, adapt);
    // Reflectance PPG falls when blood volume rises, so invert: peaks = systole.
    const float clean = -e;

    // --- motion level / state machine -----------------------------------------
    const bool heavy = o.opticalArtifact || (o.dc > 0 && ppgRms > cfg.heavyOpticalFrac * o.dc) ||
                       o.motion.acc > cfg.heavyAccG;
    PpgState st;
    if (o.motion.calibrating) st = PpgState::Calibrating;
    else if (!contact) st = PpgState::NoContact;
    else if (heavy) st = PpgState::HeavyMotion;
    else if (n_ < settleUntil_) st = PpgState::Settling;
    else if (o.motion.motion) st = PpgState::MotionTracking;
    else st = PpgState::Still;

    const bool gated = (st == PpgState::Calibrating || st == PpgState::NoContact ||
                        st == PpgState::HeavyMotion);
    if (gated) {
      settleUntil_ = n_ + (long)(cfg.settleSec * cfg.fs);
      wasGated_ = true;
    } else if (wasGated_) {
      // Heavy motion just ended: restart the filters from the current level so
      // the big transient does not ring through the band-pass.
      resetFrontEnd();
      wasGated_ = false;
    }
    if (st != PpgState::Still) {
      // Time-domain HR only uses still data: clear its history.
      peaks_.reset();
      acf_.reset();
      acRms_.reset();
      bq_.softReset();
    }
    if (st == PpgState::NoContact && contactLostTick_ == 0) {
      tracker_.reset();  // device removed: previous HR is meaningless
      spec_.reset();
      restHr_ = 0;       // maybe a different person next
      bq_.fullReset();   // ... or the sensor was re-placed: relearn the beat shape
      contactLostTick_ = 1;
    } else if (st != PpgState::NoContact) {
      contactLostTick_ = 0;
    }
    o.state = st;

    // --- PPG waveform + time-domain beats -----------------------------------
    if (st == PpgState::Still || st == PpgState::MotionTracking) {
      o.ppg = clean;
      o.ppgValid = true;
    }
    if (st == PpgState::Still) {
      o.beat = peaks_.process(clean);
      bq_.push(clean);
      if (o.beat) bq_.onBeat();
      acf_.push(clean);
      acRms_.process(clean);
    }

    // --- spectral HR (runs in all on-skin states; bad samples are marked) ----
    if (contact && !o.motion.calibrating) {
      SpectralResult r;
      const bool bad = gated || st == PpgState::Settling;
      if (spec_.push(clean, ref, bad, tracker_.bpm(), tracker_.locked(), r)) {
        lastSpec_ = r;
        if (r.cadenceBpm > 0) { cadenceBpm_ = r.cadenceBpm; cadenceAt_ = n_; }
        // When still, the time-domain estimate is better; spectral is only used
        // to acquire if nothing else has locked yet.
        if (r.valid && (st != PpgState::Still || !tracker_.locked()))
          tracker_.updateSpectral(r.bpm, r.conf, r.motion, r.motionAt, r.motionAtPred);
      }
    }

    // --- once per second: time-domain HR, tracker prediction, LED control ----
    if (++tick_ >= (int)cfg.fs) {
      tick_ = 0;
      o.secondTick = true;
      tracker_.predict(1.0f);
      if (st == PpgState::Still) {
        updateHeartRate();
        if (hrTd_ > 0 && n_ - lastTdUpdate_ < (long)cfg.fs) {
          // Measurement noise from per-beat quality: 2.5 bpm when beats match the
          // template, up to 9.5 bpm when they do not.
          tracker_.updateStill(hrTd_, 2.5f + 7.0f * (1.0f - bq_.quality()));
          // resting HR: follows decreases quickly, increases slowly
          if (restHr_ <= 0) restHr_ = hrTd_;
          else restHr_ += (hrTd_ < restHr_ ? 0.2f : 0.01f) * (hrTd_ - restHr_);
        }
      }
      // During motion HR does not fall below rest: use it as a floor.
      tracker_.setFloor((st == PpgState::MotionTracking || st == PpgState::HeavyMotion) && restHr_ > 0
                            ? restHr_ - cfg.restMarginBpm : 0.0f);
      if (st == PpgState::Still && n_ >= settleUntil_)
        o.ledRequest = ledControl(o.dc);
      secMax_ = 0;
    }
    if (n_ - lastTdUpdate_ > (long)(10 * cfg.fs)) hrTd_ = 0;  // stale -> relock later

    o.hrTd = (st == PpgState::Still) ? hrTd_ : 0;
    o.hrSpec = lastSpec_.valid ? lastSpec_.bpm : 0;
    o.specConf = lastSpec_.valid ? lastSpec_.conf : 0;
    o.hr = tracker_.bpm();
    o.hrValid = tracker_.locked() && tracker_.sinceUpdate() <= cfg.hrHoldSec &&
                st != PpgState::Calibrating && st != PpgState::NoContact;
    o.hrPeak = hrPeak_;
    o.hrAcf = hrAcf_;
    o.sqi = sqi_;
    o.beatQ = (st == PpgState::Still) ? bq_.quality() : 0.0f;
    o.cadenceBpm = (n_ - cadenceAt_ < (long)(cfg.cadenceHoldSec * cfg.fs)) ? cadenceBpm_ : 0.0f;
    if (o.secondTick) {
      cadenceLock_ = cadenceNearHr(o.cadenceBpm, o.hr, st);
      hrConf_ = computeConf(st, o);
    }
    if (!o.hrValid) hrConf_ = 0;
    o.cadenceLock = cadenceLock_;
    o.hrConf = hrConf_;
    o.pi = (o.dc > 0 && st == PpgState::Still) ? 100.0f * 2.83f * acRms_.value() / o.dc : 0;
    return o;
  }

 private:
  // HR on top of the step cadence (or its harmonics / the stride rate) is the
  // classic wrong answer of motion-corrupted PPG: flag it.
  bool cadenceNearHr(float cad, float hr, PpgState st) const {
    if (cad <= 0 || hr <= 0) return false;
    if (st != PpgState::MotionTracking && st != PpgState::HeavyMotion) return false;
    const float m[4] = {0.5f * cad, cad, 2.0f * cad, 3.0f * cad};
    for (int i = 0; i < 4; ++i)
      if (fabsf(hr - m[i]) < cfg.cadenceBandBpm) return true;
    return false;
  }

  float computeConf(PpgState st, const PpgOutput& o) const {
    if (!o.hrValid) return 0;
    float base = 0;
    switch (st) {
      case PpgState::Still: {
        const float sqiN = fminf(sqi_ / 0.8f, 1.0f);
        base = 0.5f * sqiN + 0.5f * bq_.quality();
        break;
      }
      case PpgState::MotionTracking: {
        const float c = fminf(lastSpec_.conf / 0.5f, 1.0f);
        const float onMotion = fminf(fmaxf(lastSpec_.motionAtPred, 0.0f), 1.0f);
        base = 0.85f * c * (1.0f - 0.6f * onMotion);
        break;
      }
      case PpgState::Settling: base = 0.3f; break;
      default: base = 0.2f; break;  // HeavyMotion: predicted only
    }
    const float fresh = fminf(fmaxf(1.0f - tracker_.sinceUpdate() / cfg.hrHoldSec, 0.0f), 1.0f);
    float c = base * fresh;
    if (cadenceLock_) c *= 0.5f;
    return 100.0f * fminf(fmaxf(c, 0.0f), 1.0f);
  }

  void resetFrontEnd() {
    hampel_.reset();
    dcEma_.reset();
    bp_.reset();
  }

  bool updateContact(float dc) {
    const float thr = cfg.contactDcAtMax * (float)irPa_ / 255.0f;
    if (contact_ && dc < 0.8f * thr) {
      contact_ = false;
      nlms_.reset();
    } else if (!contact_ && dc > thr) {
      contact_ = true;
      nlms_.reset();
      resetFrontEnd();  // re-seed all DC trackers / filters from the new skin level
      dcSlow_.reset();
      opticalUntil_ = 0;
    }
    return contact_;
  }

  // Time-domain HR (still only): beat peaks + autocorrelation, fused.
  void updateHeartRate() {
    const bool pkOk = peaks_.valid();
    hrPeak_ = pkOk ? peaks_.bpm() : 0;
    float bpm = 0, q = 0;
    const bool acOk = acf_.estimate(cfg.fs, cfg.minBpm, cfg.maxBpm, cfg.acfMinSec, bpm, q) &&
                      q >= cfg.sqiMin;
    hrAcf_ = acOk ? bpm : 0;
    sqi_ = acOk ? q : 0;

    float cand = 0;
    if (pkOk && acOk) {
      if (fabsf(hrPeak_ - hrAcf_) <= 0.12f * hrAcf_) cand = 0.5f * (hrPeak_ + hrAcf_);
      else if (sqi_ >= 0.6f || hrTd_ <= 0) cand = hrAcf_;
      else cand = (fabsf(hrPeak_ - hrTd_) < fabsf(hrAcf_ - hrTd_)) ? hrPeak_ : hrAcf_;
    } else if (acOk) {
      cand = hrAcf_;
    } else if (pkOk && acf_.count() < (int)(cfg.acfMinSec * cfg.fs)) {
      cand = hrPeak_;  // first seconds after motion: peaks only
    }
    if (cand <= 0) return;
    if (hrTd_ <= 0) hrTd_ = cand;
    else {
      float d = cand - hrTd_;
      if (d > 12) d = 12;          // limit the jump per second
      if (d < -12) d = -12;
      hrTd_ += 0.5f * d;
    }
    lastTdUpdate_ = n_;
  }

  int ledControl(float dc) {
    int pa = irPa_;
    if (secMax_ > cfg.ledSatFrac * cfg.fullScale || dc > cfg.ledHighFrac * cfg.fullScale)
      pa = (int)(pa * 0.8f);
    else if (dc < cfg.ledLowFrac * cfg.fullScale && pa < cfg.paMax)
      pa = (int)(pa * 1.25f) + 1;
    if (pa < cfg.paMin) pa = cfg.paMin;
    if (pa > cfg.paMax) pa = cfg.paMax;
    return (pa != irPa_) ? pa : -1;
  }

  dsp::Hampel<7> hampel_{3.5f};
  dsp::Ema dcEma_, dcSlow_, level_;
  dsp::Butter4BandPass bp_, refBp_[3], refBpG_[3], bpRed_;
  dsp::Ema redDc_;
  BeatQuality bq_;
  static constexpr int kRefCh = 7;  // 3 accel + 3 gyro + 1 red
  dsp::NlmsCanceller<kRefCh, 16> nlms_;
  dsp::MovingRms<200> acRms_;
  dsp::MovingRms<100> ppgRms_;
  MotionDetector motion_;
  PeakDetector peaks_;
  AcfEstimator<800> acf_;
  SpectralHrEstimator spec_;
  HrTracker tracker_;
  SpectralResult lastSpec_;

  uint8_t irPa_ = 0xFF;
  bool contact_ = false, wasGated_ = true;
  long n_ = 0, settleUntil_ = 0, lastTdUpdate_ = -100000, opticalUntil_ = 0;
  int tick_ = 0, contactLostTick_ = 0;
  float secMax_ = 0;
  float accOff_[3] = {0, 0, 0}, gyroOff_[3] = {0, 0, 0};
  float cadenceBpm_ = 0, hrConf_ = 0;
  long cadenceAt_ = -100000;
  bool cadenceLock_ = false;
  float restHr_ = 0;
  float hrTd_ = 0, hrPeak_ = 0, hrAcf_ = 0, sqi_ = 0;
};
