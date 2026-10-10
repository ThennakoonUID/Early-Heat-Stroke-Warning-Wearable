#pragma once
// ---------------------------------------------------------------------------
// Heat-stroke early-warning logic. Pure C++ (PC-testable): call update() once
// per second with the latest readings.
//
// This is a transparent points-based screening rule, NOT a validated medical
// algorithm. Every threshold is in HeatRiskConfig; tune them on real data and
// against a reference thermometer before relying on the alarm.
//
//   Body temperature level + rise rate ........ 0..6 points
//   Heart rate (rest: absolute, work: % HRmax)  0..3 points
//   Environment (heat index from SHT30) ....... 0..3 points
//   GSR sweat response (rise vs own baseline) . 0..2 points (+2: sweat failure)
//
//   total >= 3 CAUTION | >= 5 WARNING | >= 7 HEAT STROKE ALERT
//   Overrides (-> ALERT): body temp >= tempCritical, or a high body temp
//   together with a clearly raised heart rate or "sweat failure".
//
//   Debounce: a higher level must hold for a few seconds before it is shown;
//   the level steps down one stage only after downSec of lower readings.
// ---------------------------------------------------------------------------
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

// --------------------------- environment maths ------------------------------
// NWS heat index (Rothfusz regression). Input/output in Celsius, RH in %.
inline float heatIndexC(float tc, float rh) {
  const float t = tc * 9.0f / 5.0f + 32.0f;
  float hi = 0.5f * (t + 61.0f + (t - 68.0f) * 1.2f + rh * 0.094f);
  if ((hi + t) * 0.5f >= 80.0f) {
    hi = -42.379f + 2.04901523f * t + 10.14333127f * rh - 0.22475541f * t * rh -
         0.00683783f * t * t - 0.05481717f * rh * rh + 0.00122874f * t * t * rh +
         0.00085282f * t * rh * rh - 0.00000199f * t * t * rh * rh;
    if (rh < 13.0f && t >= 80.0f && t <= 112.0f)
      hi -= ((13.0f - rh) / 4.0f) * sqrtf((17.0f - fabsf(t - 95.0f)) / 17.0f);
    else if (rh > 85.0f && t >= 80.0f && t <= 87.0f)
      hi += ((rh - 85.0f) / 10.0f) * ((87.0f - t) / 5.0f);
  }
  return (hi - 32.0f) * 5.0f / 9.0f;
}

// --------------------------------- types ------------------------------------
enum class RiskStatus : int8_t {
  NoData = -2,   // no valid body temperature
  WarmUp = -1,   // sensors still settling after power-up
  Normal = 0,
  Caution = 1,
  Warning = 2,
  Alert = 3      // possible heat stroke
};

inline const char* riskText(RiskStatus s) {
  switch (s) {
    case RiskStatus::NoData: return "NO DATA (check temperature sensors)";
    case RiskStatus::WarmUp: return "WARM-UP";
    case RiskStatus::Normal: return "NORMAL";
    case RiskStatus::Caution: return "CAUTION - heat strain building";
    case RiskStatus::Warning: return "WARNING - heat exhaustion risk";
    case RiskStatus::Alert: return "HEAT STROKE ALERT";
  }
  return "?";
}

struct RiskInputs {
  bool bodyValid = false;  float bodyC = 0;                       // fused body temperature
  bool hrValid = false;    float hr = 0, hrConf = 0;              // from PpgOutput
  float actMg = 0;                                                // dynamic acceleration RMS (mg)
  bool envValid = false;   float ambC = 0, rh = 0;                // SHT30
  bool gsrValid = false;   float gsrUs = 0;                       // skin conductance (uS)
};

struct HeatRiskConfig {
  float warmupSec = 60.0f;
  float ageYears = 30.0f;
  // body temperature (C) -> points 1,2,3,4 ; Alert override at tempCritical
  float temp1 = 37.5f, temp2 = 38.0f, temp3 = 38.5f, temp4 = 39.0f, tempCritical = 39.5f;
  float rise1 = 0.5f, rise2 = 1.0f;       // C per 10 min -> +1, +2
  float riseMinTemp = 36.5f;              // rise points only count above this temperature
  // heart rate
  float minHrConf = 40.0f;
  float restActMg = 60.0f;                // below this the wearer is "at rest"
  float restHr1 = 100.0f, restHr2 = 120.0f, restHr3 = 140.0f;      // bpm at rest -> 1,2,3
  float workPct1 = 0.75f, workPct2 = 0.85f, workPct3 = 0.95f;      // fraction of HRmax -> 1,2,3
  // environment: heat index (C) -> 1,2,3 (NWS: caution 27, extreme caution 32, danger 39, extreme danger 51)
  float hi1 = 32.0f, hi2 = 39.0f, hi3 = 51.0f;
  // GSR relative to the person's own baseline
  float gsrBaselineSec = 60.0f;           // valid seconds used to learn the baseline
  float gsrRatio1 = 2.0f, gsrRatio2 = 4.0f;
  float gsrFailDrop = 0.5f;               // now < 50 % of recent peak, while hot => sweat failure
  // levels
  int scoreCaution = 3, scoreWarn = 5, scoreAlert = 7;
  int needUpCaution = 15, needUpWarn = 15, needUpAlert = 10, needUpCritical = 3;  // seconds
  int downSec = 30;
  int noDataSec = 10;
};

