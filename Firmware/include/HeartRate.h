#pragma once
// ---------------------------------------------------------------------------
// Heart-rate estimators (pure C++, PC-testable).
//
//  PeakDetector : time-domain beat detection on the cleaned PPG.
//                 adaptive threshold (decaying amplitude envelope), a
//                 "must dip below zero before the next beat" arming rule that
//                 rejects the dicrotic notch, 0.3 s refractory period, and an
//                 inter-beat-interval (IBI) plausibility check against the
//                 median of recent IBIs.
//
//  AcfEstimator : frequency-domain-style estimate from the normalised
//                 autocorrelation of the last 4-8 s of PPG. Robust when single
//                 beats are hard to see (weak signal on the back). The height
//                 of the autocorrelation peak doubles as a signal-quality
//                 index (SQI): ~1 = clean periodic pulse, ~0 = noise.
// ---------------------------------------------------------------------------
#include "Filters.h"

class PeakDetector {
 public:
  void begin(float fs) {
    fs_ = fs;
    envDecay_ = powf(0.5f, 1.0f / (1.5f * fs));  // envelope halves in 1.5 s
    reset();
  }
  void reset() {
    x1_ = x2_ = 0; env_ = 0; armed_ = false; n_ = 0;
    lastPeak_ = -1; nIbi_ = 0; rejectRun_ = 0;
  }

  // Returns true when a beat is confirmed (the peak was the previous sample).
  bool process(float x) {
    ++n_;
    env_ = fmaxf(env_ * envDecay_, fabsf(x));
    bool beat = false;
    if (x < -0.15f * env_) armed_ = true;
    if (armed_ && x1_ > x2_ && x1_ >= x && x1_ > 0.3f * env_ && n_ > 2) {
      const long peak = n_ - 1;
      if (lastPeak_ < 0) {
        lastPeak_ = peak; armed_ = false; beat = true;
      } else {
        const float ibi = (peak - lastPeak_) / fs_;
        if (ibi >= 0.3f) {  // refractory: faster than 200 bpm is not a beat
          armed_ = false; beat = true;
          if (ibi <= 1.6f) acceptIbi(ibi);  // slower than ~37 bpm: gap, don't use
          lastPeak_ = peak;
        }
      }
    }
    x2_ = x1_;
    x1_ = x;
    return beat;
  }

  bool valid(float maxAgeSec = 2.5f) const {
    return nIbi_ >= 3 && lastPeak_ >= 0 && (n_ - lastPeak_) < maxAgeSec * fs_;
  }
  float bpm() const {
    if (nIbi_ < 1) return 0;
    return 60.0f / medianRecent(nIbi_ < 6 ? nIbi_ : 6);
  }
  float lastIbi() const { return nIbi_ ? ibi_[(head_ + kHist - 1) % kHist] : 0; }

 private:
  static constexpr int kHist = 8;
  void acceptIbi(float ibi) {
    if (nIbi_ >= 3) {
      const float med = medianRecent(nIbi_ < 6 ? nIbi_ : 6);
      if (fabsf(ibi - med) > 0.30f * med) {
        // Implausible jump. After 3 in a row, assume the rhythm really changed.
        if (++rejectRun_ < 3) return;
        nIbi_ = 0;
      }
    }
    rejectRun_ = 0;
    ibi_[head_] = ibi;
    head_ = (head_ + 1) % kHist;
    if (nIbi_ < kHist) ++nIbi_;
  }
  float medianRecent(int m) const {
    float t[kHist];
    for (int i = 0; i < m; ++i) t[i] = ibi_[(head_ + kHist - 1 - i) % kHist];
    for (int i = 1; i < m; ++i) {
      const float v = t[i];
      int j = i - 1;
      while (j >= 0 && t[j] > v) { t[j + 1] = t[j]; --j; }
      t[j + 1] = v;
    }
    return (m & 1) ? t[m / 2] : 0.5f * (t[m / 2 - 1] + t[m / 2]);
  }

