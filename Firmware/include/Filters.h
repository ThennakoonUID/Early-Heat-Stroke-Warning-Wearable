#pragma once
// ---------------------------------------------------------------------------
// DSP building blocks. Pure C++ (no Arduino dependency) so the whole signal
// chain can be compiled and tested on a PC with synthetic data.
// ---------------------------------------------------------------------------
#include <math.h>
#include <stdint.h>
#include <string.h>

namespace dsp {

constexpr float kPi = 3.14159265358979f;

// ---------------------------------------------------------------------------
// 2nd-order IIR section (RBJ cookbook design, Direct Form II Transposed).
// ---------------------------------------------------------------------------
template <typename T>
class BiquadT {
 public:
  void lowpass(float fs, float fc, float q) { design(fs, fc, q, false); }
  void highpass(float fs, float fc, float q) { design(fs, fc, q, true); }

  inline T process(T x) {
    const T y = b0_ * x + z1_;
    z1_ = b1_ * x - a1_ * y + z2_;
    z2_ = b2_ * x - a2_ * y;
    return y;
  }
  void reset() { z1_ = z2_ = 0; }

 private:
  void design(float fsF, float fcF, float qF, bool hp) {
    // Coefficients are always computed in double. For the low-frequency
    // sections (0.3 / 0.5 Hz at 100 Hz) the poles sit extremely close to
    // z = 1, where float32 coefficients and state lose too many bits.
    const double fs = fsF, fc = fcF, q = qF;
    const double w0 = 2.0 * 3.14159265358979323846 * fc / fs;
    const double c = cos(w0), s = sin(w0);
    const double alpha = s / (2.0 * q);
    const double a0 = 1.0 + alpha;
    double b0, b1;
    if (hp) {
      b0 = (1.0 + c) * 0.5;
      b1 = -(1.0 + c);
    } else {
      b0 = (1.0 - c) * 0.5;
      b1 = (1.0 - c);
    }
    b0_ = (T)(b0 / a0);
    b1_ = (T)(b1 / a0);
    b2_ = b0_;
    a1_ = (T)(-2.0 * c / a0);
    a2_ = (T)((1.0 - alpha) / a0);
    reset();
  }
  T b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0, z1_ = 0, z2_ = 0;
};

using Biquad = BiquadT<float>;    // fast path (1 kHz IMU filters)
using BiquadD = BiquadT<double>;  // low-frequency sections (100 Hz and below)

// ---------------------------------------------------------------------------
// 4th-order Butterworth band-pass = 4th-order HP cascade + 4th-order LP cascade.
// Q values of the two sections of a 4th-order Butterworth: 0.5412 and 1.3066.
// ---------------------------------------------------------------------------
class Butter4BandPass {
 public:
  void design(float fs, float fLow, float fHigh) {
    const float q1 = 0.54119610f, q2 = 1.30656296f;
    hp1_.highpass(fs, fLow, q1);
    hp2_.highpass(fs, fLow, q2);
    lp1_.lowpass(fs, fHigh, q1);
    lp2_.lowpass(fs, fHigh, q2);
  }
  inline float process(float x) {
    return (float)lp2_.process(lp1_.process(hp2_.process(hp1_.process((double)x))));
  }
  void reset() { hp1_.reset(); hp2_.reset(); lp1_.reset(); lp2_.reset(); }

 private:
  BiquadD hp1_, hp2_, lp1_, lp2_;
};

// ---------------------------------------------------------------------------
// Exponential moving average (1st-order low-pass) with a time constant.
// After reset() it initialises to the next input, so there is no start-up step.
// ---------------------------------------------------------------------------
class Ema {
 public:
  void setTau(float fs, float tauSec) { alpha_ = 1.0f - expf(-1.0f / (fs * tauSec)); }
  inline float process(float x) {
    if (!init_) { y_ = x; init_ = true; }
    else y_ += alpha_ * (x - y_);
    return y_;
  }
  float value() const { return y_; }
  void reset() { init_ = false; }

 private:
  float alpha_ = 0.1f, y_ = 0.0f;
  bool init_ = false;
};

// ---------------------------------------------------------------------------
// Hampel filter: robust spike remover. If the newest sample is further than
// k * sigma from the window median (sigma = 1.4826 * MAD), it is replaced by
// the median. Unlike a plain median filter it leaves normal samples untouched.
// ---------------------------------------------------------------------------
template <int N>
class Hampel {
  static_assert(N % 2 == 1, "Hampel window must be odd");

