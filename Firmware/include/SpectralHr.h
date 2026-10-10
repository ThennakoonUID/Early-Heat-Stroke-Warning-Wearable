#pragma once
// ---------------------------------------------------------------------------
// Heart rate DURING MOTION (pure C++, PC-testable).
//
// Idea (TROIKA / JOSS / WFPV family, IEEE SP Cup 2015):
//   Periodic work (walking, hammering, shovelling) puts the motion artifact
//   at a few fixed frequencies. The accelerometer measures exactly those
//   frequencies, so:
//     1. every 2 s take the last 12 s of PPG and accelerometer (25 Hz)
//     2. FFT both (Hann window, zero-padded to 1024 points)
//     3. find the accelerometer peaks (step rate, arm rate) and their
//        harmonics, and suppress those bins in the PPG spectrum
//     4. among the remaining PPG peaks, pick the one closest to where the
//        heart rate is expected to be (from the tracker)
//   HrTracker (a 1-D Kalman filter) then smooths the estimates, rejects
//   outliers, and predicts through short gaps (heavy motion).
// ---------------------------------------------------------------------------
#include "Filters.h"

// ---------------------------------------------------------------------------
// In-place radix-2 complex FFT, fixed size N (power of 2), precomputed twiddles.
// ---------------------------------------------------------------------------
template <int N>
class Fft {
 public:
  Fft() {
    for (int i = 0; i < N / 2; ++i) {
      cos_[i] = cosf(2.0f * dsp::kPi * i / N);
      sin_[i] = -sinf(2.0f * dsp::kPi * i / N);
    }
  }
  void run(float* re, float* im) const {
    for (int i = 1, j = 0; i < N; ++i) {  // bit reversal
      int bit = N >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) {
        float t = re[i]; re[i] = re[j]; re[j] = t;
        t = im[i]; im[i] = im[j]; im[j] = t;
      }
    }
    for (int len = 2; len <= N; len <<= 1) {
      const int half = len >> 1, step = N / len;
      for (int i = 0; i < N; i += len)
        for (int k = 0; k < half; ++k) {
          const float wr = cos_[k * step], wi = sin_[k * step];
          const int a = i + k, b = a + half;
          const float tr = re[b] * wr - im[b] * wi;
          const float ti = re[b] * wi + im[b] * wr;
          re[b] = re[a] - tr; im[b] = im[a] - ti;
          re[a] += tr;        im[a] += ti;
        }
    }
  }

 private:
  float cos_[N / 2], sin_[N / 2];
};

struct SpectralResult {
  bool valid = false;    // window usable and a peak was found
  float bpm = 0;
  float conf = 0;        // 0..1: share of PPG in-band power in the chosen peak
  int motionPeaks = 0;   // accelerometer peaks removed
  float accMg = 0;       // accelerometer in-band RMS (mg)
  float motionAt = 0;    // motion template value at the chosen peak (0 = clear of motion)
  float motionAtPred = 0;  // motion template value at the tracker's predicted HR
  bool motion = false;   // accelerometer showed in-band motion in this window
  float cadenceBpm = 0;  // strongest accelerometer peak (step / stride / arm rate), 0 = none
};

class SpectralHrEstimator {
 public:
  static constexpr int kDec = 4;     // 100 Hz -> 25 Hz
  static constexpr int kWin = 300;   // 12 s (5 bpm true resolution; 8 s gave 7.5 bpm and more error in the sim)
  static constexpr int kStep = 50;   // new estimate every 2 s
  static constexpr int kN = 1024;    // zero-padded FFT -> 1.46 bpm per bin

  float minBpm = 40, maxBpm = 200;
  float maxBadFrac = 0.2f;     // window rejected if >20 % of it was heavy motion
  float accMinMg = 8.0f;       // below this the accelerometer peaks are ignored
  float accPeakRel = 0.15f;    // accel peaks >= 15 % of the largest one count
  int notchHalfBins = 3;       // motion peaks closer than this are merged
  float motionWeight = 20.0f;  // strength of motion-frequency suppression
  float harmonicLevel = 0.5f;  // template height at motion harmonics

  void begin(float fsIn) {
    fs_ = fsIn / kDec;
    df_ = fs_ / kN;
    kLo_ = (int)ceilf(minBpm / 60.0f / df_);
    kHi_ = (int)floorf(maxBpm / 60.0f / df_);
    for (int i = 0; i < kWin; ++i) hann_[i] = 0.5f - 0.5f * cosf(2 * dsp::kPi * i / (kWin - 1));
    reset();
  }
  void reset() { filled_ = 0; head_ = 0; sinceLast_ = 0; acc_ = 0; memset(sum_, 0, sizeof(sum_)); badAcc_ = false; }

