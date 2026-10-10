#pragma once
// ---------------------------------------------------------------------------
// Firebase Realtime Database uploader (plain REST, no extra library).
//
// Runs in its own FreeRTOS task on core 0, so the TLS handshake and the network
// waits can never stall the PPG / IMU sampling in loop() (core 1). The task
// priority is above the GSR task (10) on purpose: at a lower priority it would
// only get CPU in the 1 ms gaps between GSR windows and the upload would crawl.
// A GSR window that overlaps an upload is flagged late by GsrFrontEnd and simply
// skipped (SweatTracker rides through it).
//
// Data goes to   https://<host>/devices/<id>.json   with PATCH (one request,
// only these five keys are touched):
//   skin_temp, outside_temp, feels_like_temp, heart_rate, sweat_level
// A value that is not available is sent as null (the key is removed from the
// database, so the app never shows an old number as if it were current).
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "Config.h"

struct FbPayload {
  bool skinOk = false;        // false -> skin_temp = kSkinErrorText
  float skinC = 0;
  bool outsideOk = false;     // SHT30
  float outsideC = 0, feelsC = 0;
  bool hrOk = false;
  float hr = 0;
  bool sweatOk = false;
  int sweat = 0;
  uint32_t seq = 0;           // increments with every submit()
};

class FirebaseUploader {
 public:
  void begin() {
    xTaskCreatePinnedToCore(&FirebaseUploader::trampoline, "fb", 10240, this, 12, nullptr, 0);
  }

  // Latest values to send (thread-safe, never blocks).
  void submit(FbPayload p) {
    portENTER_CRITICAL(&mux_);
    p.seq = ++seq_;
    pending_ = p;
    portEXIT_CRITICAL(&mux_);
  }

  // For the serial status line
  bool wifiUp() const { return wifiUp_; }
  int lastHttp() const { return lastHttp_; }       // 204 / 200 = ok, <0 = transport error, 0 = none yet
  uint32_t lastMs() const { return lastDurMs_; }   // duration of the last request
  uint32_t okCount() const { return okCount_; }
  uint32_t failCount() const { return failCount_; }
  int rssi() const { return wifiUp_ ? WiFi.RSSI() : 0; }

 private:
  static void trampoline(void* self) { static_cast<FirebaseUploader*>(self)->run(); }

  static void addNumber(String& s, const char* key, bool ok, float v, int decimals, bool first = false) {
    if (!first) s += ',';
    s += '"'; s += key; s += "\":";
    if (ok) s += String(v, decimals);
    else s += "null";
  }

  static String buildJson(const FbPayload& p) {
    String j;
    j.reserve(180);
    j += "{\"skin_temp\":";
    if (p.skinOk) j += String(p.skinC, 1);
    else { j += '"'; j += kSkinErrorText; j += '"'; }
    addNumber(j, "outside_temp", p.outsideOk, p.outsideC, 1);
    addNumber(j, "feels_like_temp", p.outsideOk, p.feelsC, 1);
    addNumber(j, "heart_rate", p.hrOk, p.hr, 0);
    addNumber(j, "sweat_level", p.sweatOk, (float)p.sweat, 0);
    j += '}';
    return j;
  }

  void run() {
    // Start WiFi from this task so its interrupts and tasks sit on core 0, away from the PPG loop.
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);  // modem sleep adds latency spikes
    WiFi.begin(kWifiSsid, kWifiPass);

    WiFiClientSecure client;
    client.setInsecure();  // no certificate pinning (encrypted, but the server is not verified)
    client.setTimeout(6);  // seconds
    HTTPClient http;
    http.setReuse(true);   // keep the TLS connection open between uploads
    http.setTimeout(6000);

    String path = String("/devices/") + kDeviceId + ".json?print=silent";
    if (kFirebaseAuth[0]) path += String("&auth=") + kFirebaseAuth;

    uint32_t lastSentSeq = 0, lastSendMs = 0, lastReconnectMs = millis();

    for (;;) {
      vTaskDelay(pdMS_TO_TICKS(100));
      const bool up = WiFi.status() == WL_CONNECTED;
      wifiUp_ = up;
      if (!up) {
        if (millis() - lastReconnectMs > 10000) {  // auto-reconnect sometimes gives up
          lastReconnectMs = millis();
          WiFi.disconnect();
          WiFi.begin(kWifiSsid, kWifiPass);
        }
        continue;
      }
      if (millis() - lastSendMs < kFirebaseIntervalMs) continue;

      FbPayload p;
      portENTER_CRITICAL(&mux_);
      p = pending_;
      portEXIT_CRITICAL(&mux_);
      if (p.seq == 0 || p.seq == lastSentSeq) continue;  // nothing new yet

      lastSendMs = millis();
      lastSentSeq = p.seq;
      const String body = buildJson(p);

      const uint32_t t0 = millis();
      int code = -1;
      if (http.begin(client, kFirebaseHost, 443, path, true)) {
        http.addHeader("Content-Type", "application/json");
        code = http.sendRequest("PATCH", (uint8_t*)body.c_str(), body.length());
        http.end();
      }
      lastDurMs_ = millis() - t0;
      lastHttp_ = code;
      if (code == 200 || code == 204) ++okCount_;
      else {
        ++failCount_;
        client.stop();  // force a fresh TLS connection next time
      }
    }
  }

  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  FbPayload pending_;
  uint32_t seq_ = 0;
  volatile bool wifiUp_ = false;
  volatile int lastHttp_ = 0;
  volatile uint32_t lastDurMs_ = 0, okCount_ = 0, failCount_ = 0;
};