 public:
  explicit Hampel(float k = 3.5f) : k_(k) {}

  float process(float x) {
    const int slot = idx_;
    buf_[slot] = x;
    idx_ = (idx_ + 1) % N;
    if (count_ < N) { ++count_; return x; }

    float tmp[N];
    memcpy(tmp, buf_, sizeof(tmp));
    const float med = median(tmp);
    for (int i = 0; i < N; ++i) tmp[i] = fabsf(buf_[i] - med);
    const float sigma = 1.4826f * median(tmp);
    if (sigma > 0.0f && fabsf(x - med) > k_ * sigma) {
      buf_[slot] = med;  // keep the spike out of future windows too
      ++outliers_;
      return med;
    }
    return x;
  }
  void reset() { idx_ = 0; count_ = 0; }
  uint32_t outliers() const { return outliers_; }

 private:
  static float median(float* a) {  // insertion sort, N is tiny
    for (int i = 1; i < N; ++i) {
      const float v = a[i];
      int j = i - 1;
      while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; --j; }
      a[j + 1] = v;
    }
    return a[N / 2];
  }
  float buf_[N] = {0};
  int idx_ = 0, count_ = 0;
  float k_;
  uint32_t outliers_ = 0;
};

// ---------------------------------------------------------------------------
// Moving RMS over the last N samples (running sum, re-summed every N samples
// to stop floating-point drift).
// ---------------------------------------------------------------------------
template <int N>
class MovingRms {
 public:
  float process(float x) {
    const float sq = x * x;
    sum_ += sq - buf_[idx_];
    buf_[idx_] = sq;
    if (++idx_ == N) {
      idx_ = 0;
      sum_ = 0.0f;
      for (int i = 0; i < N; ++i) sum_ += buf_[i];
    }
    if (count_ < N) ++count_;
    value_ = sqrtf(fmaxf(sum_, 0.0f) / (float)count_);
    return value_;
  }
  float value() const { return value_; }
  void reset() { memset(buf_, 0, sizeof(buf_)); sum_ = 0; idx_ = 0; count_ = 0; value_ = 0; }

 private:
  float buf_[N] = {0};
  float sum_ = 0.0f, value_ = 0.0f;
  int idx_ = 0, count_ = 0;
};

// ---------------------------------------------------------------------------
// Multi-channel NLMS adaptive noise canceller.
//   d   = primary input (PPG = pulse + motion artifact)
//   ref = reference inputs (accelerometer axes = motion only)
// The filter learns how motion leaks into the PPG and subtracts that estimate:
//   e = d - sum_c sum_i w[c][i] * x[c][i]
// While adapting, a tiny leak keeps the weights bounded. When NOT adapting
// (body still) the weights decay faster towards zero, so the canceller cannot
// remove the heartbeat itself via the ballistocardiogram the IMU picks up.
// ---------------------------------------------------------------------------
template <int CH, int L>
class NlmsCanceller {
 public:
  void configure(float mu, float leak, float idleLeak, float eps) {
    mu_ = mu; leak_ = leak; idleLeak_ = idleLeak; eps_ = eps;
  }

  float process(float d, const float* ref, bool adapt) {
    for (int c = 0; c < CH; ++c) {
      memmove(&x_[c][1], &x_[c][0], (L - 1) * sizeof(float));
      x_[c][0] = ref[c];
    }
    float y = 0.0f, p = 0.0f;
    for (int c = 0; c < CH; ++c)
      for (int i = 0; i < L; ++i) {
        y += w_[c][i] * x_[c][i];
        p += x_[c][i] * x_[c][i];
      }
    const float e = d - y;
    if (adapt) {
      const float g = mu_ * e / (eps_ + p);
      for (int c = 0; c < CH; ++c)
        for (int i = 0; i < L; ++i) w_[c][i] = leak_ * w_[c][i] + g * x_[c][i];
    } else {
      for (int c = 0; c < CH; ++c)
        for (int i = 0; i < L; ++i) w_[c][i] *= idleLeak_;
    }
    lastEstimate_ = y;
    return e;
  }
  float lastEstimate() const { return lastEstimate_; }
  void reset() { memset(w_, 0, sizeof(w_)); memset(x_, 0, sizeof(x_)); }

 private:
  float w_[CH][L] = {{0}};
  float x_[CH][L] = {{0}};
  float mu_ = 0.02f, leak_ = 0.9999f, idleLeak_ = 0.995f, eps_ = 1.0f;
  float lastEstimate_ = 0.0f;
};

}  // namespace dsp
