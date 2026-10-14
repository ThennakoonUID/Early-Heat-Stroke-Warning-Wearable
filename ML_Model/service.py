"""
service.py - live heat-strain inference from Firebase.

Reads /devices/<id> (written by the ESP32 every ~2 s), builds a 1-minute time
series, runs the validated core-temperature model and writes the result to
/devices/<id>/prediction. Uses the SAME features.py as the training notebook.

    python service.py --config config.json                 # live (Firebase)
    python service.py --replay examples/replay_example.csv  # offline test, no Firebase

Credentials: path to a Firebase service-account JSON in the environment variable
FIREBASE_CREDENTIALS (or GOOGLE_APPLICATION_CREDENTIALS). Never commit that file.
"""
from __future__ import annotations

import argparse
import json
import logging
import math
import os
import sqlite3
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone

import joblib
import numpy as np
import pandas as pd

import features as F

LOG = logging.getLogger("heatstrain")
KEYS = ("skin_temp", "outside_temp", "feels_like_temp", "heart_rate", "sweat_level")

DEFAULT_CONFIG = {
    "database_url": "https://heatstrockapp-default-rtdb.asia-southeast1.firebasedatabase.app",
    "device_path": "/devices/DEV-001",
    "model_path": "models/core_temp_model.joblib",
    "db_path": "data/live_readings.sqlite",
    "sample_period_s": 2.0,         # snapshot of the Firebase values this often (ESP32 period)
    "stale_after_s": 30.0,          # no change event for this long -> device "stale"
    "poll_period_s": 30.0,          # health check / catch-up read of the device path
    "session_gap_min": 60,          # no valid data for > 60 min -> new session + new baseline (core temp needs ~30-60 min to recover)
    "skin_invalid_max_min": 3,      # skin missing this long -> "insufficient_data"
    "hr_missing_policy": "fallback",  # HR missing > 5 min: "fallback" = skin-only model, "insufficient" = no prediction
    "write_back": True,
}


# ------------------------------- parsing --------------------------------------
def parse_value(key: str, v) -> float:
    """Firebase value -> float, NaN for null, error strings or garbage (never 0)."""
    if v is None or isinstance(v, bool):
        return math.nan
    if isinstance(v, (int, float)):
        return float(v) if math.isfinite(v) else math.nan
    if isinstance(v, str):
        if v.strip().upper().startswith("ERR"):
            return math.nan
        try:
            x = float(v)
            return x if math.isfinite(x) else math.nan
        except ValueError:
            return math.nan
    return math.nan


def _nanmedian(xs):
    a = np.asarray([x for x in xs if np.isfinite(x)], dtype=float)
    return float(np.median(a)) if a.size else math.nan


# ------------------------------- pipeline -------------------------------------
@dataclass
class Session:
    sid: int
    start: datetime
    rows: list = field(default_factory=list)   # one dict per minute
    last_valid: datetime | None = None


