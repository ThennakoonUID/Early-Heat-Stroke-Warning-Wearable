// ===========================================================================
//  HEAT-STROKE EARLY-WARNING MONITOR
//  Upper-back (C7) reflectance PPG + heart rate + 2 x NTC skin temperature
//  + SHT30 ambient T/RH + GSR (sweat level)  ->  Firebase + buzzer
//  ESP32 DevKit V1  +  MAX30102  +  MPU6050  +  SHT30
//
//  Wiring (all three I2C sensors on the same bus):
//     SDA -> GPIO21     SCL -> GPIO22     VIN -> 3V3     GND -> GND
//     MAX30102 0x57, MPU6050 0x68 (AD0 low), SHT30 0x44 (ADDR low)
//  Analog (see Config.h):
//     NTC1 GPIO34, NTC2 GPIO35, GSR: DAC GPIO25, SW1 GPIO16, SW2 GPIO17, ADC GPIO32
//  Buzzer: GPIO27 (active buzzer, HIGH = on, see kBuzzerPin in Config.h)
//
//  Uploaded to Firebase (/devices/DEV-001, every kFirebaseIntervalMs):
//     skin_temp       mean of the 2 NTCs; a thermistor that suddenly jumps (lost contact) is
//                     dropped; both unusable -> text kSkinErrorText
//     outside_temp    SHT30 temperature
//     feels_like_temp heat index from SHT30 temperature + humidity
//     heart_rate      bpm (null while the PPG has no reliable value)
//     sweat_level     1 (dry) .. 10 (very wet) from the GSR conductance
//  Alert (buzzer only, NOT sent to Firebase): skin > 39 C, HR > 120, HR < 50 or almost no sweat.
//
//  The PPG / heart-rate code is the original, unchanged. The GSR front end and
//  the NTC readings run in their own task on core 0 (GsrFrontEnd.h); the Firebase
//  upload runs in another task on core 0 (FirebaseUploader.h).
//
//  Serial monitor (115200): one status line per second. Commands:
//     c  : re-calibrate the "still" motion baseline (sit still for ~7 s)
//     l  : toggle RAW LOG mode (CSV for offline replay)
//     s  : print a status line (overflows, dropped lines, LED current...)
//     g  : print GSR / NTC / SHT30 diagnostics
// ===========================================================================
#include <Arduino.h>
#include <Wire.h>

#include "Max30102.h"
#include "Mpu6050.h"
#include "MotionDetector.h"
#include "PpgProcessor.h"

#include <stdarg.h>

#include "Config.h"
#include "FirebaseUploader.h"
#include "GsrFrontEnd.h"
#include "HeatMetrics.h"
#include "HeatRisk.h"  // heatIndexC() only
#include "Ntc.h"
#include "Sht3x.h"

// ----------------------------- user settings -------------------------------
static constexpr int kSdaPin = 21;
static constexpr int kSclPin = 22;
// 115200 works on every board. Budget: ppg + ir_raw ~4.5 KB/s, 1 Hz channels
// ~0.1 KB/s, raw log mode ~6.6 KB/s, link capacity ~11.5 KB/s.
static constexpr uint32_t kBaud = 115200;
static constexpr int kPpgDiv = 1;  // ppg    : 100 Hz
static constexpr int kRawDiv = 1;  // ir_raw : 100 Hz
static constexpr int kRedDiv = 0;  // red_raw: 0 = off, 4 = 25 Hz (~0.6 KB/s)
static constexpr uint8_t kIrPaStart = 0xFF;  // 51 mA: maximum IR drive for the back

// Optional motion references for the NLMS canceller (see PpgConfig). Both are
// OFF by default: enable one at a time and compare on real data (raw log +
// replay tool) against a chest strap before trusting it.
static constexpr bool kUseGyroRef = false;
static constexpr bool kUseRedRef = false;  // EXPERIMENTAL, needs the red LED on
static constexpr uint8_t kRedPa = kUseRedRef ? 0x30 : 0x10;  // 9.6 mA / 3.2 mA
// ---------------------------------------------------------------------------