  // Call at 100 Hz. pred = tracker's predicted bpm (0 if none), predOk = tracker locked.
  // Returns true when a new 8 s window has been analysed (every 2 s).
  bool push(float ppg, const float accMg[3], bool bad, float pred, bool predOk, SpectralResult& r) {
    sum_[0] += ppg;
    for (int i = 0; i < 3; ++i) sum_[i + 1] += accMg[i];
    badAcc_ |= bad;
    if (++acc_ < kDec) return false;
    for (int c = 0; c < 4; ++c) buf_[c][head_] = sum_[c] / kDec;
    bad_[head_] = badAcc_;
    acc_ = 0; memset(sum_, 0, sizeof(sum_)); badAcc_ = false;
    head_ = (head_ + 1) % kWin;
    if (filled_ < kWin) ++filled_;
    if (++sinceLast_ < kStep || filled_ < kWin) return false;
    sinceLast_ = 0;
    analyse(pred, predOk, r);
    return true;
  }

 private:
  // Copies channel c (oldest first), removes mean, applies Hann, FFT -> power in pw.
  void spectrum(int c, float* pw, int kMax) {
    float mean = 0;
    for (int i = 0; i < kWin; ++i) mean += buf_[c][i];
    mean /= kWin;
    for (int i = 0; i < kN; ++i) { re_[i] = 0; im_[i] = 0; }
    for (int i = 0; i < kWin; ++i) re_[i] = (buf_[c][(head_ + i) % kWin] - mean) * hann_[i];
    fft_.run(re_, im_);
    for (int k = 0; k <= kMax; ++k) pw[k] = re_[k] * re_[k] + im_[k] * im_[k];
  }