class CoreTempPipeline:
    """Firebase-free core: 2-s snapshots in, one prediction per minute out."""

    def __init__(self, bundle: dict, cfg: dict):
        if bundle["feature_version"] != F.FEATURE_VERSION:
            raise RuntimeError(f"model was trained with features v{bundle['feature_version']}, "
                               f"features.py is v{F.FEATURE_VERSION}")
        self.b, self.cfg = bundle, cfg
        self.buf: list[dict] = []
        self.session: Session | None = None
        self.next_sid = 1

    def add_snapshot(self, values: dict, stale: bool):
        """values: parsed floats for KEYS. A stale device contributes nothing (frozen values)."""
        if stale:
            values = {k: math.nan for k in KEYS}
        self.buf.append(values)

    def on_minute(self, minute: datetime, device_status: str = "online") -> dict:
        snap, self.buf = self.buf, []
        agg = {k: _nanmedian([s[k] for s in snap]) for k in KEYS}
        row = {"hr": agg["heart_rate"], "skin": agg["skin_temp"], "amb": agg["outside_temp"],
               "hi": agg["feels_like_temp"], "sweat_level": agg["sweat_level"]}
        valid = np.isfinite(row["skin"]) or np.isfinite(row["hr"])

        # ---- session handling ----
        gap = timedelta(minutes=self.cfg["session_gap_min"])
        if self.session and self.session.last_valid and minute - self.session.last_valid > gap:
            LOG.info("session %d ended (no valid data for > %s)", self.session.sid, gap)
            self.session = None
        if self.session is None and valid:
            self.session = Session(self.next_sid, minute)
            self.next_sid += 1
            LOG.info("session %d started at %s", self.session.sid, minute.isoformat())

        out = {"minute_utc": minute.isoformat(), "session_id": None, "t_min": None,
               "core_temp_est": None, "psi": None, "risk_level": "insufficient_data",
               "model_used": None, "device_status": device_status, **row}
        if self.session is None:
            return out
        s = self.session
        if valid:
            s.last_valid = minute
        row["t_min"] = len(s.rows)           # one row per minute since the session start
        s.rows.append(row)
        out.update(session_id=s.sid, t_min=row["t_min"])

        sess = pd.DataFrame(s.rows)
        f = F.build_features(sess[["t_min", "hr", "skin", "amb", "hi"]])
        last = f.iloc[[-1]]

        if device_status == "stale":                     # never show "normal" for a vest that stopped sending
            return out                                   # insufficient_data
        if bool(last["warming_up"].iloc[0]) or not np.isfinite(last["sk0"].iloc[0]):
            out["risk_level"] = "warming_up"
            return out
        skin_missing = sess["skin"].tail(self.cfg["skin_invalid_max_min"]).isna().all()
        if skin_missing or not np.isfinite(last["skin"].iloc[0]):
            return out                                   # insufficient_data
        if np.isfinite(last["hr"].iloc[0]) and np.isfinite(last["hr0"].iloc[0]):
            model, feats, used = self.b["model"], self.b["features"], self.b["model_name"]
            high = self.b["alarm_threshold"]
        elif self.cfg["hr_missing_policy"] == "fallback":
            model, feats, used = self.b["nohr_model"], self.b["nohr_features"], "no-HR fallback"
            high = self.b["nohr_alarm_threshold"]
        else:
            return out
        tc = float(model.predict(last[feats])[0])
        caution = min(self.b["caution_threshold"], high - 0.2)
        out.update(core_temp_est=round(tc, 1), risk_level=F.risk_level(tc, high, caution), model_used=used)
        hr, hr0 = last["hr"].iloc[0], last["hr0"].iloc[0]
        if np.isfinite(hr) and np.isfinite(hr0) and hr0 < 180:
            out["psi"] = round(float(F.psi(tc, hr, 37.0, hr0)), 1)
        return out


# ------------------------------- storage --------------------------------------
class Store:
    def __init__(self, path: str):
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        self.con = sqlite3.connect(path, check_same_thread=False)
        self.lock = threading.Lock()
        with self.lock:
            self.con.executescript("""
            CREATE TABLE IF NOT EXISTS readings(ts_utc TEXT, skin_temp_raw TEXT, skin_temp REAL,
                outside_temp REAL, feels_like_temp REAL, heart_rate REAL, sweat_level REAL, device_stale INTEGER);
            CREATE TABLE IF NOT EXISTS events(ts_utc TEXT, event_type TEXT, path TEXT, data_json TEXT);
            CREATE TABLE IF NOT EXISTS predictions(minute_utc TEXT, session_id INTEGER, t_min INTEGER,
                hr REAL, skin REAL, amb REAL, hi REAL, sweat_level REAL, core_temp_est REAL, psi REAL,
                risk_level TEXT, model_used TEXT, device_status TEXT);""")

    def reading(self, ts, raw_skin, v, stale):
        with self.lock:
            self.con.execute("INSERT INTO readings VALUES (?,?,?,?,?,?,?,?)",
                             (ts, None if raw_skin is None else str(raw_skin), *[_sql(v[k]) for k in KEYS], int(stale)))
            self.con.commit()

    def event(self, ts, etype, path, data):
        with self.lock:
            self.con.execute("INSERT INTO events VALUES (?,?,?,?)", (ts, etype, path, json.dumps(data, default=str)))
            self.con.commit()

    def prediction(self, o):
        cols = ["minute_utc", "session_id", "t_min", "hr", "skin", "amb", "hi", "sweat_level",
                "core_temp_est", "psi", "risk_level", "model_used", "device_status"]
        with self.lock:
            self.con.execute(f"INSERT INTO predictions VALUES ({','.join('?' * len(cols))})", [_sql(o.get(c)) for c in cols])
            self.con.commit()


