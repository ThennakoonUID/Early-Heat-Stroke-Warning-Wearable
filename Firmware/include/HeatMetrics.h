#pragma once
// ---------------------------------------------------------------------------
// The numbers that go to Firebase and drive the buzzer. Pure C++ (no Arduino
// calls), so it can be tested on a PC.
//
//   SkinTemp      two thermistors -> one skin temperature (or an error)
//   sweatLevel()  skin conductance (uS) -> 1..10
//   AlertLogic    skin / heart rate / sweat -> heat-stroke alert (buzzer)
// ---------------------------------------------------------------------------
#include <math.h>
#include <stdint.h>

// ============================== skin temperature ============================
enum class SkinStatus : uint8_t {
  Ok,         // both thermistors agree: average
  OneSensor,  // one thermistor rejected: only the other one is used
  Error       // neither can be skin: no contact
};

struct SkinResult {
  SkinStatus status = SkinStatus::Error;
  float tempC = 0;       // valid unless status == Error
  bool used[2] = {false, false};
  const char* note = "";
};

struct SkinParams {
  float minC = 30.0f, maxC = 43.0f;  // plausible skin window
  float maxDiffC = 2.0f;             // thermistors further apart than this: one is bad
  float jumpC = 3.0f;                // distance from a sensor's own recent value = "sudden jump"
  uint32_t staleMs = 60000;          // forget recent values after this long without a good reading
  float emaAlpha = 0.15f;            // recent value tracking per update (~1 Hz)
};

class SkinTemp {
 public:
  SkinParams p;

  // t[i], valid[i]: thermistor readings (valid = electrically sane, from ntcCelsiusFromRaw()).
  // ambientC: SHT30 temperature, NAN if not available. nowMs: millis().
  SkinResult update(const float t[2], const bool valid[2], float ambientC, uint32_t nowMs) {
    SkinResult r;
    if (haveRef_[0] || haveRef_[1])
      if (nowMs - lastGoodMs_ > p.staleMs) haveRef_[0] = haveRef_[1] = false;

    bool plaus[2];
    for (int i = 0; i < 2; ++i) plaus[i] = valid[i] && t[i] >= p.minC && t[i] <= p.maxC;

    if (!plaus[0] && !plaus[1]) {
      r.note = "both thermistors outside the skin range";
      return r;  // Error
    }

    if (plaus[0] != plaus[1]) {  // one thermistor is clearly not on skin
      const int g = plaus[0] ? 0 : 1;
      if (jumped(g, t[g])) {  // ... and the other one just jumped as well: cannot trust it
        r.note = "one thermistor off-range and the other jumped";
        return r;
      }
      return accept(r, SkinStatus::OneSensor, t[g], g, t, nowMs, "one thermistor off-range");
    }

    // both plausible
    const bool j0 = jumped(0, t[0]), j1 = jumped(1, t[1]);
    if (j0 && j1) {
      r.note = "both thermistors jumped at the same time";
      return r;  // Error
    }
    if (fabsf(t[0] - t[1]) <= p.maxDiffC) {
      r.used[0] = r.used[1] = true;
      return accept(r, SkinStatus::Ok, 0.5f * (t[0] + t[1]), -1, t, nowMs, "");
    }

    // They disagree: reject the one that jumped. Without history, reject the one closer
    // to the ambient temperature (a sensor that left the skin drifts towards the air).
    int bad;
    if (haveRef_[0] && haveRef_[1]) bad = fabsf(t[0] - ref_[0]) >= fabsf(t[1] - ref_[1]) ? 0 : 1;
    else if (!isnan(ambientC)) bad = fabsf(t[0] - ambientC) <= fabsf(t[1] - ambientC) ? 0 : 1;
    else bad = t[0] < t[1] ? 0 : 1;
    const int g = 1 - bad;
    return accept(r, SkinStatus::OneSensor, t[g], g, t, nowMs, bad == 0 ? "T1 rejected" : "T2 rejected");
  }

