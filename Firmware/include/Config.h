#pragma once
// ===========================================================================
//  All user-tunable settings of the heat-stroke monitor in one place.
//  (The PPG / heart-rate tuning knobs stay where they were: PpgProcessor.h etc.)
// ===========================================================================
#include <stdint.h>

// ------------------------------- pins --------------------------------------
// I2C bus (shared): MAX30102 0x57, MPU6050 0x68, SHT30 0x44 (ADDR pin low) / 0x45 (high)
static constexpr uint8_t kSht3xAddr = 0x44;

// NTC thermistors (ADC1, input-only pins)
static constexpr int kNtc1Pin = 34;  // ADC1_CH6
static constexpr int kNtc2Pin = 35;  // ADC1_CH7

// GSR analog front end (see schematic)
static constexpr int kGsrDacPin = 25;   // DAC1 -> Sallen-Key -> coupling -> Rs -> EXC
static constexpr int kGsrSw1Pin = 16;   // 74HC4066 SW1: 10k reference resistor path
static constexpr int kGsrSw2Pin = 17;   // 74HC4066 SW2: 1 uF + electrodes path
static constexpr int kGsrAdcPin = 32;   // ADC1_CH4, after the U2B buffer / 150R / 470pF

// Alert outputs
static constexpr int kAlertLedPin = 2;      // DevKit V1 on-board LED. -1 = off
static constexpr int kBuzzerPin = 27;       // active buzzer (HIGH = on). -1 = none. GPIO27 is free
                                            // (a buzzer drawing > 20 mA needs a transistor)

// ------------------------------- GSR ----------------------------------------
// Passive values from the schematic
static constexpr double kGsrRs = 100e3;      // Rs, 0.1 %
static constexpr double kGsrRref = 10e3;     // reference resistor, 0.1 %
static constexpr double kGsrRon = 100.0;     // 74HC4066 Ron at 3.3 V (about equal in both paths)
static constexpr double kGsrCcouple = 1e-6;  // series cap in the electrode path
static constexpr double kGsrFreq = 1000.0;   // excitation frequency (Hz); do not change: the
                                             // sample period is derived from it (see GsrFrontEnd.h)
static constexpr int kGsrDacAmp = 100;        // sine amplitude in DAC counts (8 bit). 38 -> ~0.98 Vpp
static constexpr int kGsrSettleCycles = 30;  // discarded cycles after every start (AC-coupling settling)
static constexpr int kGsrMeasCycles = 200;   // measured cycles per GSR value (0.2 s -> ~4 values/s)
static constexpr int kGsrRefCycles = 100;    // measured cycles for the reference resistor
static constexpr uint32_t kGsrRefIntervalMs = 20000;  // re-measure the 10k reference this often
static constexpr double kGsrContactMinUs = 0.10;      // below this (>10 Mohm) = electrodes not on skin
static constexpr double kGsrRefAmpMin = 200.0, kGsrRefAmpMax = 1200.0;  // plausible reference amplitude (ADC counts)

// ------------------------------- NTC ----------------------------------------
// Read like thermistor.cpp: 12-bit analogRead(), R = Rseries * (4095/raw - 1), beta equation.
// That formula assumes:  3V3 --- NTC --- node(GPIO34/35) --- R_series --- GND
static constexpr float kNtcRSeries = 100000.0f;  // fixed resistor (ohm)
static constexpr float kNtcR0 = 100000.0f;       // NTC resistance at 25 C
static constexpr float kNtcBeta = 3950.0f;       // check your datasheet (3435 / 3950 are common)
// Per-sensor calibration offsets: compare with a clinical thermometer (same water bath / skin spot)
static constexpr float kNtcOffsetC[2] = {0.0f, 0.0f};
// Added to the fused NTC reading to get the "body temperature" the thresholds below refer to.
// Skin-contact NTCs read below core temperature: calibrate this on your sensor placement.
static constexpr float kBodyTempOffsetC = 0.0f;
static constexpr float kNtcMaxDiffC = 2.0f;      // two NTCs disagree by more than this -> one lost contact:
                                                 // use only the one that did NOT jump (see SkinTemp in HeatMetrics.h)
// Skin-temperature plausibility. A reading outside this window cannot be skin (= no contact).
static constexpr float kSkinMinC = 26.0f;
static constexpr float kSkinMaxC = 39.0f;
// A sensor that moved more than this from its own recent value (EMA) has "suddenly jumped".
// If BOTH jump at once, both are assumed to have lost contact -> error.
static constexpr float kNtcJumpC = 3.0f;
static constexpr uint32_t kNtcRefStaleMs = 60000;  // forget the recent values after this long without a good reading
static constexpr const char* kSkinErrorText = "ERR: no skin contact";  // sent to Firebase as skin_temp when both fail

// ------------------------------- sweat level (1..10 from GSR) ---------------
// Log mapping of the smoothed skin conductance: <= kSweatGMinUs -> 1, >= kSweatGMaxUs -> 10.
// CALIBRATE: read "GSR x.xx uS" on the serial monitor with dry skin and with clearly wet skin.
static constexpr float kSweatGMinUs = 2.0f;
static constexpr float kSweatGMaxUs = 150.0f;
static constexpr uint32_t kGsrHoldMs = 10000;   // keep the last good GSR value this long if windows are disturbed (WiFi)

// ------------------------------- heat-stroke alert (buzzer) -----------------
static constexpr float kAlertSkinC = 40.0f;        // skin temperature >  this
static constexpr float kAlertHrHigh = 120.0f;      // heart rate       >  this
static constexpr float kAlertHrLow = 50.0f;        // heart rate       <  this
static constexpr int kAlertSweatMaxLevel = 1;      // sweat level      <= this = "almost no sweat"
static constexpr float kHrMinConf = 40.0f;         // ignore heart rate below this confidence (0..100)
static constexpr uint32_t kAlertHoldSec = 3;       // skin / HR condition must last this long
static constexpr uint32_t kSweatLowHoldSec = 20;   // low sweat must last this long (GSR is slow and noisy)

// ------------------------------- WiFi + Firebase ----------------------------
static constexpr const char* kWifiSsid = "Isula";
static constexpr const char* kWifiPass = "00000000";
static constexpr const char* kFirebaseHost = "heatstrockapp-default-rtdb.asia-southeast1.firebasedatabase.app";
static constexpr const char* kDeviceId = "DEV-001";          // data goes to /devices/<kDeviceId>
// Leave empty if the database rules allow public writes (test mode). Otherwise put the
// database secret or an ID token here (sent as ?auth=...).
static constexpr const char* kFirebaseAuth = "";
static constexpr uint32_t kFirebaseIntervalMs = 2000;        // upload period

// ------------------------------- person -------------------------------------
static constexpr float kAgeYears = 22.0f;        // HRmax = 208 - 0.7 * age

// ------------------------------- serial -------------------------------------
static constexpr uint32_t kPrintPeriodSec = 1;   // human-readable status line period