Max30102 ppgSensor;
Mpu6050 imu;
ImuFrontEnd imuFe;
PpgProcessor proc;

// Latest 100 Hz IMU sample (filled by the 1 kHz IMU path)
static float gAcc[3] = {0, 0, 1}, gGyro[3] = {0, 0, 0}, gVib = 0;
static uint32_t ppgSampleCount = 0;

static bool gLogMode = false;
static uint32_t gDroppedPlots = 0, gGaps = 0, gAlcEvents = 0;
static uint32_t gLastPpgOvf = 0, gLastImuOvf = 0;

// ---- heat-stroke monitor additions ----
Sht3x sht;
GsrFrontEnd gsrFe;
FirebaseUploader fb;
SkinTemp skinFuse;
SweatTracker sweatTrk;
AlertLogic alertLogic;
static AlertState gAlert;
// Latest 1 Hz heart-rate results (copied from PpgOutput in servicePpg)
static bool gHrValid = false;
static float gHr = 0, gHrConf = 0, gActMg = 0;
static uint32_t gHrStamp = 0;

// Raw log: everything the processor needs to replay this session offline.
//   L,n,ir,red,ax_mg,ay_mg,az_mg,gx_0.1dps,gy,gz,vib_0.1mg,pa
// n = sample counter, so the replay can detect lines lost on the serial link.
static void logSample(uint32_t ir, uint32_t red) {
  char b[96];
  const int n = snprintf(
      b, sizeof(b), "L,%lu,%lu,%lu,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%u\n", (unsigned long)ppgSampleCount,
      (unsigned long)ir, (unsigned long)red, lroundf(gAcc[0] * 1000.0f), lroundf(gAcc[1] * 1000.0f),
      lroundf(gAcc[2] * 1000.0f), lroundf(gGyro[0] * 10.0f), lroundf(gGyro[1] * 10.0f),
      lroundf(gGyro[2] * 10.0f), lroundf(gVib * 10000.0f), (unsigned)proc.irPa());
  if (Serial.availableForWrite() >= n) Serial.write((const uint8_t*)b, n);
  else ++gDroppedPlots;  // the counter column makes the gap visible in the replay
}

// ------------------------------ sensor I/O ---------------------------------
static void serviceImu() {
  static int16_t raw[40][6];
  const int n = imu.readFifo(raw, 40);
  for (int i = 0; i < n; ++i) {
    float a[3], g[3];
    for (int k = 0; k < 3; ++k) {
      a[k] = raw[i][k] / Mpu6050::kAccLsbPerG;
      g[k] = raw[i][k + 3] / Mpu6050::kGyroLsbPerDps;
    }
    if (imuFe.push(a, g)) {  // new 100 Hz sample
      memcpy(gAcc, imuFe.acc(), sizeof(gAcc));
      memcpy(gGyro, imuFe.gyro(), sizeof(gGyro));
    }
    gVib = imuFe.vib();
  }
}

static void servicePpg() {
  static uint32_t red[32], ir[32];
  const int n = ppgSensor.readSamples(red, ir, 32);
  for (int i = 0; i < n; ++i) {
    if (gLogMode) logSample(ir[i], red[i]);
    const PpgOutput o = proc.process(ir[i], red[i], gAcc, gGyro, gVib);
    ++ppgSampleCount;
    if (o.secondTick) {  // cache for the heat-stroke monitor (read-only use of the results)
      gHrValid = o.hrValid;
      gHr = o.hr;
      gHrConf = o.hrConf;
      gActMg = o.motion.acc * 1000.0f;
      gHrStamp = millis();
    }
    if (o.ledRequest >= 0) {  // automatic IR current control
      ppgSensor.setIrPa((uint8_t)o.ledRequest);
      proc.notifyLedChanged((uint8_t)o.ledRequest);
    }
  }
}