 private:
  bool jumped(int i, float v) const { return haveRef_[i] && fabsf(v - ref_[i]) > p.jumpC; }

  // good = index of the single accepted sensor, -1 = both
  SkinResult accept(SkinResult r, SkinStatus st, float temp, int good, const float t[2], uint32_t now,
                    const char* note) {
    r.status = st;
    r.tempC = temp;
    r.note = note;
    for (int i = 0; i < 2; ++i) {
      const bool use = good < 0 || good == i;
      r.used[i] = use;
      if (!use) continue;
      if (!haveRef_[i]) { ref_[i] = t[i]; haveRef_[i] = true; }
      else ref_[i] += p.emaAlpha * (t[i] - ref_[i]);
    }
    lastGoodMs_ = now;
    return r;
  }

  float ref_[2] = {0, 0};
  bool haveRef_[2] = {false, false};
  uint32_t lastGoodMs_ = 0;
};

// ================================ sweat level ===============================
// Skin conductance -> 1..10 on a log scale: <= gMinUs gives 1, >= gMaxUs gives 10.
inline int sweatLevelFromUs(float gUs, float gMinUs, float gMaxUs) {
  if (!(gUs > gMinUs)) return 1;
  if (gUs >= gMaxUs) return 10;
  const float x = logf(gUs / gMinUs) / logf(gMaxUs / gMinUs);  // 0..1
  int lvl = 1 + (int)lroundf(9.0f * x);
  return lvl < 1 ? 1 : (lvl > 10 ? 10 : lvl);
}

// Smooths the GSR value and rides through short gaps (a GSR window that was
// disturbed, e.g. by the WiFi stack, is simply skipped).
class SweatTracker {
 public:
  // gUs/valid: latest GSR snapshot (valid = contact with the skin and a good measurement)
  void update(bool valid, float gUs, uint32_t nowMs) {
    if (valid) {
      g_ = have_ ? g_ + 0.3f * (gUs - g_) : gUs;
      have_ = true;
      lastMs_ = nowMs;
    }
  }
  bool available(uint32_t nowMs, uint32_t holdMs) const { return have_ && (nowMs - lastMs_) <= holdMs; }
  float gUs() const { return g_; }
  void reset() { have_ = false; }  // electrodes off the skin: forget

 private:
  float g_ = 0;
  bool have_ = false;
  uint32_t lastMs_ = 0;
};

// ================================== alert ===================================
struct AlertInputs {
  bool skinValid = false;  float skinC = 0;
  bool hrValid = false;    float hr = 0;
  bool sweatValid = false; int sweat = 0;
};

struct AlertParams {
  float skinC = 39.0f, hrHigh = 120.0f, hrLow = 50.0f;
  int sweatMax = 1;
  uint32_t holdSec = 3, sweatHoldSec = 20;
};

struct AlertState {
  bool alert = false;
  bool skinHot = false, hrHigh = false, hrLow = false, sweatLow = false;
};

// Call once per second. A condition has to stay true for its hold time before it counts,
// and it is released as soon as it is gone (or its sensor is no longer valid).
class AlertLogic {
 public:
  AlertParams p;
  AlertState update(const AlertInputs& in) {
    AlertState s;
    s.skinHot = step(0, in.skinValid && in.skinC > p.skinC, p.holdSec);
    s.hrHigh = step(1, in.hrValid && in.hr > p.hrHigh, p.holdSec);
    s.hrLow = step(2, in.hrValid && in.hr < p.hrLow, p.holdSec);
    s.sweatLow = step(3, in.sweatValid && in.sweat <= p.sweatMax, p.sweatHoldSec);
    s.alert = s.skinHot || s.hrHigh || s.hrLow || s.sweatLow;
    return s;
  }

 private:
  bool step(int i, bool cond, uint32_t hold) {
    cnt_[i] = cond ? cnt_[i] + 1 : 0;
    return cnt_[i] >= hold;
  }
  uint32_t cnt_[4] = {0, 0, 0, 0};
};