def _sql(v):
    return None if isinstance(v, float) and not math.isfinite(v) else v


# ------------------------------- Firebase -------------------------------------
class FirebaseService:
    def __init__(self, cfg: dict, pipe: CoreTempPipeline, store: Store):
        import firebase_admin
        from firebase_admin import credentials, db
        key = os.environ.get("FIREBASE_CREDENTIALS") or os.environ.get("GOOGLE_APPLICATION_CREDENTIALS")
        if not key or not os.path.exists(key):
            raise SystemExit("Set FIREBASE_CREDENTIALS to the path of your service-account JSON file.")
        firebase_admin.initialize_app(credentials.Certificate(key), {"databaseURL": cfg["database_url"]})
        self.db, self.cfg, self.pipe, self.store = db, cfg, pipe, store
        self.ref = db.reference(cfg["device_path"])
        self.state = {k: None for k in KEYS}
        self.lock = threading.Lock()
        self.last_change = -math.inf    # monotonic time of the last change event
        self.listener = None
        self.synced = False             # state holds a first snapshot of the device node
        self.first_event = False        # the next event is the initial snapshot of a new listener

    # Firebase only fires an event when a value CHANGES (identical PATCHes are silent).
    def _on_event(self, ev):
        now = datetime.now(timezone.utc).isoformat()
        path, data = ev.path or "/", ev.data
        if path.startswith("/prediction"):
            return                                  # our own write-back
        self.store.event(now, ev.event_type, path, data)
        with self.lock:
            first, self.first_event = self.first_event, False
            before = dict(self.state)
            if path == "/":
                if ev.event_type == "put":         # full replacement (keys absent = deleted)
                    d = data if isinstance(data, dict) else {}
                    for k in KEYS:
                        self.state[k] = d.get(k)
                elif isinstance(data, dict):       # patch: only the given keys
                    for k in KEYS:
                        if k in data:
                            self.state[k] = data[k]
            else:
                key = path.strip("/").split("/")[0]
                if key in KEYS:
                    self.state[key] = data
            # A new listener first sends the whole node. Those values have no known age (they can be
            # leftovers from yesterday), so they only count as a change if they differ from what we had.
            if not first or (self.synced and self.state != before):
                self.last_change = time.monotonic()
            self.synced = True

    def _connect(self):
        if self.listener is not None:
            try:
                self.listener.close()
            except Exception:
                pass
        with self.lock:
            self.first_event = True
        self.listener = self.ref.listen(self._on_event)
        LOG.info("listening to %s", self.cfg["device_path"])

    def _poll(self):
        """Health check + catch-up: re-read the device node; reconnect the listener if it died."""
        d = self.ref.get()
        with self.lock:
            if isinstance(d, dict):
                changed = any(d.get(k) != self.state[k] for k in KEYS)
                for k in KEYS:
                    self.state[k] = d.get(k)
                if changed and self.synced:      # the very first read is of unknown age, see _on_event
                    self.last_change = time.monotonic()
                self.synced = True
        alive = getattr(getattr(self.listener, "_thread", None), "is_alive", lambda: True)()
        if not alive:
            LOG.warning("listener thread died, reconnecting")
            self._connect()

    def run(self):
        backoff = 1
        while True:
            try:
                self._connect()
                break
            except Exception as e:
                LOG.error("connect failed: %s (retry in %ss)", e, backoff)
                time.sleep(backoff); backoff = min(backoff * 2, 60)
        next_sample = time.monotonic()
        next_poll = time.monotonic()
        cur_min = datetime.now(timezone.utc).replace(second=0, microsecond=0)
        while True:
            now = time.monotonic()
            if now >= next_poll:
                next_poll = now + self.cfg["poll_period_s"]
                try:
                    self._poll(); backoff = 1
                except Exception as e:
                    LOG.error("Firebase read failed: %s", e)
            if now >= next_sample:
                next_sample += self.cfg["sample_period_s"]
                stale = (now - self.last_change) > self.cfg["stale_after_s"]
                with self.lock:
                    raw = dict(self.state)
                vals = {k: parse_value(k, raw[k]) for k in KEYS}
                self.pipe.add_snapshot(vals, stale)
                self.store.reading(datetime.now(timezone.utc).isoformat(), raw["skin_temp"], vals, stale)
            wall = datetime.now(timezone.utc).replace(second=0, microsecond=0)
            if wall > cur_min:
                stale = (time.monotonic() - self.last_change) > self.cfg["stale_after_s"]
                out = self.pipe.on_minute(cur_min, "stale" if stale else "online")
                cur_min = wall
                self.store.prediction(out)
                LOG.info("%s | %s", out["minute_utc"], _fmt(out))
                if self.cfg["write_back"]:
                    self._write(out)
            time.sleep(0.2)

    def _write(self, o):
        payload = {"core_temp_est": o["core_temp_est"], "psi": o["psi"], "risk_level": o["risk_level"],
                   "model_version": self.pipe.b["model_version"], "model_used": o["model_used"],
                   "device_status": o["device_status"], "updated_at": {".sv": "timestamp"}}
        try:
            self.ref.child("prediction").update(payload)     # update(), never set(): ESP32 keys untouched
        except Exception as e:
            LOG.error("write-back failed: %s", e)