// Lost samples or an overloaded photodiode corrupt the waveform / frequency
// axis: tell the processor so those seconds are excluded from every estimate.
static void checkDataIntegrity() {
  static uint32_t lastAlc = 0, lastMsg = 0;
  const uint32_t now = millis();
  bool gap = false;
  if (ppgSensor.overflows() != gLastPpgOvf || imu.overflows() != gLastImuOvf) {
    gLastPpgOvf = ppgSensor.overflows();
    gLastImuOvf = imu.overflows();
    gap = true;
  }
  if (now - lastAlc >= 250) {  // ALC flag: read rarely, it costs an I2C transaction
    lastAlc = now;
    if (ppgSensor.alcOverflow()) { ++gAlcEvents; gap = true; }
  }
  if (gap) {
    ++gGaps;
    proc.notifyGap();
    if (now - lastMsg > 5000) {  // rate-limited text (ignored by the replay tool)
      lastMsg = now;
      Serial.printf("# data gap: ppgOvf=%lu imuOvf=%lu alc=%lu\n", (unsigned long)ppgSensor.overflows(),
                    (unsigned long)imu.overflows(), (unsigned long)gAlcEvents);
    }
  }
}

static void printState(const MotionDetector& m) {
  Serial.printf("# Motion baseline: acc %.1f mg, gyro %.2f dps, vib %.1f mg\n",
                m.baseline(0) * 1000, m.baseline(1), m.baseline(2) * 1000);
  Serial.printf("# Motion threshold: acc %.1f mg, gyro %.2f dps, vib %.1f mg%s\n",
                m.threshold(0) * 1000, m.threshold(1), m.threshold(2) * 1000,
                m.calFallback() ? "  (FALLBACK: wearer never kept still, using floors)" : "");
}

static void printStatus() {
  Serial.printf("# status: ppgOvf=%lu imuOvf=%lu gaps=%lu alc=%lu droppedTx=%lu irPa=0x%02X log=%d\n",
                (unsigned long)ppgSensor.overflows(), (unsigned long)imu.overflows(),
                (unsigned long)gGaps, (unsigned long)gAlcEvents, (unsigned long)gDroppedPlots,
                proc.irPa(), (int)gLogMode);
}


// ======================= heat-stroke monitor additions =======================
// Text output that can never block the sampling loop.
static void say(const char* fmt, ...) {
  char b[400];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (n > (int)sizeof(b) - 1) n = sizeof(b) - 1;
  if (Serial.availableForWrite() >= n) Serial.write((const uint8_t*)b, n);
  else ++gDroppedPlots;
}

// NTC raw ADC readings -> two temperatures (ok[i] false = open / shorted / no data)
static void readNtcs(const GsrSnapshot& s, bool fresh, float t[2], bool ok[2]) {
  NtcParams np;
  np.rSeries = kNtcRSeries; np.r0 = kNtcR0; np.beta = kNtcBeta;
  for (int i = 0; i < 2; ++i) {
    t[i] = 0;
    ok[i] = fresh && s.ntcSeq > 0 && ntcCelsiusFromRaw(s.ntcRaw[i], np, t[i]);
    if (ok[i]) t[i] += kNtcOffsetC[i];
  }
}

// Buzzer / LED: fast beeping while a heat-stroke alert is active.
static void updateAlertOutputs() {
  const uint32_t now = millis();
  const bool beep = gAlert.alert && (now % 400) < 200;
  if (kAlertLedPin >= 0) digitalWrite(kAlertLedPin, gAlert.alert ? HIGH : LOW);
  if (kBuzzerPin >= 0) digitalWrite(kBuzzerPin, beep ? HIGH : LOW);
}

static void alertReasons(const AlertState& a, char* out, size_t n) {
  out[0] = 0;
  size_t k = 0;
  auto add = [&](const char* txt) { k += snprintf(out + k, n - k, "%s%s", k ? ", " : "", txt); };
  if (a.skinHot) add("skin temp high");
  if (a.hrHigh) add("heart rate high");
  if (a.hrLow) add("heart rate low");
  if (a.sweatLow) add("almost no sweat");
}