  void analyse(float pred, bool predOk, SpectralResult& r) {
    r = SpectralResult();
    int nBad = 0;
    for (int i = 0; i < kWin; ++i) nBad += bad_[i];
    if (nBad > maxBadFrac * kWin) return;

    // --- accelerometer: in-band RMS and summed 3-axis spectrum ---------------
    float e = 0;
    for (int c = 1; c < 4; ++c)
      for (int i = 0; i < kWin; ++i) e += buf_[c][i] * buf_[c][i];
    r.accMg = sqrtf(e / kWin);

    spectrum(0, P_, kHi_ + 1);
    float pmaxAll = 0;
    for (int k = kLo_; k <= kHi_; ++k) pmaxAll = fmaxf(pmaxAll, P_[k]);
    if (pmaxAll <= 0) return;

    motionActive_ = r.accMg > accMinMg;
    if (motionActive_) {
      memset(A_, 0, sizeof(A_));
      for (int c = 1; c < 4; ++c) {
        spectrum(c, tmp_, kHi_ + 1);
        for (int k = 0; k <= kHi_ + 1; ++k) A_[k] += tmp_[k];
      }
      // accelerometer peaks from 0.5 Hz (filter band edge) to the top of the band
      const int k0 = (int)(0.5f / df_);
      float amax = 0;
      for (int k = k0; k <= kHi_; ++k) amax = fmaxf(amax, A_[k]);
      int peaks[3], np = 0;
      for (int pass = 0; pass < 3; ++pass) {  // 3 largest local maxima
        int best = -1;
        for (int k = k0 + 1; k < kHi_; ++k) {
          if (!(A_[k] > A_[k - 1] && A_[k] >= A_[k + 1] && A_[k] >= accPeakRel * amax)) continue;
          bool used = false;
          for (int j = 0; j < np; ++j) used |= abs(peaks[j] - k) <= notchHalfBins;
          if (!used && (best < 0 || A_[k] > A_[best])) best = k;
        }
        if (best < 0) break;
        peaks[np++] = best;
      }
      r.motionPeaks = np;
      if (np > 0) r.cadenceBpm = peaks[0] * df_ * 60.0f;  // peaks[0] = largest
      // Motion template M(k) in 0..1. Each motion peak contributes its own
      // spectral shape (which has exactly the same window leakage as the
      // artifact in the PPG), scaled to height 1: every axis couples into the
      // light path differently, so a small accelerometer peak (e.g. sideways
      // sway) can still give a big optical artifact. Its harmonics (h = 2..5)
      // are added at harmonicLevel: non-linear coupling creates harmonics and
      // sum frequencies (e.g. step + stride) the accelerometer barely shows;
      // for walking all of them are multiples of the stride frequency.
      const float inv = (amax > 0) ? 1.0f / amax : 0.0f;
      for (int k = 0; k <= kHi_ + 1; ++k) M_[k] = A_[k] * inv;
      const int L = (int)(0.3f / df_);  // +/- 0.3 Hz (window main lobe + margin)
      for (int j = 0; j < np; ++j) {
        const float apk = A_[peaks[j]];
        for (int h = 1; h <= 5; ++h) {
          const int kc = peaks[j] * h;
          if (kc - L > kHi_) break;
          const float level = (h == 1) ? 1.0f : harmonicLevel;
          for (int d = -L; d <= L; ++d) {
            const int k = kc + d, src = peaks[j] + d;
            if (k < 0 || k > kHi_ + 1 || src < 0 || src > kHi_ + 1) continue;
            const float v = level * A_[src] / apk;
            if (v > M_[k]) M_[k] = v;
          }
        }
      }
      // Weight: ~0.05 on a motion peak, ~0.5 where motion is 5 % of its peak.
      // Near the tracker's predicted HR keep at least 30 %, so the heart rate
      // can pass through the step rate without being erased.
      const int predBin = (predOk && pred > 0) ? (int)lroundf(pred / 60.0f / df_) : -1000;
      for (int k = 0; k <= kHi_ + 1; ++k) {
        float w = 1.0f / (1.0f + motionWeight * M_[k]);
        if (abs(k - predBin) <= 2 && w < 0.3f) w = 0.3f;
        P_[k] *= w;
      }
    }

    // --- choose the heart-rate peak ------------------------------------------
    float total = 0, pmax = 0;
    for (int k = kLo_; k <= kHi_; ++k) { total += P_[k]; pmax = fmaxf(pmax, P_[k]); }
    if (total <= 0) return;
    const float sigma = 12.0f;  // bpm, width of the tracker prior
    int best = -1;
    float bestScore = 0;
    for (int k = kLo_ + 1; k < kHi_; ++k) {
      if (!(P_[k] > P_[k - 1] && P_[k] >= P_[k + 1] && P_[k] >= 0.1f * pmax)) continue;
      float prior = 1.0f;
      if (predOk && pred > 0) {
        const float d = (k * df_ * 60.0f - pred) / sigma;
        prior = expf(-0.5f * d * d) + 0.02f;
      }
      const float s = P_[k] * prior;
      if (s > bestScore) { bestScore = s; best = k; }
    }
    if (best < 0) return;
    if (!predOk) {
      // Acquisition: the PPG 2nd harmonic can be the biggest peak. If there is
      // a solid peak at half the frequency, the heart rate is that one.
      const int kh = best / 2;
      if (kh - 2 >= kLo_) {
        int kb = kh;
        for (int k = kh - 2; k <= kh + 2; ++k) if (P_[k] > P_[kb]) kb = k;
        if (P_[kb] >= 0.35f * P_[best] && P_[kb] > P_[kb - 1] && P_[kb] >= P_[kb + 1]) best = kb;
      }
    }
    // parabolic interpolation between bins
    const float a = P_[best - 1], b = P_[best], c = P_[best + 1];
    const float den = a - 2 * b + c;
    const float delta = (fabsf(den) > 1e-20f) ? 0.5f * (a - c) / den : 0.0f;
    r.bpm = (best + delta) * df_ * 60.0f;
    float pk = 0;
    for (int k = best - 2; k <= best + 2; ++k) pk += P_[k];
    r.conf = pk / total;
    r.motion = motionActive_;
    r.motionAt = motionActive_ ? M_[best] : 0.0f;
    if (motionActive_ && predOk && pred > 0) {
      const int pb = (int)lroundf(pred / 60.0f / df_);
      if (pb >= 0 && pb <= kHi_ + 1) r.motionAtPred = M_[pb];
    }
    r.valid = true;
  }

  float fs_ = 25, df_ = 25.0f / 1024;
  int kLo_ = 0, kHi_ = 0;
  float buf_[4][kWin] = {{0}};
  bool bad_[kWin] = {false};
  float hann_[kWin];
  float sum_[4] = {0};
  bool badAcc_ = false, motionActive_ = false;
  int acc_ = 0, filled_ = 0, head_ = 0, sinceLast_ = 0;
  float re_[kN], im_[kN];
  float P_[kN / 2 + 1], A_[kN / 2 + 1], M_[kN / 2 + 1], tmp_[kN / 2 + 1];
  Fft<kN> fft_;
};

