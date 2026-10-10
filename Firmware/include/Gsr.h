#pragma once
// ---------------------------------------------------------------------------
// GSR (skin conductance) maths. Pure C++, no hardware access, PC-testable.
//
// Measurement principle (ratiometric, see the schematic):
//   - A 1 kHz sine drives Rs (100k). The current goes either through the
//     10k 0.1 % reference resistor (SW1) or through 1 uF + the skin (SW2)
//     into the transimpedance amplifier. The ADC samples the TIA output.
//   - A software lock-in over WHOLE excitation cycles gives the complex
//     amplitude M of the 1 kHz component for each path. The DAC -> ADC
//     delay, the TIA's 470 pF phase lag and the ADC/DAC gains are identical
//     for both paths, so they cancel in the ratio:
//
//        I_skin / I_ref = M_skin / M_ref = (Rs + Ron + Rref) / (Rs + Ron + Zs)
//
//        Zs    = (Rs + Ron + Rref) * (M_ref / M_skin) - (Rs + Ron)
//        Zskin = Zs - 1 / (j*w*Ccouple)               (remove the 1 uF series cap)
//        Y     = 1 / Zskin                             (|Y| in microsiemens)
// ---------------------------------------------------------------------------
#include <cmath>
#include <complex>
#include <cstdint>

namespace gsr {

constexpr int kMaxN = 32;  // max samples per excitation cycle

class LockIn {
 public:
  // n = samples per excitation cycle (sample rate = n * excitation frequency)
  void begin(int n) {
    n_ = n > kMaxN ? kMaxN : n;
    for (int i = 0; i < n_; ++i) {
      const double th = 2.0 * M_PI * i / n_;
      cosT_[i] = (int32_t)lround(4096.0 * cos(th));
      sinT_[i] = (int32_t)lround(4096.0 * sin(th));
    }
    reset();
  }
  void reset() {
    idx_ = 0;
    cycC_ = cycS_ = cycX_ = 0;
    cycXX_ = 0;
    sumC_ = sumS_ = sumX_ = sumXX_ = 0;
    cycles_ = 0;
    vmin_ = 65535;
    vmax_ = 0;
  }
  // Feed 12-bit ADC samples in order. The first sample after reset() must be
  // the one taken together with DAC table entry 0.
  inline void add(int x) {
    cycC_ += (int32_t)x * cosT_[idx_];
    cycS_ += (int32_t)x * sinT_[idx_];
    cycX_ += x;
    cycXX_ += (int64_t)x * x;
    if (x < vmin_) vmin_ = x;
    if (x > vmax_) vmax_ = x;
    if (++idx_ >= n_) {  // cycle complete: fold into the 64-bit totals
      sumC_ += cycC_;
      sumS_ += cycS_;
      sumX_ += cycX_;
      sumXX_ += cycXX_;
      cycC_ = cycS_ = cycX_ = 0;
      cycXX_ = 0;
      idx_ = 0;
      ++cycles_;
    }
  }
  int cycles() const { return cycles_; }
  int minSample() const { return vmin_; }
  int maxSample() const { return vmax_; }

  // Complex amplitude M = a - j*b of  x[n] ~ a*cos(th) + b*sin(th)  (ADC counts)
  std::complex<double> phasor() const {
    if (cycles_ == 0) return {0, 0};
    const double k = (double)cycles_ * n_ * 4096.0;
    return {2.0 * (double)sumC_ / k, -2.0 * (double)sumS_ / k};
  }
  double mean() const { return cycles_ ? (double)sumX_ / ((double)cycles_ * n_) : 0.0; }
  // RMS of everything that is neither DC nor the fundamental (noise + harmonics)
  double residualRms() const {
    if (cycles_ == 0) return 0;
    const double kk = (double)cycles_ * n_;
    const double m = (double)sumX_ / kk;
    const std::complex<double> p = phasor();
    const double var = (double)sumXX_ / kk - m * m;
    const double r = var - 0.5 * std::norm(p);
    return r > 0 ? sqrt(r) : 0.0;
  }

 private:
  int n_ = 20, idx_ = 0, cycles_ = 0, vmin_ = 65535, vmax_ = 0;
  int32_t cosT_[kMaxN] = {0}, sinT_[kMaxN] = {0};
  int32_t cycC_ = 0, cycS_ = 0, cycX_ = 0;
  int64_t cycXX_ = 0;
  int64_t sumC_ = 0, sumS_ = 0, sumX_ = 0, sumXX_ = 0;
};

struct SkinParams {
  double rs = 100e3;        // series resistor after the coupling network (0.1 %)
  double rref = 10e3;       // reference resistor behind SW1 (0.1 %)
  double ron = 100.0;       // 74HC4066 on-resistance at 3.3 V (about the same in both paths)
  double ccouple = 1e-6;    // DC-blocking capacitor in series with the electrodes
  double freq = 1000.0;     // excitation frequency (Hz)
};

struct SkinResult {
  bool ok = false;
  double zRe = 0, zIm = 0;  // skin impedance (ohm), electrode coupling cap removed
  double zMag = 0;          // ohm
  double phaseDeg = 0;      // angle of Z (negative = capacitive, normal for skin)
  double gUs = 0;           // |Y| in microsiemens
};

inline SkinResult skinFromPhasors(std::complex<double> mRef, std::complex<double> mSkin,
                                  const SkinParams& p) {
  SkinResult r;
  if (std::abs(mRef) < 1e-6) return r;
  const double w = 2.0 * M_PI * p.freq;
  if (std::abs(mSkin) < 1e-9) {  // no current at all: electrodes open
    r.zMag = 1e12; r.gUs = 0; r.ok = true; return r;
  }
  const double rsTot = p.rs + p.ron;
  std::complex<double> zs = (rsTot + p.rref) * (mRef / mSkin) - rsTot;
  std::complex<double> zSkin = zs + std::complex<double>(0.0, 1.0 / (w * p.ccouple));
  r.zRe = zSkin.real();
  r.zIm = zSkin.imag();
  r.zMag = std::abs(zSkin);
  if (r.zMag < 100.0) r.zMag = 100.0;  // noise / calibration error cannot give a short
  r.phaseDeg = std::arg(zSkin) * 180.0 / M_PI;
  r.gUs = 1e6 / r.zMag;
  r.ok = true;
  return r;
}

}  // namespace gsr