// Runs once per second: gathers every sensor, computes the five Firebase values,
// evaluates the alert, hands the values to the uploader and prints one status line.
static void serviceHeatMonitor() {
  static uint32_t last = 0, lastPrint = 0, lastSeq = 0, seqChangedAt = 0;
  static bool lastAlert = false;
  const uint32_t now = millis();
  if (now - last < 1000) return;
  last = now;

  GsrSnapshot s;
  const bool have = gsrFe.get(s);
  if (have && s.seq != lastSeq) { lastSeq = s.seq; seqChangedAt = now; }
  const bool fresh = have && (now - seqChangedAt) < 3000;  // GSR task alive

  // ---- skin temperature: mean of the two NTCs, a thermistor that jumped is dropped ----
  float t[2];
  bool ntcOk[2];
  readNtcs(s, fresh, t, ntcOk);
  skinFuse.p.minC = kSkinMinC;   skinFuse.p.maxC = kSkinMaxC;
  skinFuse.p.maxDiffC = kNtcMaxDiffC;  skinFuse.p.jumpC = kNtcJumpC;
  skinFuse.p.staleMs = kNtcRefStaleMs;
  const bool envOk = sht.valid(now);
  const SkinResult skin = skinFuse.update(t, ntcOk, envOk ? sht.tempC() : NAN, now);
  const bool skinOk = skin.status != SkinStatus::Error;

  // ---- outside temperature + feels like (heat index from T and RH) ----
  const float outC = sht.tempC(), rh = sht.rh();
  const float feelsC = envOk ? heatIndexC(outC, rh) : 0.0f;

  // ---- heart rate (only a confident, recent value) ----
  const bool hrOk = gHrValid && gHr > 0 && gHrConf >= kHrMinConf && (now - gHrStamp) < 3000;

  // ---- sweat level 1..10 from the GSR conductance ----
  if (fresh && s.valid && !s.contact) sweatTrk.reset();           // electrodes off the skin
  sweatTrk.update(fresh && s.valid && s.contact, s.gUs, now);      // disturbed windows are skipped
  const bool sweatOk = sweatTrk.available(now, kGsrHoldMs);
  const int sweat = sweatOk ? sweatLevelFromUs(sweatTrk.gUs(), kSweatGMinUs, kSweatGMaxUs) : 0;

  // ---- heat-stroke alert -> buzzer (not sent to Firebase) ----
  alertLogic.p.skinC = kAlertSkinC;  alertLogic.p.hrHigh = kAlertHrHigh;  alertLogic.p.hrLow = kAlertHrLow;
  alertLogic.p.sweatMax = kAlertSweatMaxLevel;
  alertLogic.p.holdSec = kAlertHoldSec;  alertLogic.p.sweatHoldSec = kSweatLowHoldSec;
  AlertInputs ai;
  ai.skinValid = skinOk;  ai.skinC = skin.tempC;
  ai.hrValid = hrOk;      ai.hr = gHr;
  ai.sweatValid = sweatOk; ai.sweat = sweat;
  gAlert = alertLogic.update(ai);

  // ---- hand over to the uploader ----
  FbPayload fp;
  fp.skinOk = skinOk;     fp.skinC = skin.tempC;
  fp.outsideOk = envOk;   fp.outsideC = outC;  fp.feelsC = feelsC;
  fp.hrOk = hrOk;         fp.hr = gHr;
  fp.sweatOk = sweatOk;   fp.sweat = sweat;
  fb.submit(fp);

  // ---- serial output ----
  if (gAlert.alert != lastAlert) {
    lastAlert = gAlert.alert;
    if (gAlert.alert) {
      char why[96];
      alertReasons(gAlert, why, sizeof(why));
      say("# >>> HEAT STROKE ALERT <<<  %s\n", why);
    } else {
      say("# Alert cleared\n");
    }
  }

  if (now - lastPrint >= kPrintPeriodSec * 1000UL) {
    lastPrint = now;
    char skinTxt[96], envTxt[64], hrTxt[24], swTxt[48], fbTxt[64];
    if (skinOk) {
      const char* which = skin.status == SkinStatus::Ok ? "mean of T1+T2" : (skin.used[0] ? "T1 only" : "T2 only");
      snprintf(skinTxt, sizeof(skinTxt), "Skin %.1fC (%s)", skin.tempC, which);
    } else {
      snprintf(skinTxt, sizeof(skinTxt), "Skin ERROR: no skin contact (T1 %s, T2 %s)",
               ntcOk[0] ? String(t[0], 1).c_str() : "--", ntcOk[1] ? String(t[1], 1).c_str() : "--");
    }
    if (envOk) snprintf(envTxt, sizeof(envTxt), "Out %.1fC %.0f%%RH Feels %.1fC", outC, rh, feelsC);
    else snprintf(envTxt, sizeof(envTxt), "Out -- (SHT30)");
    if (hrOk) snprintf(hrTxt, sizeof(hrTxt), "HR %.0f", gHr);
    else snprintf(hrTxt, sizeof(hrTxt), "HR --");
    if (sweatOk) snprintf(swTxt, sizeof(swTxt), "Sweat %d/10 (%.2f uS)", sweat, sweatTrk.gUs());
    else snprintf(swTxt, sizeof(swTxt), "Sweat -- (no GSR contact)");
    if (!fb.wifiUp()) snprintf(fbTxt, sizeof(fbTxt), "FB: WiFi down");
    else if (fb.lastHttp() == 0) snprintf(fbTxt, sizeof(fbTxt), "FB: waiting");
    else if (fb.lastHttp() == 200 || fb.lastHttp() == 204)
      snprintf(fbTxt, sizeof(fbTxt), "FB ok #%lu (%lu ms, %d dBm)", (unsigned long)fb.okCount(),
               (unsigned long)fb.lastMs(), fb.rssi());
    else snprintf(fbTxt, sizeof(fbTxt), "FB ERROR %d (fails %lu)", fb.lastHttp(), (unsigned long)fb.failCount());
    say("%s | %s | %s | %s | %s%s\n", skinTxt, envTxt, hrTxt, swTxt, fbTxt, gAlert.alert ? " | ALERT" : "");
  }
}