struct RiskOutput {
  RiskStatus status = RiskStatus::WarmUp;
  int score = 0;
  int ptsTemp = 0, ptsHr = 0, ptsEnv = 0, ptsGsr = 0;
  float bodySmooth = 0, slope10 = 0, hrMax = 0, hi = 0, gsrRatio = 0, restHr = 0;
  bool sweatFailure = false;
  char reasons[200] = {0};
};

// -------------------------------- engine ------------------------------------
class HeatRiskEngine {
 public:
  HeatRiskConfig cfg;

  RiskOutput update(const RiskInputs& in) {
    RiskOutput o;
    ++t_;
    const bool warm = t_ < cfg.warmupSec;
    char* r = o.reasons;
    size_t rn = sizeof(o.reasons);
    r[0] = 0;

    // ---- body temperature: smoothing, 5-min history, slope ----
    if (in.bodyValid) {
      noBodySec_ = 0;
      bodySm_ = haveBody_ ? bodySm_ + (in.bodyC - bodySm_) / 8.0f : in.bodyC;
      haveBody_ = true;
      if (++histTick_ >= 10) {  // one history point per 10 s
        histTick_ = 0;
        hist_[histHead_] = bodySm_;
        histHead_ = (histHead_ + 1) % kHist;
        if (histCount_ < kHist) ++histCount_;
      }
    } else {
      ++noBodySec_;
    }
    o.bodySmooth = bodySm_;
    if (histCount_ >= 12) {  // at least ~2 min of history
      const int oldest = (histHead_ - histCount_ + kHist * 2) % kHist;
      const float span = (histCount_ - 1) * 10.0f;  // seconds between oldest and newest point
      const float newest = hist_[(histHead_ - 1 + kHist) % kHist];
      o.slope10 = (newest - hist_[oldest]) * (600.0f / span);
    }

    // ---- heart rate ----
    const bool hrOk = in.hrValid && in.hrConf >= cfg.minHrConf;
    o.hrMax = 208.0f - 0.7f * cfg.ageYears;
    const bool atRest = in.actMg < cfg.restActMg;
    if (hrOk && atRest && in.hrConf >= 60.0f) {  // resting HR baseline: falls fast, rises slowly
      restHr_ = restHr_ <= 0 ? in.hr : restHr_ + (in.hr < restHr_ ? 0.2f : 0.01f) * (in.hr - restHr_);
    }
    o.restHr = restHr_;

    // ---- environment ----
    o.hi = in.envValid ? heatIndexC(in.ambC, in.rh) : 0.0f;

    // ---- GSR vs own baseline ----
    float ratio = 0;
    bool dryHot = false;
    if (in.gsrValid) {
      gsrSm_ = haveGsr_ ? gsrSm_ + (in.gsrUs - gsrSm_) / 5.0f : in.gsrUs;
      haveGsr_ = true;
      if (gsrBaseN_ < (int)cfg.gsrBaselineSec) {  // learn the baseline from the first minute
        gsrBaseSum_ += gsrSm_;
        ++gsrBaseN_;
        if (gsrBaseN_ == (int)cfg.gsrBaselineSec) gsrBase_ = gsrBaseSum_ / gsrBaseN_;
      } else {
        if (lastLevel_ == 0) gsrBase_ += (gsrSm_ - gsrBase_) / 1800.0f;  // slow drift, frozen once strain starts
        gsrPeak_ = gsrSm_ > gsrPeak_ ? gsrSm_ : gsrPeak_ + (gsrSm_ - gsrPeak_) / 300.0f;
      }
      if (gsrBase_ > 0.02f) ratio = gsrSm_ / gsrBase_;
      if (gsrBase_ > 0.02f && gsrPeak_ >= 2.0f * gsrBase_ && gsrSm_ < cfg.gsrFailDrop * gsrPeak_ &&
          haveBody_ && bodySm_ >= cfg.temp2)
        dryHot = true;
    }
    o.gsrRatio = ratio;
    o.sweatFailure = dryHot;

    // ---- points ----
    int pt = 0;
    if (haveBody_) {
      pt = bodySm_ >= cfg.temp4 ? 4 : bodySm_ >= cfg.temp3 ? 3 : bodySm_ >= cfg.temp2 ? 2 : bodySm_ >= cfg.temp1 ? 1 : 0;
      // a fast rise only counts once the temperature is already near the fever range
      if (bodySm_ >= cfg.riseMinTemp) pt += o.slope10 >= cfg.rise2 ? 2 : o.slope10 >= cfg.rise1 ? 1 : 0;
      if (pt > 0) append(r, rn, "Tb %.1fC", bodySm_);
      if (bodySm_ >= cfg.riseMinTemp && o.slope10 >= cfg.rise1) append(r, rn, "rising %.1fC/10min", o.slope10);
    }
    o.ptsTemp = pt;

    int ph = 0;
    if (hrOk) {
      if (atRest) {
        ph = in.hr >= cfg.restHr3 ? 3 : in.hr >= cfg.restHr2 ? 2 : in.hr >= cfg.restHr1 ? 1 : 0;
        if (ph > 0) append(r, rn, "HR %.0f at rest", in.hr);
      } else {
        const float pct = in.hr / o.hrMax;
        ph = pct >= cfg.workPct3 ? 3 : pct >= cfg.workPct2 ? 2 : pct >= cfg.workPct1 ? 1 : 0;
        if (ph > 0) append(r, rn, "HR %.0f (%.0f%% max)", in.hr, pct * 100.0f);
      }
    }
    o.ptsHr = ph;

    int pe = 0;
    if (in.envValid) {
      pe = o.hi >= cfg.hi3 ? 3 : o.hi >= cfg.hi2 ? 2 : o.hi >= cfg.hi1 ? 1 : 0;
      if (pe > 0) append(r, rn, "heat index %.0fC", o.hi);
    }
    o.ptsEnv = pe;

    int pg = ratio >= cfg.gsrRatio2 ? 2 : ratio >= cfg.gsrRatio1 ? 1 : 0;
    if (pg > 0) append(r, rn, "sweating x%.1f", ratio);
    if (dryHot) { pg += 2; append(r, rn, "SWEAT FAILURE while hot"); }
    o.ptsGsr = pg;

    o.score = o.ptsTemp + o.ptsHr + o.ptsEnv + o.ptsGsr;

    // ---- raw level + overrides ----
    int raw = o.score >= cfg.scoreAlert ? 3 : o.score >= cfg.scoreWarn ? 2 : o.score >= cfg.scoreCaution ? 1 : 0;
    bool critical = false;
    if (haveBody_ && bodySm_ >= cfg.tempCritical) critical = true;
    if (o.ptsTemp >= 3 && (o.ptsHr >= 2 || dryHot)) critical = true;
    if (critical) raw = 3;

    // ---- debounce / hysteresis ----
    if (warm) {
      o.status = RiskStatus::WarmUp;
      return o;
    }
    if (noBodySec_ >= cfg.noDataSec) {
      o.status = RiskStatus::NoData;
      return o;
    }
    if (raw > level_) {
      downCnt_ = 0;
      const int need = critical ? cfg.needUpCritical
                     : raw == 3 ? cfg.needUpAlert : raw == 2 ? cfg.needUpWarn : cfg.needUpCaution;
      if (++upCnt_ >= need) { level_ = raw; upCnt_ = 0; }
    } else if (raw < level_) {
      upCnt_ = 0;
      if (++downCnt_ >= cfg.downSec) { --level_; downCnt_ = 0; }
    } else {
      upCnt_ = downCnt_ = 0;
    }
    lastLevel_ = level_;
    o.status = (RiskStatus)level_;
    return o;
  }

 private:
  static void append(char* buf, size_t n, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
  static constexpr int kHist = 30;  // 30 x 10 s = 5 min
  float hist_[kHist] = {0};
  int histHead_ = 0, histCount_ = 0, histTick_ = 0;
  float bodySm_ = 0, restHr_ = 0, gsrSm_ = 0, gsrBase_ = 0, gsrBaseSum_ = 0, gsrPeak_ = 0;
  int gsrBaseN_ = 0, level_ = 0, lastLevel_ = 0, upCnt_ = 0, downCnt_ = 0, noBodySec_ = 0;
  bool haveBody_ = false, haveGsr_ = false;
  float t_ = 0;
};

inline void HeatRiskEngine::append(char* buf, size_t n, const char* fmt, ...) {
  const size_t len = strlen(buf);
  if (len + 4 >= n) return;
  size_t pos = len;
  if (len > 0) { buf[pos++] = ','; buf[pos++] = ' '; }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf + pos, n - pos, fmt, ap);
  va_end(ap);
}