// ---------------------------------------------------------------------------
// Kalman tracker for heart rate: state = [HR (bpm), trend (bpm/s)].
//   - The trend state lets it follow HR rising at work onset / falling during
//     recovery without lagging, and carry on through short gaps (heavy motion,
//     or HR crossing the step rate where the spectral peak is hidden).
//   - Each estimate describes the HR some seconds ago (the 8 s spectral window
//     is centred 4 s back), so the measurement model is z = HR - delay * trend.
//   - The trend slowly decays to 0 when there is no data (no runaway).
//   inputs : time-domain HR when still (accurate), spectral HR during motion
//            (accuracy from its confidence)
//   - gates out estimates far from the prediction (wrong peak picked)
//   - if 3 rejected estimates in a row agree with each other, it re-locks to
//     them (the tracker itself was wrong / HR really jumped)
//   - acquisition from spectra needs 2 consecutive agreeing windows
// ---------------------------------------------------------------------------
class HrTracker {
 public:
  float qHr = 0.5f;         // bpm^2/s      direct HR noise (beat-to-beat variability)
  float qTrend = 0.04f;     // (bpm/s)^2/s  how fast the trend may change
  float trendTau = 40.0f;   // s, trend decays to 0 without data
  float maxTrend = 1.5f;    // bpm/s
  float acqConf = 0.25f;    // spectral confidence needed to acquire
  float minConf = 0.15f;    // ... and to update while tracking (still)
  float minConfMotion = 0.2f;  // ... and during motion
  float relockConfMotion = 0.22f;  // re-lock during motion: 3 agreeing peaks >= this
  float lostSec = 60.0f;    // no accepted update for this long -> unlock
  float acqMaxMotion = 0.05f;  // never (re)lock onto a peak sitting on a motion frequency
  float acqConfMotion = 0.35f; // confidence needed to acquire during motion
  float coastMotion = 0.3f;    // predicted HR on a motion frequency -> coast
  float specDelay = 6.0f;   // s, spectral estimate lag (half the window = kWin / 25 Hz / 2)
  float maxGate = 30.0f;    // bpm, largest innovation ever accepted from spectra
  float stillDelay = 3.0f;  // s, time-domain estimate lag

  void reset() { locked_ = false; acqBpm_ = 0; acqN_ = 0; nRej_ = 0; since_ = 1e9f; }
  bool locked() const { return locked_; }
  float bpm() const { return x_[0]; }
  // Physiological floor (bpm, 0 = none). During work HR does not drop below
  // the resting HR, so lower estimates are motion residue.
  void setFloor(float f) { floor_ = f; }
  float trend() const { return x_[1]; }
  float sinceUpdate() const { return since_; }

  void predict(float dt) {
    since_ += dt;
    if (!locked_) return;
    const float a = expf(-dt / trendTau);
    // x = F x,  F = [[1, dt], [0, a]]
    x_[0] += dt * x_[1];
    x_[1] *= a;
    if (floor_ > 0 && x_[0] < floor_) { x_[0] = floor_; if (x_[1] < 0) x_[1] = 0; }
    // P = F P F' + Q
    const float p00 = P_[0][0] + dt * (P_[1][0] + P_[0][1]) + dt * dt * P_[1][1];
    const float p01 = a * (P_[0][1] + dt * P_[1][1]);
    const float p11 = a * a * P_[1][1];
    P_[0][0] = p00 + qHr * dt + qTrend * dt * dt * dt / 3.0f;
    P_[0][1] = P_[1][0] = p01 + qTrend * dt * dt / 2.0f;
    P_[1][1] = p11 + qTrend * dt;
    if (since_ > lostSec) reset();
  }

  // Accurate time-domain measurement (still). Locks immediately.
  // sigma: measurement std in bpm (2.5 = clean; larger when beat quality is low)
  void updateStill(float z, float sigma = 2.5f) {
    if (!locked_) { lock(z, 4.0f); return; }
    conf_ = 1.0f;
    kalman(z, sigma * sigma, stillDelay, true);
  }