  float fs_ = 100, envDecay_ = 0.995f;
  float x1_ = 0, x2_ = 0, env_ = 0;
  bool armed_ = false;
  long n_ = 0, lastPeak_ = -1;
  float ibi_[kHist] = {0};
  int head_ = 0, nIbi_ = 0, rejectRun_ = 0;
};

template <int N>
class AcfEstimator {
 public:
  void reset() { count_ = 0; idx_ = 0; }
  void push(float x) {
    buf_[idx_] = x;
    idx_ = (idx_ + 1) % N;
    if (count_ < N) ++count_;
  }
  int count() const { return count_; }

  // Returns false if there is not enough data or no periodicity was found.
  bool estimate(float fs, float minBpm, float maxBpm, float minSec,
                float& bpm, float& quality) {
    const int n = count_;
    if (n < (int)(minSec * fs)) return false;
    // Linearise the ring buffer (oldest first) and remove the mean.
    const int start = (idx_ - n + N) % N;
    float mean = 0;
    for (int i = 0; i < n; ++i) { tmp_[i] = buf_[(start + i) % N]; mean += tmp_[i]; }
    mean /= n;
    float r0 = 0;
    for (int i = 0; i < n; ++i) { tmp_[i] -= mean; r0 += tmp_[i] * tmp_[i]; }
    if (r0 <= 0) return false;
    r0 /= n;

    const int kMin = (int)floorf(60.0f * fs / maxBpm) - 1;
    int kMax = (int)ceilf(60.0f * fs / minBpm) + 1;
    if (kMax > n / 2) kMax = n / 2;
    if (kMax - kMin < 3 || kMax >= kMaxLag) return false;

    for (int k = kMin; k <= kMax; ++k) {
      float s = 0;
      for (int i = 0; i + k < n; ++i) s += tmp_[i] * tmp_[i + k];
      r_[k] = (s / (n - k)) / r0;  // unbiased, normalised to r(0) = 1
    }
    // Global max among local maxima, then the FIRST local max that reaches
    // 80 % of it (avoids locking onto 2x or 3x the beat period).
    float gmax = -1;
    for (int k = kMin + 1; k < kMax; ++k)
      if (r_[k] > r_[k - 1] && r_[k] >= r_[k + 1] && r_[k] > gmax) gmax = r_[k];
    if (gmax <= 0.05f) return false;
    int best = -1;
    for (int k = kMin + 1; k < kMax; ++k)
      if (r_[k] > r_[k - 1] && r_[k] >= r_[k + 1] && r_[k] >= 0.8f * gmax) { best = k; break; }
    if (best < 0) return false;
    // Parabolic interpolation for sub-sample lag resolution.
    const float a = r_[best - 1], b = r_[best], c = r_[best + 1];
    const float den = a - 2 * b + c;
    const float delta = (fabsf(den) > 1e-9f) ? 0.5f * (a - c) / den : 0.0f;
    const float lag = best + delta;
    bpm = 60.0f * fs / lag;
    quality = fminf(fmaxf(b, 0.0f), 1.0f);
    return bpm >= minBpm && bpm <= maxBpm;
  }

 private:
  static constexpr int kMaxLag = 400;
  float buf_[N] = {0};
  float tmp_[N] = {0};
  float r_[kMaxLag] = {0};
  int count_ = 0, idx_ = 0;
};

// ---------------------------------------------------------------------------
// BeatQuality: per-beat template matching.
//   Every confirmed beat, the PPG around the peak (0.25 s before .. 0.35 s
//   after) is compared with a running template (normalised correlation).
//   - a clean pulse looks like the previous pulses            -> corr ~ 0.9
//   - motion residue / noise / a wrongly detected beat        -> corr < 0.6
//   The template only learns from good beats, so artifacts cannot corrupt it.
//   quality() is a smoothed 0..1 value, used as
//     * measurement noise for the time-domain HR in the Kalman tracker
//     * one input of the reported confidence (hr_conf)
// Call push() for every sample and onBeat() when PeakDetector reports a beat
// (the peak is the previous sample). The window is evaluated 0.35 s later.
// ---------------------------------------------------------------------------
class BeatQuality {
 public:
  static constexpr int kPre = 25, kPost = 35, kLen = kPre + kPost + 1;