static void printDiag() {
  GsrSnapshot s;
  const bool have = gsrFe.get(s);
  say("# GSR: %s N=%d (%d kS/s) DAC+ADC=%.1f us overruns=%lu ref=%s refAmp=%.0f refAge=%lu ms\n",
      have ? "running" : "no data yet", s.samplesPerCycle, s.samplesPerCycle, s.benchUs,
      (unsigned long)s.overruns, s.refOk ? "ok" : "BAD", s.refAmp, (unsigned long)s.refAgeMs);
  say("# GSR: skinAmp=%.0f counts snr=%.1f dB G=%.3f uS Z=%.1f kohm phase=%.1f deg sat=%d contact=%d valid=%d\n",
      s.ampCounts, s.snrDb, s.gUs, s.zKohm, s.phaseDeg, (int)s.saturated, (int)s.contact, (int)s.valid);
  say("# NTC: GPIO34 raw %d, GPIO35 raw %d | SHT30 %s T=%.2f RH=%.1f fails=%lu\n", s.ntcRaw[0], s.ntcRaw[1],
      sht.present() ? (sht.valid(millis()) ? "ok" : "no recent data") : "NOT FOUND", sht.tempC(), sht.rh(),
      (unsigned long)sht.failures());
}

// ------------------------------ setup / loop -------------------------------
void setup() {
  Serial.setTxBufferSize(4096);  // printing must never block the sampling
  Serial.begin(kBaud);
  delay(300);
  Serial.println("\n# C7 PPG monitor starting...");

  Wire.begin(kSdaPin, kSclPin);
  Wire.setClock(400000);  // 400 kHz fast mode: IMU FIFO at 1 kHz needs it

  if (!ppgSensor.begin(Wire, kIrPaStart, kRedPa)) {
    Serial.println("# ERROR: MAX30102 not found at 0x57. Check wiring.");
    while (true) delay(1000);
  }
  if (!imu.begin(Wire)) {
    Serial.println("# ERROR: MPU6050 not found at 0x68. Check wiring / AD0.");
    while (true) delay(1000);
  }
  if (imu.whoAmI() != 0x68)
    Serial.printf("# Note: MPU6050 WHO_AM_I = 0x%02X (clone chip), continuing.\n", imu.whoAmI());

  imuFe.begin(1000.0f, 10);
  proc.cfg.useGyroRef = kUseGyroRef;
  proc.cfg.useRedRef = kUseRedRef;
  proc.begin(kIrPaStart);
  Serial.println("# Calibrating motion baseline: keep the subject STILL for ~7 s.");

  // ---- heat-stroke monitor additions ----
  if (kAlertLedPin >= 0) { pinMode(kAlertLedPin, OUTPUT); digitalWrite(kAlertLedPin, LOW); }
  if (kBuzzerPin >= 0) { pinMode(kBuzzerPin, OUTPUT); digitalWrite(kBuzzerPin, LOW); }
  if (!sht.begin(Wire, kSht3xAddr))
    Serial.println("# WARNING: SHT30 not found at the configured address (0x44 / 0x45). Ambient T/RH disabled.");
  if (!gsrFe.begin())
    Serial.printf("# WARNING: DAC+ADC step takes %.1f us, too slow for 10 kS/s: GSR may be inaccurate.\n",
                  gsrFe.benchUs());
  else
    Serial.printf("# GSR front end started: %d samples/cycle (%d kS/s), DAC+ADC step %.1f us.\n",
                  gsrFe.samplesPerCycle(), gsrFe.samplesPerCycle(), gsrFe.benchUs());
  if (kBuzzerPin >= 0) {  // short self-test beep so a wrong buzzer wiring is noticed at once
    digitalWrite(kBuzzerPin, HIGH); delay(120); digitalWrite(kBuzzerPin, LOW);
  }
  fb.begin();  // WiFi + Firebase task (core 0)
  Serial.printf("# Connecting to WiFi \"%s\", uploading to %s/devices/%s every %lu ms.\n", kWifiSsid,
                kFirebaseHost, kDeviceId, (unsigned long)kFirebaseIntervalMs);
  Serial.println("# One status line per second; '# >>> HEAT STROKE ALERT <<<' when the buzzer starts.");
}