def _fmt(o):
    tc = "--" if o["core_temp_est"] is None else f"{o['core_temp_est']:.1f}C"
    ps = "--" if o["psi"] is None else f"{o['psi']:.1f}"
    return (f"session {o['session_id']} t={o['t_min']} | HR {o['hr']} skin {o['skin']} amb {o['amb']} | "
            f"Tc_hat {tc} PSI {ps} -> {o['risk_level']} ({o['model_used']}, device {o['device_status']})")


# ------------------------------- replay ---------------------------------------
def replay(csv_path: str, pipe: CoreTempPipeline, cfg: dict, out_csv: str | None):
    """Feed recorded 2-s snapshots (columns: timestamp + the five Firebase keys) through
    exactly the same pipeline. Timestamps drive the clock, gaps > stale_after_s mark the device stale."""
    df = pd.read_csv(csv_path)
    ts = pd.to_datetime(df["timestamp"], utc=True)
    results, cur_min, last_ts = [], None, None
    for t, (_, r) in zip(ts, df.iterrows()):
        t = t.to_pydatetime()
        m = t.replace(second=0, microsecond=0)
        if cur_min is None:
            cur_min = m
        while m > cur_min:                             # close every finished minute (also empty ones)
            stale = last_ts is None or (cur_min + timedelta(minutes=1) - last_ts).total_seconds() > cfg["stale_after_s"]
            results.append(pipe.on_minute(cur_min, "stale" if stale else "online"))
            cur_min += timedelta(minutes=1)
        raw = {k: (None if pd.isna(r.get(k)) else r.get(k)) for k in KEYS}
        pipe.add_snapshot({k: parse_value(k, raw[k]) for k in KEYS}, stale=False)
        last_ts = t
    results.append(pipe.on_minute(cur_min, "online"))
    res = pd.DataFrame(results)
    for _, o in res.iterrows():
        LOG.info("%s | %s", o["minute_utc"], _fmt(o))
    if out_csv:
        res.to_csv(out_csv, index=False)
        LOG.info("wrote %s", out_csv)
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", default="config.json")
    ap.add_argument("--replay", help="CSV of recorded snapshots instead of Firebase")
    ap.add_argument("--out", help="replay: write per-minute results to this CSV")
    ap.add_argument("--log-level", default="INFO")
    a = ap.parse_args()
    logging.basicConfig(level=a.log_level, format="%(asctime)s %(levelname)s %(message)s")
    cfg = dict(DEFAULT_CONFIG)
    if os.path.exists(a.config):
        with open(a.config) as fh:
            cfg.update(json.load(fh))
    elif not a.replay:
        raise SystemExit(f"config file {a.config} not found (copy config.example.json)")
    pipe = CoreTempPipeline(joblib.load(cfg["model_path"]), cfg)
    LOG.info("model %s (%s)", pipe.b["model_version"], pipe.b["model_name"])
    if a.replay:
        replay(a.replay, pipe, cfg, a.out)
        return
    try:
        FirebaseService(cfg, pipe, Store(cfg["db_path"])).run()
    except KeyboardInterrupt:
        LOG.info("stopped (Ctrl+C)")
        os._exit(0)      # the Firebase listener thread is not a daemon and would keep the process alive


if __name__ == "__main__":
    main()