  void begin(float fs) { fs_ = fs; fullReset(); }
  void fullReset() {  // new wearer / sensor moved: forget the template
    haveTmpl_ = false; haveCand_ = false; q_ = 0; n_ = 0; lastEval_ = -1000000;
    softReset();
  }
  void softReset() { pending_ = -1; filled_ = 0; head_ = 0; }  // keeps template and clock

  void push(float x) {
    buf_[head_] = x;
    head_ = (head_ + 1) % kRing;
    if (filled_ < kRing) ++filled_;
    ++n_;
    if (pending_ > 0 && --pending_ == 0) {
      pending_ = -1;
      evaluate();
    }
  }
  void onBeat() {
    if (filled_ >= kLen && pending_ < 0) pending_ = kPost - 1;
  }

  // 0..1; 0 when no beat has been scored in the last 4 s
  float quality() const { return (n_ - lastEval_) < (long)(4.0f * fs_) ? q_ : 0.0f; }
  float lastCorr() const { return corr_; }

 private:
  static constexpr int kRing = 128;

  void evaluate() {
    float w[kLen];
    // window ends at the newest sample; its peak sits kPre samples from the start
    const int start = (head_ - kLen + 2 * kRing) % kRing;
    float mean = 0;
    for (int i = 0; i < kLen; ++i) { w[i] = buf_[(start + i) % kRing]; mean += w[i]; }
    mean /= kLen;
    float nrm = 0;
    for (int i = 0; i < kLen; ++i) { w[i] -= mean; nrm += w[i] * w[i]; }
    if (nrm <= 1e-12f) return;
    nrm = 1.0f / sqrtf(nrm);
    for (int i = 0; i < kLen; ++i) w[i] *= nrm;

    float c;
    if (!haveTmpl_) {
      // Bootstrap: the template is the first pair of beats that resemble each other.
      if (!haveCand_) { memcpy(cand_, w, sizeof(w)); haveCand_ = true; return; }
      c = dot(w, cand_);
      if (c >= 0.8f) {
        for (int i = 0; i < kLen; ++i) tmpl_[i] = cand_[i] + w[i];
        normalise(tmpl_);
        haveTmpl_ = true;
      } else {
        memcpy(cand_, w, sizeof(w));
        return;
      }
    } else {
      c = dot(w, tmpl_);
      if (c >= 0.7f) {  // learn only from good beats
        for (int i = 0; i < kLen; ++i) tmpl_[i] = 0.85f * tmpl_[i] + 0.15f * w[i];
        normalise(tmpl_);
      }
    }
    corr_ = c;
    const float cq = fminf(fmaxf((c - 0.3f) / 0.6f, 0.0f), 1.0f);  // 0.3 -> 0, 0.9 -> 1
    q_ = (n_ - lastEval_ > (long)(4.0f * fs_)) ? cq : q_ + 0.35f * (cq - q_);
    lastEval_ = n_;
  }
  static float dot(const float* a, const float* b) {
    float s = 0;
    for (int i = 0; i < kLen; ++i) s += a[i] * b[i];
    return s;
  }
  static void normalise(float* a) {
    float s = 0;
    for (int i = 0; i < kLen; ++i) s += a[i] * a[i];
    if (s > 0) { s = 1.0f / sqrtf(s); for (int i = 0; i < kLen; ++i) a[i] *= s; }
  }

  float fs_ = 100, q_ = 0, corr_ = 0;
  float buf_[kRing] = {0}, tmpl_[kLen] = {0}, cand_[kLen] = {0};
  bool haveTmpl_ = false, haveCand_ = false;
  int head_ = 0, filled_ = 0, pending_ = -1;
  long n_ = 0, lastEval_ = -1000000;
};