void loop() {
  static uint32_t lastPoll = 0;
  static bool wasCalibrating = true;

  // Poll both FIFOs every 5 ms. The MPU FIFO holds 85 ms and the MAX30102
  // FIFO 320 ms, so short hiccups (e.g. serial) never lose data.
  const uint32_t now = micros();
  if (now - lastPoll >= 5000) {
    lastPoll = now;
    serviceImu();
    servicePpg();
    checkDataIntegrity();
    sht.service(millis());  // non-blocking SHT30 (command now, result 16 ms later)
  }

  serviceHeatMonitor();  // 1 Hz: NTC + GSR + SHT30 + HR -> Firebase values + alert
  updateAlertOutputs();

  const bool cal = proc.motion().features().calibrating;
  if (wasCalibrating && !cal) {
    Serial.println("# Calibration done.");
    printState(proc.motion());
  }
  wasCalibrating = cal;

  if (Serial.available()) {
    const char c = Serial.read();
    if (c == 'c' || c == 'C') {
      proc.recalibrateMotion();
      Serial.println("# Re-calibrating: keep still for ~7 s.");
    } else if (c == 'l' || c == 'L') {
      gLogMode = !gLogMode;
      if (gLogMode)
        Serial.println("# LOG fs=100 cols=L,n,ir,red,ax_mg,ay_mg,az_mg,gx_0.1dps,gy,gz,vib_0.1mg,pa");
      else
        Serial.println("# LOG off");
    } else if (c == 's' || c == 'S') {
      printStatus();
    } else if (c == 'g' || c == 'G') {
      printDiag();
    }
  }
}
