#pragma once
// NTC thermistor raw ADC reading -> temperature (beta equation, same logic as thermistor.cpp). Pure C++.
#include <math.h>

struct NtcParams {
  float rSeries = 100000.0f, r0 = 100000.0f, beta = 3950.0f, t0C = 25.0f;
};

// rawAdc = 12-bit analogRead() value (0..4095). Returns false when there is no usable reading.
inline bool ntcCelsiusFromRaw(int rawAdc, const NtcParams& p, float& tC) {
  if (rawAdc == 0) return false;  // same error condition as thermistor.cpp
  const float ntcResistance = p.rSeries * ((4095.0f / rawAdc) - 1.0f);
  float steinhart = ntcResistance / p.r0;
  steinhart = logf(steinhart);
  steinhart /= p.beta;
  steinhart += 1.0f / (p.t0C + 273.15f);
  steinhart = 1.0f / steinhart;
  steinhart -= 273.15f;  // Celsius
  if (!isfinite(steinhart) || steinhart < -20.0f || steinhart > 90.0f) return false;  // open / shorted sensor
  tC = steinhart;
  return true;
}