  // Spectral measurement (2 s windows).
  // motion      : the window contained in-band body motion
  // motionAt    : motion template value at the measured peak
  // motionAtPred: motion template value at the predicted HR
  void updateSpectral(float z, float conf, bool motion, float motionAt, float motionAtPred) {
    motion_ = motion;
    clearOfMotion_ = motionAt < acqMaxMotion;
    if (!locked_) {
      // During motion, acquire only from a strong peak well clear of every
      // motion frequency, confirmed by 3 windows in a row (6 s).
      const float need = motion ? acqConfMotion : acqConf;
      const int nNeed = motion ? 3 : 2;
      if (floor_ > 0 && z < floor_) { acqN_ = 0; return; }
      if (conf < need || !clearOfMotion_ || (acqN_ > 0 && fabsf(z - acqBpm_) >= 10.0f)) {
        acqN_ = 0;
        if (conf < need || !clearOfMotion_) return;
      }
      acqBpm_ = (acqN_ == 0) ? z : 0.5f * (acqBpm_ + z);
      if (++acqN_ >= nNeed) lock(acqBpm_, 25.0f);
      return;
    }
    // The heart rate is expected to be right on a motion frequency (e.g. HR
    // crossing the step rate): the spectral peak there is distorted, so do not
    // trust it - coast on the HR + trend prediction instead.
    // Exception: a clear peak away from every motion frequency is still used
    // (that is how the HR is picked up again once it has passed the step rate).
    if (motionAtPred > coastMotion && !(clearOfMotion_ && conf >= relockConfMotion)) return;
    if (conf < (motion ? minConfMotion : minConf)) return;
    if (floor_ > 0 && z < floor_) return;
    conf_ = conf;
    const float s = 2.5f + 12.0f * (1.0f - fminf(conf / 0.5f, 1.0f));  // 2.5..14.5 bpm
    kalman(z, s * s, specDelay, false);
  }

 private:
  void lock(float z, float var) {
    locked_ = true;
    x_[0] = z; x_[1] = 0;
    P_[0][0] = var; P_[0][1] = P_[1][0] = 0; P_[1][1] = 0.25f;
    since_ = 0; nRej_ = 0; acqBpm_ = 0; acqN_ = 0;
  }

  void kalman(float z, float R, float delay, bool trusted) {
    // H = [1, -delay]
    const float h1 = -delay;
    const float pred = x_[0] + h1 * x_[1];
    const float innov = z - pred;
    const float PH0 = P_[0][0] + P_[0][1] * h1;   // (P H')[0]
    const float PH1 = P_[1][0] + P_[1][1] * h1;   // (P H')[1]
    const float S = PH0 + h1 * PH1 + R;
    // Outlier gate grows with confidence: a weak peak may only nudge the
    // estimate, a strong one may correct it more.
    const float gate = fminf(maxGate, fmaxf(10.0f, 6.0f + 40.0f * conf_));
    if (!trusted && fabsf(innov) > gate) {
      // Rejected. Re-lock to the rejected values if they keep agreeing:
      //  - with motion: 2 strong peaks in a row, clear of all motion frequencies
      //  - without motion: 3 in a row
      //  - with motion: clear of all motion frequencies, and either 2 strong
      //    peaks or 3 medium peaks in a row
      if (motion_ && (!clearOfMotion_ || conf_ < relockConfMotion)) { nRej_ = 0; return; }
      if (nRej_ > 0 && nRej_ < 3 && fabsf(z - rej_[nRej_ - 1]) < 8.0f) {
        rej_[nRej_] = z; rejConf_[nRej_++] = conf_;
      } else { rej_[0] = z; rejConf_[0] = conf_; nRej_ = 1; }
      bool relock = nRej_ >= 3;
      if (motion_ && nRej_ >= 2)
        relock = relock || (fminf(rejConf_[nRej_ - 1], rejConf_[nRej_ - 2]) >= acqConfMotion);
      if (relock) {
        float m = 0;
        for (int i = 0; i < nRej_; ++i) m += rej_[i];
        lock(m / nRej_, 25.0f);
      }
      (void)S;
      return;
    }
    nRej_ = 0;
    const float K0 = PH0 / S, K1 = PH1 / S;
    x_[0] += K0 * innov;
    x_[1] += K1 * innov;
    if (x_[1] > maxTrend) x_[1] = maxTrend;
    if (x_[1] < -maxTrend) x_[1] = -maxTrend;
    // P = (I - K H) P
    const float p00 = P_[0][0] - K0 * PH0, p01 = P_[0][1] - K0 * PH1;
    const float p11 = P_[1][1] - K1 * PH1;
    P_[0][0] = p00; P_[0][1] = P_[1][0] = p01; P_[1][1] = p11;
    since_ = 0;
  }

  bool locked_ = false;
  float x_[2] = {0, 0};
  float P_[2][2] = {{100, 0}, {0, 0.25f}};
  float since_ = 1e9f, acqBpm_ = 0;
  float rej_[3] = {0}, rejConf_[3] = {0};
  int nRej_ = 0;
  bool clearOfMotion_ = true, motion_ = false;
  float floor_ = 0, conf_ = 0;
  int acqN_ = 0;
};
