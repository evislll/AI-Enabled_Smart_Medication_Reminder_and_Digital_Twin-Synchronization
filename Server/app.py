from flask import Flask, request, jsonify, render_template_string, Response
import sqlite3
from datetime import datetime, timedelta
import csv
import io
import os
from datetime import datetime
from zoneinfo import ZoneInfo
import joblib


#######################################################################################################################################
##------------This file is mostly AI genrated except all the HTML part and I mentioned the parts as NON-AI in this file--------------##
#######################################################################################################################################


app = Flask(__name__)
DB_NAME = "med_logs.db"
MODEL_FILE = "med_model_sequence.pkl"
PRETRAINED_MODEL = None
WINDOW_SIZE = 5
PENDING_COMMAND = "NONE"

CURRENT_TWIN_STATE = {
    "timestamp": None,
    "mcu_timestamp_ms": None,
    "reminder_active": False,
    "ai_high_risk": False,
    "servo_state_deg": None,
    "weight_grams": None,
    "reminder_state": "UNKNOWN",
    "servo_state": "UNKNOWN",
    "tray_state": "UNKNOWN",
    "weight_source": "UNKNOWN",
    "last_command": "NONE",
    "last_command_result": "NONE",
    "last_command_time": None
}

# AI policy config
# HIGH_RISK_THRESHOLD = 0.60
HIGH_RISK_THRESHOLD = 0.20

# DEMO_PREALERT_LEAD_SEC = 30
DEMO_PREALERT_LEAD_SEC = 15
REAL_PREALERT_LEAD_SEC = 300
USE_REAL_LEAD_TIME = False

# Online memory config
EW_ALPHA = 0.35
VOL_ALPHA = 0.25
PROFILE_RISK_WEIGHT = 0.12

# Bounded adaptive fusion config
MAX_ADAPTIVE_UPLIFT = 0.18
MAX_ADAPTIVE_DROP = 0.15

# -----------------------------
# Digital Twin live state
# -----------------------------
# CURRENT_TWIN_STATE = {
#     "timestamp": None,          # server receive time (ISO)
#     "mcu_timestamp_ms": None,   # raw MCU / RTOS tick ms
#     "reminder_active": False,
#     "ai_high_risk": False,
#     "servo_state_deg": None, 
#           # None = unknown
#     "weight_grams": None        # None = unknown
# }


def init_db():
    conn = sqlite3.connect(DB_NAME)
    cur = conn.cursor()

    cur.execute("""
        CREATE TABLE IF NOT EXISTS events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp TEXT NOT NULL,
            med_id TEXT NOT NULL,
            status TEXT NOT NULL,
            reminder_count INTEGER NOT NULL,
            delay_minutes INTEGER NOT NULL,
            visible INTEGER NOT NULL DEFAULT 1
        )
    """)

    cur.execute("PRAGMA table_info(events)")
    cols = [row[1] for row in cur.fetchall()]
    if "visible" not in cols:
        cur.execute("ALTER TABLE events ADD COLUMN visible INTEGER NOT NULL DEFAULT 1")


#############-------------NON-AI (FROM LINE 97 TO 153)-------------------##############


    cur.execute("""
        CREATE TABLE IF NOT EXISTS med_profiles (
            med_id TEXT PRIMARY KEY,
            ew_miss_rate REAL NOT NULL DEFAULT 0.0,
            ew_snooze_rate REAL NOT NULL DEFAULT 0.0,
            ew_taken_rate REAL NOT NULL DEFAULT 0.0,
            ew_delay REAL NOT NULL DEFAULT 0.0,
            ew_reminders REAL NOT NULL DEFAULT 0.0,
            adherence_volatility REAL NOT NULL DEFAULT 0.0,
            last_risk REAL NOT NULL DEFAULT 0.0,
            updated_at TEXT NOT NULL
        )
    """)

    cur.execute("""
        CREATE TABLE IF NOT EXISTS dispense_verification_logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp TEXT NOT NULL,
            med_id TEXT NOT NULL,
            result TEXT NOT NULL,
            pre_weight_g REAL NOT NULL,
            post_weight_g REAL NOT NULL,
            delta_g REAL NOT NULL,
            threshold_g REAL NOT NULL
        )
    """)

    cur.execute("""
        CREATE TABLE IF NOT EXISTS risk_weights (
            med_id TEXT PRIMARY KEY,
            w_missed REAL NOT NULL,
            w_reminder REAL NOT NULL,
            w_failure REAL NOT NULL,
            w_ack REAL NOT NULL,
            updated_at TEXT NOT NULL
        )
    """)

    cur.execute("""
        CREATE TABLE IF NOT EXISTS recovered_flash_logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp TEXT NOT NULL,
            seq INTEGER NOT NULL,
            event_type TEXT NOT NULL,
            med_id TEXT NOT NULL,
            src TEXT NOT NULL,
            status TEXT NOT NULL,
            reminder_count INTEGER NOT NULL DEFAULT 0,
            delay_minutes INTEGER NOT NULL DEFAULT 0,
            val INTEGER NOT NULL DEFAULT 0,
            extra INTEGER NOT NULL DEFAULT 0,
            extra2 INTEGER NOT NULL DEFAULT 0,
            flags INTEGER NOT NULL DEFAULT 0,
            crc INTEGER NOT NULL DEFAULT 0,
            received_at TEXT NOT NULL
        )
    """)

    conn.commit()
    conn.close()


def load_pretrained_model():
    global PRETRAINED_MODEL
    if os.path.exists(MODEL_FILE):
        PRETRAINED_MODEL = joblib.load(MODEL_FILE)
        print(f"Loaded pretrained model from {MODEL_FILE}")
    else:
        PRETRAINED_MODEL = None
        print(f"No pretrained model found at {MODEL_FILE}. Using fallback analytics.")


init_db()
load_pretrained_model()


def db_query(sql, params=(), fetch=True):
    conn = sqlite3.connect(DB_NAME)
    cur = conn.cursor()
    cur.execute(sql, params)
    rows = cur.fetchall() if fetch else None
    conn.commit()
    conn.close()
    return rows


def save_risk_weights(med_id, w_missed, w_reminder, w_failure, w_ack):
    db_query("""
        INSERT INTO risk_weights (
            med_id, w_missed, w_reminder, w_failure, w_ack, updated_at
        )
        VALUES (?, ?, ?, ?, ?, datetime('now', 'localtime'))
        ON CONFLICT(med_id) DO UPDATE SET
            w_missed = excluded.w_missed,
            w_reminder = excluded.w_reminder,
            w_failure = excluded.w_failure,
            w_ack = excluded.w_ack,
            updated_at = datetime('now', 'localtime')
    """, (med_id, w_missed, w_reminder, w_failure, w_ack), fetch=False)


def parse_dt(ts: str):
    try:
        return datetime.fromisoformat(ts.replace("Z", "").replace(" ", "T"))
    except Exception:
        return None


def clamp(x, lo=0.0, hi=1.0):
    return max(lo, min(hi, x))


def safe_int(v, default=0):
    try:
        return int(v)
    except Exception:
        return default


def safe_float(v, default=0.0):
    try:
        return float(v)
    except Exception:
        return default


def safe_bool_int(v, default=False):
    try:
        return bool(int(v))
    except Exception:
        return default


def status_to_code(status: str) -> float:
    s = (status or "").upper()
    if s == "TAKEN":
        return 0.0
    if s == "SNOOZED":
        return 0.5
    if s == "MISSED":
        return 1.0
    return 0.0


# -----------------------------
# Sequence-aware feature building + prediction
# -----------------------------
def build_sequence_inference_features(history_events, target_dt):
    """
    history_events: last WINDOW_SIZE events for this medication
    target_dt: datetime of next scheduled dose
    """
    x = []

    hour = target_dt.hour if target_dt else 0
    dow = target_dt.weekday() if target_dt else 0

    # Target time context
    x.append(hour / 23.0)
    for i in range(7):
        x.append(1.0 if dow == i else 0.0)

    # Left-pad if fewer than WINDOW_SIZE events exist
    padded = []
    missing = max(0, WINDOW_SIZE - len(history_events))

    for _ in range(missing):
        padded.append({
            "status": "TAKEN",
            "reminder_count": 0,
            "delay_minutes": 0
        })

    padded.extend(history_events[-WINDOW_SIZE:])

    # Last N statuses
    for ev in padded:
        x.append(status_to_code(ev.get("status", "TAKEN")))

    # Last N reminders
    for ev in padded:
        x.append(clamp(safe_float(ev.get("reminder_count", 0)) / 10.0, 0.0, 1.0))

    # Last N delays
    for ev in padded:
        x.append(clamp(safe_float(ev.get("delay_minutes", 0)) / 60.0, 0.0, 1.0))

    # Rolling summary of padded history
    miss_count = sum(1 for ev in padded if (ev.get("status", "") or "").upper() == "MISSED")
    snooze_count = sum(1 for ev in padded if (ev.get("status", "") or "").upper() == "SNOOZED")
    taken_count = sum(1 for ev in padded if (ev.get("status", "") or "").upper() == "TAKEN")

    avg_rem = sum(safe_float(ev.get("reminder_count", 0)) for ev in padded) / len(padded)
    avg_delay = sum(safe_float(ev.get("delay_minutes", 0)) for ev in padded) / len(padded)

    x.append(miss_count / len(padded))
    x.append(snooze_count / len(padded))
    x.append(taken_count / len(padded))
    x.append(clamp(avg_rem / 10.0, 0.0, 1.0))
    x.append(clamp(avg_delay / 60.0, 0.0, 1.0))

    return x


def predict_with_sequence_model(model, history_events, target_dt):
    x = build_sequence_inference_features(history_events, target_dt)
    proba = model.predict_proba([x])[0][1]  # probability of MISSED
    return float(proba)


# def risk_label(risk: float) -> str:
#     if risk < 0.30:
#         return "LOW"
#     if risk < 0.60:
#         return "MEDIUM"
#     return "HIGH"



def risk_label(risk: float) -> str:
    if risk < 0.10:
        return "LOW"
    if risk < 0.20:
        return "MEDIUM"
    return "HIGH"


def select_lead_sec(high_risk: bool) -> int:
    if not high_risk:
        return 0
    return REAL_PREALERT_LEAD_SEC if USE_REAL_LEAD_TIME else DEMO_PREALERT_LEAD_SEC


# -----------------------------
# Online memory helpers
# -----------------------------
def get_med_profile(med_id: str):
    rows = db_query("""
        SELECT med_id, ew_miss_rate, ew_snooze_rate, ew_taken_rate,
               ew_delay, ew_reminders, adherence_volatility, last_risk, updated_at
        FROM med_profiles
        WHERE med_id=?
    """, (med_id,))
    if not rows:
        return None

    r = rows[0]
    return {
        "med_id": r[0],
        "ew_miss_rate": safe_float(r[1]),
        "ew_snooze_rate": safe_float(r[2]),
        "ew_taken_rate": safe_float(r[3]),
        "ew_delay": safe_float(r[4]),
        "ew_reminders": safe_float(r[5]),
        "adherence_volatility": safe_float(r[6]),
        "last_risk": safe_float(r[7]),
        "updated_at": r[8]
    }


def upsert_med_profile(profile: dict):
    db_query("""
        INSERT INTO med_profiles (
            med_id, ew_miss_rate, ew_snooze_rate, ew_taken_rate,
            ew_delay, ew_reminders, adherence_volatility, last_risk, updated_at
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(med_id) DO UPDATE SET
            ew_miss_rate=excluded.ew_miss_rate,
            ew_snooze_rate=excluded.ew_snooze_rate,
            ew_taken_rate=excluded.ew_taken_rate,
            ew_delay=excluded.ew_delay,
            ew_reminders=excluded.ew_reminders,
            adherence_volatility=excluded.adherence_volatility,
            last_risk=excluded.last_risk,
            updated_at=excluded.updated_at
    """, (
        profile["med_id"],
        profile["ew_miss_rate"],
        profile["ew_snooze_rate"],
        profile["ew_taken_rate"],
        profile["ew_delay"],
        profile["ew_reminders"],
        profile["adherence_volatility"],
        profile["last_risk"],
        profile["updated_at"]
    ), fetch=False)


def default_med_profile(med_id: str):
    now_iso = datetime.now().isoformat(timespec="seconds")
    return {
        "med_id": med_id,
        "ew_miss_rate": 0.0,
        "ew_snooze_rate": 0.0,
        "ew_taken_rate": 1.0,
        "ew_delay": 0.0,
        "ew_reminders": 0.0,
        "adherence_volatility": 0.0,
        "last_risk": 0.0,
        "updated_at": now_iso
    }


def ew_update(old_value: float, observation: float, alpha: float = EW_ALPHA):
    return alpha * observation + (1.0 - alpha) * old_value


def event_status_observation(status: str):
    s = (status or "").upper()
    return {
        "miss": 1.0 if s == "MISSED" else 0.0,
        "snooze": 1.0 if s == "SNOOZED" else 0.0,
        "taken": 1.0 if s == "TAKEN" else 0.0,
        "problem": 1.0 if s in ("MISSED", "SNOOZED") else 0.0
    }


def update_online_med_profile(med_id: str, status: str, reminder_count: int, delay_minutes: int):
    profile = get_med_profile(med_id)
    if profile is None:
        profile = default_med_profile(med_id)

    obs = event_status_observation(status)
    delay_norm = clamp(safe_float(delay_minutes) / 60.0, 0.0, 1.0)
    rem_norm = clamp(safe_float(reminder_count) / 10.0, 0.0, 1.0)

    new_ew_miss = ew_update(profile["ew_miss_rate"], obs["miss"])
    new_ew_snooze = ew_update(profile["ew_snooze_rate"], obs["snooze"])
    new_ew_taken = ew_update(profile["ew_taken_rate"], obs["taken"])
    new_ew_delay = ew_update(profile["ew_delay"], delay_norm)
    new_ew_reminders = ew_update(profile["ew_reminders"], rem_norm)

    prev_problem_signal = clamp(profile["ew_miss_rate"] + 0.5 * profile["ew_snooze_rate"], 0.0, 1.0)
    current_problem_signal = clamp(obs["miss"] + 0.5 * obs["snooze"], 0.0, 1.0)
    change_mag = abs(current_problem_signal - prev_problem_signal)
    new_volatility = VOL_ALPHA * change_mag + (1.0 - VOL_ALPHA) * profile["adherence_volatility"]

    profile["ew_miss_rate"] = clamp(new_ew_miss)
    profile["ew_snooze_rate"] = clamp(new_ew_snooze)
    profile["ew_taken_rate"] = clamp(new_ew_taken)
    profile["ew_delay"] = clamp(new_ew_delay)
    profile["ew_reminders"] = clamp(new_ew_reminders)
    profile["adherence_volatility"] = clamp(new_volatility)
    profile["updated_at"] = datetime.now().isoformat(timespec="seconds")

    upsert_med_profile(profile)
    return profile


def compute_profile_risk_signal(profile: dict):
    if not profile:
        return {
            "profile_risk": 0.0,
            "profile_protective": 0.0,
            "profile_net": 0.0
        }

    risk_signal = 0.0
    risk_signal += 0.45 * clamp(profile["ew_miss_rate"])
    risk_signal += 0.20 * clamp(profile["ew_snooze_rate"])
    risk_signal += 0.15 * clamp(profile["ew_delay"])
    risk_signal += 0.10 * clamp(profile["ew_reminders"])
    risk_signal += 0.10 * clamp(profile["adherence_volatility"])

    protective_signal = 0.25 * clamp(profile["ew_taken_rate"])
    net = clamp(risk_signal - protective_signal, 0.0, 1.0)

    return {
        "profile_risk": round(risk_signal, 3),
        "profile_protective": round(protective_signal, 3),
        "profile_net": round(net, 3)
    }


# -----------------------------
# API: ingest + fetch
# -----------------------------
@app.route("/api/events", methods=["POST"])
def receive_event():
    data = request.get_json(force=True, silent=True) or {}

    timestamp = str(data.get("timestamp", datetime.now().isoformat(timespec="seconds")))
    med_id = str(data.get("med_id", "unknown")).strip() or "unknown"
    status = str(data.get("status", "unknown")).upper().strip()
    reminder_count = int(data.get("reminder_count", 0))
    delay_minutes = int(data.get("delay_minutes", 0))

    if status not in ("TAKEN", "MISSED", "SNOOZED"):
        status = "UNKNOWN"

    db_query("""
        INSERT INTO events (timestamp, med_id, status, reminder_count, delay_minutes, visible)
        VALUES (?, ?, ?, ?, ?, 1)
    """, (timestamp, med_id, status, reminder_count, delay_minutes), fetch=False)

    profile = update_online_med_profile(
        med_id=med_id,
        status=status,
        reminder_count=reminder_count,
        delay_minutes=delay_minutes
    )

    return jsonify({
        "message": "Event logged successfully",
        "profile_updated": True,
        "profile": profile
    }), 201


@app.route("/api/events", methods=["GET"])
def get_events():
    limit = request.args.get("limit", default=100, type=int)
    limit = max(1, min(limit, 100000))

    med = request.args.get("med", default=None, type=str)
    if med:
        rows = db_query(
            "SELECT id, timestamp, med_id, status, reminder_count, delay_minutes "
            "FROM events WHERE med_id=? AND visible=1 ORDER BY id DESC LIMIT ?",
            (med, limit)
        )
    else:
        rows = db_query(
            "SELECT id, timestamp, med_id, status, reminder_count, delay_minutes "
            "FROM events WHERE visible=1 ORDER BY id DESC LIMIT ?",
            (limit,)
        )

    events = [{
        "id": r[0],
        "timestamp": r[1],
        "med_id": r[2],
        "status": r[3],
        "reminder_count": r[4],
        "delay_minutes": r[5]
    } for r in rows]

    return jsonify(events)


@app.route("/api/recovered_logs", methods=["POST"])
def receive_recovered_logs():
    data = request.get_json(force=True, silent=True)

    if not data:
        return jsonify({"error": "Empty or invalid JSON payload"}), 400

    logs = data.get("logs")
    if logs is None:
        logs = [data]

    if not isinstance(logs, list) or len(logs) == 0:
        return jsonify({"error": "Payload must contain a non-empty log or logs list"}), 400

    inserted = 0
    skipped = 0
    now_iso = datetime.now().isoformat(timespec="seconds")

    for item in logs:
        if not isinstance(item, dict):
            skipped += 1
            continue

        # Required fields for a valid recovered flash log
        required_fields = [
            "timestamp", "seq", "event_type", "med_id", "src",
            "status", "val", "extra", "extra2", "flags", "crc"
        ]

        if any(field not in item for field in required_fields):
            skipped += 1
            continue

        timestamp = str(item["timestamp"]).strip()
        seq = safe_int(item["seq"], -1)
        event_type = str(item["event_type"]).strip().upper()
        med_id = str(item["med_id"]).strip()
        src = str(item["src"]).strip().upper()
        status = str(item["status"]).strip().upper()
        reminder_count = safe_int(item.get("reminder_count", 0))
        delay_minutes = safe_int(item.get("delay_minutes", 0))
        val = safe_int(item["val"], 0)
        extra = safe_int(item["extra"], 0)
        extra2 = safe_int(item["extra2"], 0)
        flags = safe_int(item["flags"], 0)
        crc = safe_int(item["crc"], 0)

        # Reject obviously bogus rows
        if not timestamp or seq < 0 or not event_type or not med_id or not src or not status:
            skipped += 1
            continue

        existing = db_query("""
            SELECT id
            FROM recovered_flash_logs
            WHERE timestamp=?
              AND seq=?
              AND event_type=?
              AND med_id=?
              AND src=?
              AND status=?
              AND reminder_count=?
              AND delay_minutes=?
              AND val=?
              AND extra=?
              AND extra2=?
              AND flags=?
              AND crc=?
            LIMIT 1
        """, (
            timestamp,
            seq,
            event_type,
            med_id,
            src,
            status,
            reminder_count,
            delay_minutes,
            val,
            extra,
            extra2,
            flags,
            crc
        ))

        if existing:
            skipped += 1
            continue

        db_query("""
            INSERT INTO recovered_flash_logs (
                timestamp, seq, event_type, med_id, src, status,
                reminder_count, delay_minutes, val, extra, extra2, flags, crc, received_at
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, (
            timestamp,
            seq,
            event_type,
            med_id,
            src,
            status,
            reminder_count,
            delay_minutes,
            val,
            extra,
            extra2,
            flags,
            crc,
            now_iso
        ), fetch=False)

        inserted += 1

    return jsonify({
        "message": "Recovered flash logs processed",
        "inserted": inserted,
        "skipped": skipped,
        "received": len(logs)
    }), 201





@app.route("/api/recovered_logs", methods=["GET"])
def get_recovered_logs():
    limit = request.args.get("limit", default=50, type=int)
    limit = max(1, min(limit, 500))

    med = request.args.get("med", default=None, type=str)

    if med:
        rows = db_query("""
            SELECT id, timestamp, seq, event_type, med_id, src, status,
                   reminder_count, delay_minutes, val, extra, extra2, flags, crc, received_at
            FROM recovered_flash_logs
            WHERE med_id=?
            ORDER BY id DESC
            LIMIT ?
        """, (med, limit))
    else:
        rows = db_query("""
            SELECT id, timestamp, seq, event_type, med_id, src, status,
                   reminder_count, delay_minutes, val, extra, extra2, flags, crc, received_at
            FROM recovered_flash_logs
            ORDER BY id DESC
            LIMIT ?
        """, (limit,))

    logs = [{
        "id": r[0],
        "timestamp": r[1],
        "seq": r[2],
        "event_type": r[3],
        "med_id": r[4],
        "src": r[5],
        "status": r[6],
        "reminder_count": r[7],
        "delay_minutes": r[8],
        "val": r[9],
        "extra": r[10],
        "extra2": r[11],
        "flags": r[12],
        "crc": r[13],
        "received_at": r[14]
    } for r in rows]

    return jsonify(logs)




@app.route("/api/risk_weights", methods=["GET"])
def get_risk_weights():
    rows = db_query("""
        SELECT
            med_id,
            w_missed,
            w_reminder,
            w_failure,
            w_ack,
            updated_at
        FROM risk_weights
        ORDER BY med_id
    """)

    result = []
    for r in rows:
        result.append({
            "med_id": r[0],
            "w_missed": r[1],
            "w_reminder": r[2],
            "w_failure": r[3],
            "w_ack": r[4],
            "updated_at": r[5]
        })

    return jsonify(result), 200





@app.route("/api/risk_weights/update", methods=["POST"])
def update_risk_weights():
    data = request.get_json(force=True, silent=True) or {}

    med_id = str(data.get("med_id", "")).strip()
    if not med_id:
        return jsonify({"error": "med_id required"}), 400

    try:
        if "w_missed_milli" in data:
            w_missed = int(data.get("w_missed_milli", 0)) / 1000.0
            w_reminder = int(data.get("w_reminder_milli", 0)) / 1000.0
            w_failure = int(data.get("w_failure_milli", 0)) / 1000.0
            w_ack = int(data.get("w_ack_milli", 0)) / 1000.0
        else:
            w_missed = float(data.get("w_missed", 0.0))
            w_reminder = float(data.get("w_reminder", 0.0))
            w_failure = float(data.get("w_failure", 0.0))
            w_ack = float(data.get("w_ack", 0.0))
    except (TypeError, ValueError):
        return jsonify({"error": "invalid weight value"}), 400

    save_risk_weights(med_id, w_missed, w_reminder, w_failure, w_ack)

    return jsonify({
        "status": "ok",
        "med_id": med_id,
        "w_missed": w_missed,
        "w_reminder": w_reminder,
        "w_failure": w_failure,
        "w_ack": w_ack
    }), 200


@app.route("/api/recovered_risk_trend", methods=["GET"])
def get_recovered_risk_trend():
    med = request.args.get("med", "").strip()

    if med:
        rows = db_query("""
            SELECT med_id, timestamp, flags
            FROM recovered_flash_logs
            WHERE event_type = 'RISK_CYCLE' AND med_id = ?
            ORDER BY id ASC
        """, (med,))
    else:
        rows = db_query("""
            SELECT med_id, timestamp, flags
            FROM recovered_flash_logs
            WHERE event_type = 'RISK_CYCLE'
            ORDER BY id ASC
        """)

    out = []
    for r in rows:
        out.append({
            "med_id": r[0],
            "timestamp": r[1],
            "risk_percent": int(r[2] or 0)
        })

    return jsonify(out)


@app.route("/api/recovered_risk_latest", methods=["GET"])
def get_recovered_risk_latest():
    rows = db_query("""
        SELECT r1.med_id, r1.timestamp, r1.flags
        FROM recovered_flash_logs r1
        INNER JOIN (
            SELECT med_id, MAX(id) AS max_id
            FROM recovered_flash_logs
            WHERE event_type = 'RISK_CYCLE'
            GROUP BY med_id
        ) r2
        ON r1.id = r2.max_id
        ORDER BY r1.med_id ASC
    """)

    out = []
    for r in rows:
        risk_percent = int(r[2] or 0)
        if risk_percent >= 70:
            risk_label = "HIGH"
        elif risk_percent >= 40:
            risk_label = "MEDIUM"
        else:
            risk_label = "LOW"

        out.append({
            "med_id": r[0],
            "timestamp": r[1],
            "risk_percent": risk_percent,
            "risk_label": risk_label
        })

    return jsonify(out)


#######--------------------NON-AI (FROM LINE 846 - 875)-------------------#########




@app.route("/api/clear_recovered", methods=["POST"])
def clear_recovered():
    db_query("DELETE FROM recovered_flash_logs", fetch=False)
    return jsonify({"message": "Recovered logs cleared"}), 200

@app.route("/api/meds", methods=["GET"])
def get_meds():
    rows = db_query("SELECT DISTINCT med_id FROM events WHERE visible=1 ORDER BY med_id ASC")
    meds = [r[0] for r in rows]
    return jsonify(meds)


@app.route("/api/profile", methods=["GET"])
def get_profile():
    med = request.args.get("med", default=None, type=str)
    if not med:
        return jsonify({"error": "med query parameter required"}), 400

    profile = get_med_profile(med)
    if profile is None:
        return jsonify({"med_id": med, "profile_exists": False})

    profile_signal = compute_profile_risk_signal(profile)
    return jsonify({
        "med_id": med,
        "profile_exists": True,
        "profile": profile,
        "profile_signal": profile_signal
    })


# -----------------------------
# Digital Twin API
# -----------------------------
@app.route("/api/twin/update", methods=["POST"])
def twin_update():
    """
    MCU or serial bridge posts periodic twin snapshots here.
    Expected JSON:
    {
      "timestamp_ms": 123456,
      "reminder_active": 0,
      "ai_high_risk": 1,
      "servo_state_deg": 0,
      "weight_grams": -1,
      "reminder_state": "IDLE",
      "servo_state": "HOME",
      "tray_state": "EMPTY",
      "weight_source": "REAL"
    }
    """
    global CURRENT_TWIN_STATE

    data = request.get_json(force=True, silent=True) or {}

    mcu_ts_ms = safe_int(data.get("timestamp_ms"), None)
    reminder_active = safe_bool_int(data.get("reminder_active", 0), False)
    ai_high_risk = safe_bool_int(data.get("ai_high_risk", 0), False)
    servo_state_deg = safe_int(data.get("servo_state_deg", -1), -1)
    weight_grams = safe_int(data.get("weight_grams", -1), -1)

    reminder_state = str(data.get("reminder_state", "IDLE")).strip().upper()
    servo_state = str(data.get("servo_state", "UNKNOWN")).strip().upper()
    tray_state = str(data.get("tray_state", "UNKNOWN")).strip().upper()
    weight_source = str(data.get("weight_source", "REAL")).strip().upper()

    if servo_state_deg == -1:
        servo_state_deg = None
    if weight_grams == -1:
        weight_grams = None

    if reminder_state not in ("IDLE", "ACTIVE", "PREALERT", "GRACE", "UNKNOWN"):
        reminder_state = "UNKNOWN"

    if servo_state not in ("HOME", "MOVED", "DISPENSING", "DONE", "UNKNOWN"):
        servo_state = "UNKNOWN"

    if tray_state not in ("EMPTY", "PILL_PRESENT", "REMOVED", "UNKNOWN"):
        tray_state = "UNKNOWN"

    if weight_source not in ("REAL", "TWIN_OVERRIDE", "UNKNOWN"):
        weight_source = "UNKNOWN"

    CURRENT_TWIN_STATE = {
        "timestamp": datetime.now(ZoneInfo("America/Toronto")).isoformat(timespec="seconds"),
        "mcu_timestamp_ms": mcu_ts_ms,
        "reminder_active": reminder_active,
        "ai_high_risk": ai_high_risk,
        "servo_state_deg": servo_state_deg,
        "weight_grams": weight_grams,
        "reminder_state": reminder_state,
        "servo_state": servo_state,
        "tray_state": tray_state,
        "weight_source": weight_source
    }

    return jsonify({"status": "ok", "twin": CURRENT_TWIN_STATE}), 200


@app.route("/api/twin/state", methods=["GET"])
def twin_state():
    return jsonify(CURRENT_TWIN_STATE)


# @app.route("/api/command", methods=["GET"])
# def get_command():
#     global PENDING_COMMAND

#     cmd = PENDING_COMMAND.strip().upper() if PENDING_COMMAND else "NONE"

#     if cmd not in ("ACK", "SNOOZE", "DISPENSE", "TAKEN", "NONE"):
#         cmd = "NONE"

#     PENDING_COMMAND = "NONE"

#     return Response(cmd + "\n", mimetype="text/plain")




@app.route("/api/command", methods=["GET"])
def get_command():
    global PENDING_COMMAND

    cmd = PENDING_COMMAND.strip().upper() if PENDING_COMMAND else "NONE"

    # Allow normal commands + TWIN_WEIGHT
    if not (
        cmd in ("ACK", "SNOOZE", "DISPENSE", "NONE", "TAKEN")
        or cmd.startswith("TWIN_WEIGHT:")
    ):
        cmd = "NONE"

    PENDING_COMMAND = "NONE"

    return Response(cmd + "\n", mimetype="text/plain")




# @app.route("/api/command", methods=["POST"])
# def set_command():
#     global PENDING_COMMAND, CURRENT_TWIN_STATE

#     data = request.get_json(force=True, silent=True) or {}
#     cmd = str(data.get("command", "NONE")).strip().upper()

#     if cmd not in ("ACK", "SNOOZE", "DISPENSE", "TAKEN", "NONE"):
#         return jsonify({
#             "status": "error",
#             "message": "Invalid command. Use ACK, SNOOZE, DISPENSE, or NONE."
#         }), 400

#     PENDING_COMMAND = cmd

#     CURRENT_TWIN_STATE["last_command"] = cmd
#     CURRENT_TWIN_STATE["last_command_result"] = "PENDING"
#     CURRENT_TWIN_STATE["last_command_time"] =  datetime.now(ZoneInfo("America/Toronto")).isoformat(timespec="seconds")

#     return jsonify({
#         "status": "ok",
#         "pending_command": PENDING_COMMAND
#     }), 200





@app.route("/api/command", methods=["POST"])
def set_command():
    global PENDING_COMMAND, CURRENT_TWIN_STATE

    data = request.get_json(force=True, silent=True) or {}
    cmd = str(data.get("command", "NONE")).strip().upper()

    # Allow normal commands + TWIN_WEIGHT
    if not (
        cmd in ("ACK", "SNOOZE", "DISPENSE", "NONE", "TAKEN")
        or cmd.startswith("TWIN_WEIGHT:")
    ):
        return jsonify({
            "status": "error",
            "message": "Invalid command. Use ACK, SNOOZE, DISPENSE, or TWIN_WEIGHT:<value>."
        }), 400

    PENDING_COMMAND = cmd

    CURRENT_TWIN_STATE["last_command"] = cmd
    CURRENT_TWIN_STATE["last_command_result"] = "PENDING"
    CURRENT_TWIN_STATE["last_command_time"] = datetime.now(
        ZoneInfo("America/Toronto")
    ).isoformat(timespec="seconds")

    return jsonify({
        "status": "ok",
        "pending_command": PENDING_COMMAND
    }), 200





# -----------------------------
# Analytics helpers
# -----------------------------
def compute_basic_stats(events):
    total = len(events)
    taken = sum(1 for e in events if (e["status"] or "").upper() == "TAKEN")
    missed = sum(1 for e in events if (e["status"] or "").upper() == "MISSED")
    snoozed = sum(1 for e in events if (e["status"] or "").upper() == "SNOOZED")
    miss_rate = (missed / total) if total else 0.0

    miss_by_hour = [0] * 24
    total_by_hour = [0] * 24
    miss_by_dow = [0] * 7
    total_by_dow = [0] * 7

    sum_delay = 0
    sum_rem = 0
    c_delay = 0
    c_rem = 0

    for e in events:
        dt = parse_dt(e["timestamp"])
        if not dt:
            continue

        h = dt.hour
        d = dt.weekday()

        if 0 <= h <= 23:
            total_by_hour[h] += 1
        if 0 <= d <= 6:
            total_by_dow[d] += 1

        if (e["status"] or "").upper() == "MISSED":
            if 0 <= h <= 23:
                miss_by_hour[h] += 1
            if 0 <= d <= 6:
                miss_by_dow[d] += 1

        try:
            sum_delay += int(e["delay_minutes"])
            c_delay += 1
        except Exception:
            pass

        try:
            sum_rem += int(e["reminder_count"])
            c_rem += 1
        except Exception:
            pass

    avg_delay = (sum_delay / c_delay) if c_delay else 0.0
    avg_rem = (sum_rem / c_rem) if c_rem else 0.0

    return {
        "total": total,
        "taken": taken,
        "missed": missed,
        "snoozed": snoozed,
        "miss_rate": miss_rate,
        "miss_by_hour": miss_by_hour,
        "total_by_hour": total_by_hour,
        "miss_by_dow": miss_by_dow,
        "total_by_dow": total_by_dow,
        "avg_delay_minutes": avg_delay,
        "avg_reminders": avg_rem
    }


def fetch_events_for_training(med=None):
    if med:
        rows = db_query(
            "SELECT timestamp, med_id, status, reminder_count, delay_minutes FROM events WHERE med_id=? ORDER BY id ASC",
            (med,)
        )
    else:
        rows = db_query(
            "SELECT timestamp, med_id, status, reminder_count, delay_minutes FROM events ORDER BY id ASC"
        )

    events = [{
        "timestamp": r[0],
        "med_id": r[1],
        "status": r[2],
        "reminder_count": r[3],
        "delay_minutes": r[4]
    } for r in rows]

    return events


def get_typical_schedule_for_med(events):
    hm_counts = {}

    for e in events:
        dt = parse_dt(e["timestamp"])
        if dt:
            key = (dt.hour, dt.minute)
            hm_counts[key] = hm_counts.get(key, 0) + 1

    if not hm_counts:
        now = datetime.now()
        return now.hour, 0

    return max(hm_counts.items(), key=lambda kv: kv[1])[0]


def get_next_scheduled_datetime(events):
    hour, minute = get_typical_schedule_for_med(events)
    now = datetime.now()

    target_dt = now.replace(hour=hour, minute=minute, second=0, microsecond=0)
    if target_dt <= now:
        target_dt += timedelta(days=1)

    return target_dt


def compute_recent_behavior(events, recent_n=5):
    if not events:
        return {
            "recent_miss_rate": 0.0,
            "recent_snooze_rate": 0.0,
            "recent_taken_rate": 0.0,
            "recent_problem_rate": 0.0
        }

    recent = events[-recent_n:]
    n = len(recent)

    missed = sum(1 for e in recent if (e["status"] or "").upper() == "MISSED")
    snoozed = sum(1 for e in recent if (e["status"] or "").upper() == "SNOOZED")
    taken = sum(1 for e in recent if (e["status"] or "").upper() == "TAKEN")

    return {
        "recent_miss_rate": missed / n if n else 0.0,
        "recent_snooze_rate": snoozed / n if n else 0.0,
        "recent_taken_rate": taken / n if n else 0.0,
        "recent_problem_rate": (missed + snoozed) / n if n else 0.0
    }


def compute_streak_features(events):
    result = {
        "taken_streak": 0,
        "problem_streak": 0,
        "last_was_taken": 0.0,
        "last_was_missed": 0.0,
        "last_was_snoozed": 0.0
    }

    if not events:
        return result

    last_status = (events[-1]["status"] or "").upper()
    if last_status == "TAKEN":
        result["last_was_taken"] = 1.0
    elif last_status == "MISSED":
        result["last_was_missed"] = 1.0
    elif last_status == "SNOOZED":
        result["last_was_snoozed"] = 1.0

    for e in reversed(events):
        status = (e["status"] or "").upper()

        if status == "TAKEN":
            if result["problem_streak"] == 0:
                result["taken_streak"] += 1
            else:
                break
        elif status in ("MISSED", "SNOOZED"):
            if result["taken_streak"] == 0:
                result["problem_streak"] += 1
            else:
                break
        else:
            break

    return result


def compute_prediction_for_med(med_id: str):
    events = fetch_events_for_training(med=med_id)
    basic = compute_basic_stats(events)
    recent = compute_recent_behavior(events, recent_n=5)
    streak = compute_streak_features(events)
    profile = get_med_profile(med_id)
    profile_signal = compute_profile_risk_signal(profile)

    next_dt = get_next_scheduled_datetime(events)
    target_iso = next_dt.isoformat(timespec="seconds")

    avg_reminders = safe_int(round(basic["avg_reminders"]), 0)
    avg_delay = safe_int(round(basic["avg_delay_minutes"]), 0)

    history_events = events[-WINDOW_SIZE:] if events else []

    if PRETRAINED_MODEL is not None:
        base_probability = predict_with_sequence_model(
            PRETRAINED_MODEL,
            history_events=history_events,
            target_dt=next_dt
        )
        model_name = "sequence_logreg"
    else:
        target_hour = next_dt.hour
        ht = basic["total_by_hour"][target_hour]
        hm = basic["miss_by_hour"][target_hour]
        base_probability = (hm / ht) if ht else basic["miss_rate"]
        model_name = "fallback"

    base_probability = clamp(base_probability)

    taken_streak_norm = clamp(streak["taken_streak"] / 3.0, 0.0, 1.0)
    problem_streak_norm = clamp(streak["problem_streak"] / 3.0, 0.0, 1.0)

    recent_n_used = min(len(events), 5)
    recent_confidence = clamp(recent_n_used / 5.0, 0.0, 1.0)
    short_term_weight = 0.45 + 0.55 * recent_confidence

    adjusted_probability = base_probability

    adjusted_probability += short_term_weight * 0.10 * recent["recent_miss_rate"]
    adjusted_probability += short_term_weight * 0.05 * recent["recent_snooze_rate"]
    adjusted_probability += short_term_weight * 0.05 * recent["recent_problem_rate"]

    adjusted_probability += short_term_weight * 0.04 * streak["last_was_missed"]
    adjusted_probability += short_term_weight * 0.02 * streak["last_was_snoozed"]
    adjusted_probability -= short_term_weight * 0.03 * streak["last_was_taken"]

    adjusted_probability += short_term_weight * 0.05 * problem_streak_norm
    adjusted_probability -= short_term_weight * 0.04 * taken_streak_norm

    adjusted_probability += 0.03 * clamp(avg_reminders / 3.0, 0.0, 1.0)
    adjusted_probability += 0.03 * clamp(avg_delay / 10.0, 0.0, 1.0)

    adjusted_probability += PROFILE_RISK_WEIGHT * profile_signal["profile_net"]

    adjusted_probability = min(adjusted_probability, base_probability + MAX_ADAPTIVE_UPLIFT)
    adjusted_probability = max(adjusted_probability, base_probability - MAX_ADAPTIVE_DROP)
    adjusted_probability = clamp(adjusted_probability)

    high_risk = adjusted_probability >= HIGH_RISK_THRESHOLD
    lead_sec = select_lead_sec(high_risk)

    if profile is None:
        profile = default_med_profile(med_id)
    profile["last_risk"] = adjusted_probability
    profile["updated_at"] = datetime.now().isoformat(timespec="seconds")
    upsert_med_profile(profile)


    # print(f"[AI DEBUG] med={med_id}")
    # print(f"[AI DEBUG] base_probability={base_probability:.3f}")
    # print(f"[AI DEBUG] adjusted_probability={adjusted_probability:.3f}")
    # print(f"[AI DEBUG] high_risk={high_risk}, lead_sec={lead_sec}, label={risk_label(adjusted_probability)}")

    return {
        "med_id": med_id,
        "miss_probability": round(adjusted_probability, 3),
        "base_model_probability": round(base_probability, 3),
        "risk_label": risk_label(adjusted_probability),
        "high_risk": high_risk,
        "lead_sec": lead_sec,
        "hour": next_dt.hour,
        "minute": next_dt.minute,
        "next_scheduled_time": target_iso,
        "avg_reminders_used": avg_reminders,
        "avg_delay_used": avg_delay,
        "recent_miss_rate": round(recent["recent_miss_rate"], 3),
        "recent_snooze_rate": round(recent["recent_snooze_rate"], 3),
        "recent_problem_rate": round(recent["recent_problem_rate"], 3),
        "taken_streak": streak["taken_streak"],
        "problem_streak": streak["problem_streak"],
        "last_was_taken": streak["last_was_taken"],
        "last_was_missed": streak["last_was_missed"],
        "last_was_snoozed": streak["last_was_snoozed"],
        "profile_ew_miss_rate": round(profile["ew_miss_rate"], 3),
        "profile_ew_snooze_rate": round(profile["ew_snooze_rate"], 3),
        "profile_ew_taken_rate": round(profile["ew_taken_rate"], 3),
        "profile_ew_delay": round(profile["ew_delay"], 3),
        "profile_ew_reminders": round(profile["ew_reminders"], 3),
        "profile_volatility": round(profile["adherence_volatility"], 3),
        "profile_risk_signal": profile_signal["profile_risk"],
        "profile_protective_signal": profile_signal["profile_protective"],
        "profile_net_signal": profile_signal["profile_net"],
        "recent_confidence": round(recent_confidence, 3),
        "short_term_weight": round(short_term_weight, 3),
        "history_window_used": len(history_events),
        "model": model_name
    }


def compute_all_predictions():
    meds = [r[0] for r in db_query("SELECT DISTINCT med_id FROM events ORDER BY med_id ASC")]
    return [compute_prediction_for_med(m) for m in meds]


# -----------------------------
# Analytics
# -----------------------------
@app.route("/api/analytics", methods=["GET"])
def analytics():
    med = request.args.get("med", default=None, type=str)

    if med:
        pred = compute_prediction_for_med(med)
        events = fetch_events_for_training(med=med)
        basic = compute_basic_stats(events)
        per_med_summary = []
        risk = {
            "hour": pred["hour"],
            "risk": pred["miss_probability"],
            "label": pred["risk_label"],
            "model": pred["model"],
            "profile_net_signal": pred["profile_net_signal"]
        }
    else:
        events = fetch_events_for_training(med=None)
        basic = compute_basic_stats(events)

        preds = compute_all_predictions()
        if preds:
            top_pred = max(preds, key=lambda p: p["miss_probability"])
            risk = {
                "hour": top_pred["hour"],
                "risk": top_pred["miss_probability"],
                "label": top_pred["risk_label"],
                "model": top_pred["model"],
                "profile_net_signal": top_pred["profile_net_signal"]
            }
        else:
            risk = {
                "hour": (datetime.now().hour + 1) % 24,
                "risk": 0.0,
                "label": "LOW",
                "model": "fallback",
                "profile_net_signal": 0.0
            }

        per_med_summary = []
        meds = [r[0] for r in db_query("SELECT DISTINCT med_id FROM events ORDER BY med_id ASC")]
        for m in meds:
            e2 = fetch_events_for_training(med=m)
            b2 = compute_basic_stats(e2)
            p2 = compute_prediction_for_med(m)
            per_med_summary.append({
                "med_id": m,
                "total": b2["total"],
                "miss_rate": b2["miss_rate"],
                "most_missed_hour": int(max(range(24), key=lambda h: b2["miss_by_hour"][h])) if b2["total"] else None,
                "ai_risk": p2["miss_probability"],
                "ai_label": p2["risk_label"],
                "profile_net_signal": p2["profile_net_signal"]
            })

    return jsonify({
        "scope": med if med else "ALL",
        "basic": basic,
        "risk_next_hour": risk,
        "per_med_summary": per_med_summary
    })


# -----------------------------
# AI endpoints for MCU
# -----------------------------
@app.route("/api/predictions", methods=["GET"])
def get_predictions():
    med = request.args.get("med", default=None, type=str)

    if med:
        predictions = [compute_prediction_for_med(med)]
    else:
        predictions = compute_all_predictions()

    return jsonify({
        "threshold": HIGH_RISK_THRESHOLD,
        "lead_time_mode": "real" if USE_REAL_LEAD_TIME else "demo",
        "predictions": predictions
    })


@app.route("/api/ai_lines", methods=["GET"])
def get_ai_lines():
    med = request.args.get("med", default=None, type=str)

    if med:
        predictions = [compute_prediction_for_med(med)]
    else:
        predictions = compute_all_predictions()

    lines = []
    for p in predictions:
        high_risk_num = 1 if p["high_risk"] else 0
        lines.append(f"AI,{p['med_id']},{high_risk_num},{p['lead_sec']}")
    
    lines.append("AI_DONE")

    payload = "\n".join(lines) + ("\n" if lines else "")
    return Response(payload, mimetype="text/plain")



@app.route("/api/ai/boot-config", methods=["GET"])
def get_ai_boot_config():
    med = request.args.get("med", default=None, type=str)

    if med:
        predictions = [compute_prediction_for_med(med)]
    else:
        predictions = compute_all_predictions()

    lines = []
    for p in predictions:
        high_risk_num = 1 if p["high_risk"] else 0
        lines.append(f"AI,{p['med_id']},{high_risk_num},{p['lead_sec']}")

    lines.append("AI_DONE")
    return Response("\n".join(lines) + "\n", mimetype="text/plain")


# -----------------------------
# Demo polish: Export CSV + Clear logs
# -----------------------------
@app.route("/api/export.csv", methods=["GET"])
def export_csv():
    med = request.args.get("med", default=None, type=str)
    if med:
        rows = db_query(
            "SELECT id, timestamp, med_id, status, reminder_count, delay_minutes FROM events WHERE med_id=? ORDER BY id DESC",
            (med,)
        )
        filename = f"med_events_{med}.csv"
    else:
        rows = db_query(
            "SELECT id, timestamp, med_id, status, reminder_count, delay_minutes FROM events ORDER BY id DESC"
        )
        filename = "med_events_all.csv"

    output = io.StringIO()
    writer = csv.writer(output)
    writer.writerow(["id", "timestamp", "med_id", "status", "reminder_count", "delay_minutes"])
    for r in rows:
        writer.writerow(list(r))

    csv_bytes = output.getvalue().encode("utf-8")
    output.close()

    return Response(
        csv_bytes,
        mimetype="text/csv",
        headers={"Content-Disposition": f"attachment; filename={filename}"}
    )


@app.route("/api/clear", methods=["POST"])
def clear_logs():
    db_query("UPDATE events SET visible=0 WHERE visible=1", fetch=False)
    return jsonify({"message": "Dashboard logs cleared only. History preserved."}), 200


@app.route("/api/dispense_verification", methods=["GET"])
def get_dispense_verification_logs():
    limit = request.args.get("limit", default=20, type=int)
    limit = max(1, min(limit, 200))

    med = request.args.get("med", default=None, type=str)

    if med:
        rows = db_query("""
            SELECT id, timestamp, med_id, result, pre_weight_g, post_weight_g, delta_g, threshold_g
            FROM dispense_verification_logs
            WHERE med_id=?
            ORDER BY id DESC
            LIMIT ?
        """, (med, limit))
    else:
        rows = db_query("""
            SELECT id, timestamp, med_id, result, pre_weight_g, post_weight_g, delta_g, threshold_g
            FROM dispense_verification_logs
            ORDER BY id DESC
            LIMIT ?
        """, (limit,))

    logs = [{
        "id": r[0],
        "timestamp": r[1],
        "med_id": r[2],
        "result": r[3],
        "pre_weight_g": r[4],
        "post_weight_g": r[5],
        "delta_g": r[6],
        "threshold_g": r[7]
    } for r in rows]

    return jsonify(logs)



# @app.route("/api/dispense_verification", methods=["POST"])
# def receive_dispense_verification():
#     data = request.get_json(force=True, silent=True) or {}

#     timestamp = str(data.get("timestamp", datetime.now().isoformat(timespec="seconds")))
#     med_id = str(data.get("med_id", "unknown")).strip() or "unknown"
#     result = str(data.get("result", "DISPENSE_ERROR")).strip().upper()

#     pre_weight_g = safe_float(data.get("pre_weight_g", 0.0))
#     post_weight_g = safe_float(data.get("post_weight_g", 0.0))
#     delta_g = safe_float(data.get("delta_g", 0.0))
#     threshold_g = safe_float(data.get("threshold_g", 0.0))

#     if result not in ("DISPENSE_SUCCESS", "DISPENSE_FAIL", "DISPENSE_ERROR"):
#         result = "DISPENSE_ERROR"

#     db_query("""
#         INSERT INTO dispense_verification_logs
#         (timestamp, med_id, result, pre_weight_g, post_weight_g, delta_g, threshold_g)
#         VALUES (?, ?, ?, ?, ?, ?, ?)
#     """, (
#         timestamp,
#         med_id,
#         result,
#         pre_weight_g,
#         post_weight_g,
#         delta_g,
#         threshold_g
#     ), fetch=False)

#     return jsonify({
#         "message": "Dispense verification logged successfully",
#         "stored": True,
#         "dispense": {
#             "timestamp": timestamp,
#             "med_id": med_id,
#             "result": result,
#             "pre_weight_g": pre_weight_g,
#             "post_weight_g": post_weight_g,
#             "delta_g": delta_g,
#             "threshold_g": threshold_g
#         }
#     }), 201


@app.route("/api/dispense_verification", methods=["POST"])
def receive_dispense_verification():
    data = request.get_json(force=True, silent=True) or {}

    timestamp = str(data.get("timestamp", datetime.now().isoformat(timespec="seconds")))
    med_id = str(data.get("med_id", "unknown")).strip() or "unknown"
    result = str(data.get("result", "DISPENSE_ERROR")).strip().upper()

    # Support MCU integer x100 fields
    if "pre_weight_x100" in data:
        pre_weight_g = safe_int(data.get("pre_weight_x100", 0)) / 100.0
        post_weight_g = safe_int(data.get("post_weight_x100", 0)) / 100.0
        delta_g = safe_int(data.get("delta_x100", 0)) / 100.0
        threshold_g = safe_int(data.get("threshold_x100", 0)) / 100.0
    else:
        # Fallback for older float-based payloads
        pre_weight_g = safe_float(data.get("pre_weight_g", 0.0))
        post_weight_g = safe_float(data.get("post_weight_g", 0.0))
        delta_g = safe_float(data.get("delta_g", 0.0))
        threshold_g = safe_float(data.get("threshold_g", 0.0))

    if result not in ("DISPENSE_SUCCESS", "DISPENSE_FAIL", "DISPENSE_ERROR"):
        result = "DISPENSE_ERROR"

    db_query("""
        INSERT INTO dispense_verification_logs
        (timestamp, med_id, result, pre_weight_g, post_weight_g, delta_g, threshold_g)
        VALUES (?, ?, ?, ?, ?, ?, ?)
    """, (
        timestamp,
        med_id,
        result,
        pre_weight_g,
        post_weight_g,
        delta_g,
        threshold_g
    ), fetch=False)

    print(f"[DISPENSE LOGGED] med_id={med_id}, result={result}, pre={pre_weight_g}, post={post_weight_g}, delta={delta_g}, threshold={threshold_g}")

    return jsonify({
        "message": "Dispense verification logged successfully",
        "stored": True,
        "dispense": {
            "timestamp": timestamp,
            "med_id": med_id,
            "result": result,
            "pre_weight_g": pre_weight_g,
            "post_weight_g": post_weight_g,
            "delta_g": delta_g,
            "threshold_g": threshold_g
        }
    }), 201


########--------------------NON-AI (FROM LINE 1655 TO 2051)-------USED AI TO CORRECT ERRORS ONLY IN THIS PART --------------------##############
##-------fOR <SCRIPT/> TAG, I dont remeber which exact functin is Ai generated because we created almost all functions but when functionailty is not as expected then corrected by ChatGpt and Copilot---------------#############


# -----------------------------
# Dashboard
# -----------------------------
@app.route("/")
def dashboard():
    html = """
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8"/>
  <title>Medication Reminder Dashboard</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <style>
    body { font-family: Arial, sans-serif; margin: 18px; }
    .row { display: flex; gap: 12px; flex-wrap: wrap; align-items: center; }
    .card { border: 1px solid #ddd; border-radius: 12px; padding: 14px; margin-top: 12px; }
    .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
    .kpi { display: grid; grid-template-columns: repeat(4, 1fr); gap: 10px; }
    .kpi > div { border: 1px solid #eee; border-radius: 10px; padding: 10px; }
    .muted { color: #666; font-size: 13px; }
    .risk { font-weight: bold; }
    button { padding: 8px 12px; border-radius: 10px; border: 1px solid #ccc; cursor: pointer; }
    select { padding: 8px 10px; border-radius: 10px; border: 1px solid #ccc; }
    table { border-collapse: collapse; width: 100%; }
    th, td { border: 1px solid #ddd; padding: 8px; font-size: 14px; }
    th { background: #f6f6f6; }
    .small { font-size: 12px; }
    code { background: #f5f5f5; padding: 2px 4px; border-radius: 4px; }
    .twin-grid { display: grid; grid-template-columns: repeat(5, 1fr); gap: 10px; }
    .twin-grid > div { border: 1px solid #eee; border-radius: 10px; padding: 10px; }
  </style>
</head>
<body>
  <h2>Medication Event Logs (Live)</h2>
  <div class="row">
    <span class="muted">Auto-refresh every 2 seconds.</span>
    <span class="muted">Scope:</span>
    <select id="medSelect"></select>
    <button onclick="exportCsv()">Export CSV</button>
    <button onclick="clearLogs()">Clear logs</button>
    <button onclick="clearRecovered()">Clear recovered logs</button>
  </div>

  <div class="card">
    <div class="kpi">
      <div><div class="muted">Total Events</div><div id="kpi-total">0</div></div>
      <div><div class="muted">Miss Rate</div><div id="kpi-missrate">0%</div></div>
      <div><div class="muted">AI Risk</div><div id="kpi-risk" class="risk">LOW</div></div>
      <div><div class="muted">Model</div><div id="kpi-model" class="small">fallback</div></div>
    </div>
    <p class="muted">
      Avg Delay: <span id="kpi-delay">0</span> min |
      Avg Reminders: <span id="kpi-rem">0</span> |
      Profile Signal: <span id="kpi-profile">0</span>
    </p>
    <p class="muted">MCU endpoint available at <code>/api/ai_lines</code>, JSON endpoint at <code>/api/predictions</code>, and twin endpoint at <code>/api/twin/update</code></p>
  </div>

  





 <div style="margin-top:20px; padding:16px; border:1px solid #ccc; border-radius:12px; background:#f9fafb;">
  <h3 style="margin-bottom:12px;">Remote Commands</h3>

  <button class="cmd-btn ack" onclick="sendCommand('ACK')">ACK</button>
  <button class="cmd-btn snooze" onclick="sendCommand('SNOOZE')">SNOOZE</button>
<div style="margin-top:14px; padding:12px; border:1px solid #ddd; border-radius:10px; background:#fff;">
  <div style="font-weight:bold; margin-bottom:8px;">Debug / Test Controls</div>
  <button class="cmd-btn dispense" onclick="sendCommand('DISPENSE')">MANUAL DISPENSE TEST</button>
  <div class="muted" style="margin-top:8px;">
    Normal workflow auto-dispenses when reminder becomes ACTIVE. This button is only for actuator-path testing.
  </div>
</div>
  <div style="margin-top:14px; padding:12px; border:1px solid #ddd; border-radius:10px; background:#fff;">
    <div style="font-weight:bold; margin-bottom:8px;">Taken Simulation</div>

    <div class="row" style="align-items:center;">
      <span id="takenIndicator" style="font-size:20px;">💊</span>
      <span class="muted">Set tray weight to simulate pill pickup</span>
    </div>

    <div class="row" style="margin-top:10px; align-items:center;">
    <input
        id="twinWeightInput"
        type="number"
        step="0.1"
        value="0.0"
        onfocus="twinWeightUserEditing = true"
        onblur="twinWeightUserEditing = false"
        oninput="twinWeightUserEditing = true"
        style="padding:8px 10px; border-radius:8px; border:1px solid #ccc; width:120px;"
    />
      <span class="muted">grams</span>
      <button class="cmd-btn taken" onclick="sendTwinWeight()">Apply Weight</button>
    </div>

    <div class="muted" style="margin-top:8px;">
      During ACTIVE reminder, setting this near base weight simulates TAKEN.
    </div>
  </div>

  <p id="cmdStatus" style="margin-top:12px; color:#555;">No command sent yet.</p>
</div>









  <div class="card">
    <h3>Digital Twin (Live MCU State)</h3>

    <div class="twin-grid">
        <div>
        <div class="muted">Server Timestamp</div>
        <div id="twin-ts">-</div>
        </div>

        <div>
        <div class="muted">MCU Tick (ms)</div>
        <div id="twin-mcu-ts">-</div>
        </div>

        <div>
        <div class="muted">Reminder Phase</div>
        <div id="twin-rem">UNKNOWN</div>
        </div>

        <div>
        <div class="muted">AI Risk</div>
        <div id="twin-risk">LOW</div>
        </div>

        <div>
        <div class="muted">Servo Angle</div>
        <div id="twin-servo-angle">-</div>
        </div>

        <div>
        <div class="muted">Servo State</div>
        <div id="twin-servo-state">UNKNOWN</div>
        </div>

        <div>
        <div class="muted">Tray State</div>
        <div id="twin-tray-state">UNKNOWN</div>
        </div>

        <div>
        <div class="muted">Weight</div>
        <div id="twin-weight">-</div>
        </div>

        <div>
        <div class="muted">Weight Source</div>
        <div id="twin-weight-source">UNKNOWN</div>
        </div>
    </div>
  </div>


  <div class="grid">
    <div class="card">
      <h3>Misses by Hour</h3>
      <canvas id="chartHour"></canvas>
    </div>
    <div class="card">
      <h3>Misses by Day of Week</h3>
      <canvas id="chartDow"></canvas>
    </div>
  </div>

  <div class="card">
    <h3>Per-Medication Summary (when scope = ALL)</h3>
    <table>
      <thead>
        <tr>
          <th>Medication</th>
          <th>Total</th>
          <th>Miss Rate</th>
          <th>Most Missed Hour</th>
          <th>AI Risk</th>
          <th>AI Label</th>
          <th>Profile Signal</th>
        </tr>
      </thead>
      <tbody id="medSummaryBody"></tbody>
    </table>
  </div>

  <div class="card">
    <h3>Latest Events</h3>
    <table>
      <thead>
        <tr>
          <th>ID</th>
          <th>Timestamp</th>
          <th>Medication</th>
          <th>Status</th>
          <th>Reminders</th>
          <th>Delay (min)</th>
        </tr>
      </thead>
      <tbody id="eventsBody"></tbody>
    </table>
  </div>

    <div class="card">
    <h3>Dispense Verification Logs</h3>
    <table>
      <thead>
        <tr>
          <th>ID</th>
          <th>Timestamp</th>
          <th>Medication</th>
          <th>Result</th>
          <th>Pre Weight (g)</th>
          <th>Post Weight (g)</th>
          <th>Delta (g)</th>
          <th>Threshold (g)</th>
        </tr>
      </thead>
      <tbody id="dispenseBody"></tbody>
    </table>
  </div>

  
<div class="card" style="margin-bottom: 15px;">
  <h3>Current AI Weights</h3>
  <table>
    <thead>
      <tr>
        <th>Medication</th>
        <th>W Missed</th>
        <th>W Reminder</th>
        <th>W Failure</th>
        <th>W Ack</th>
        <th> Updated</th>
      </tr>
    </thead>
    <tbody id="weightsBody"></tbody>
  </table>
</div>


  <div class="card">
    <h3>Recovered Onboard Flash Logs</h3>
    <table>
      <thead>
        <tr>
          <th>ID</th>
          <th>Device TS</th>
          <th>SEQ</th>
          <th>Type</th>
          <th>Medication</th>
          <th>Source</th>
          <th>Status</th>
          <th>Rem</th>
          <th>Delay</th>
          <th>Val</th>
          <th>Extra</th>
          <th>Extra2</th>
          <th>Flags/ Risk</th>
          <th>CRC</th>
        </tr>
      </thead>
      <tbody id="recoveredBody"></tbody>
    </table>
  </div>


  <div class="card">
    <h3>Recovered Flash Risk Trend</h3>
    <canvas id="chartRecoveredRisk"></canvas>
  </div>


  <div class="card">
    <h3>Latest Recovered Flash Risk per Medication</h3>
    <canvas id="chartRecoveredRiskBar"></canvas>
  </div>

  


<style>


.state-pill {
  display: inline-block;
  padding: 4px 10px;
  border-radius: 999px;
  font-weight: bold;
  font-size: 13px;
  border: 1px solid #ddd;
}

.state-idle {
  background: #f3f4f6;
  color: #374151;
}

.state-prealert {
  background: #dbeafe;
  color: #1d4ed8;
}

.state-active {
  background: #fee2e2;
  color: #b91c1c;
}

.state-grace {
  background: #fef3c7;
  color: #b45309;
}

.state-pill-present {
  background: #ede9fe;
  color: #6d28d9;
}

.state-removed {
  background: #dcfce7;
  color: #15803d;
}

.state-empty {
  background: #f3f4f6;
  color: #374151;
}

.state-unknown {
  background: #e5e7eb;
  color: #6b7280;
}

.state-real {
  background: #dbeafe;
  color: #1d4ed8;
}

.state-override {
  background: #fce7f3;
  color: #be185d;
}

.cmd-btn {
    padding: 10px 18px;
    margin-right: 10px;
    border: none;
    border-radius: 8px;
    font-weight: bold;
    cursor: pointer;
    color: white;
    transition: all 0.2s ease;
}

/* ACK = Blue */
.cmd-btn.ack {
    background: #3b82f6;
}
.cmd-btn.ack:hover {
    background: #2563eb;
}

/* SNOOZE = Orange */
.cmd-btn.snooze {
    background: #f59e0b;
}
.cmd-btn.snooze:hover {
    background: #d97706;
}

/* DISPENSE = Green */
.cmd-btn.dispense {
    background: #10b981;
}
.cmd-btn.dispense:hover {
    background: #059669;
}

.cmd-btn.taken {
    background: #8b5cf6;
}
.cmd-btn.taken:hover {
    background: #7c3aed;
}

/* Click effect */
.cmd-btn:active {
    transform: scale(0.95);
}
</style>




<script>
let hourChart, dowChart, recoveredRiskChart, recoveredRiskBarChart;
let twinWeightUserEditing = false;
let twinWeightLastManualSetMs = 0;

function pct(x) { return (x * 100).toFixed(3) + "%"; }

function getScopeMed() {
  const v = document.getElementById("medSelect").value;
  return v === "__ALL__" ? null : v;
}

async function loadMeds() {
  const res = await fetch("/api/meds");
  const meds = await res.json();
  const sel = document.getElementById("medSelect");
  sel.innerHTML = "";

  const optAll = document.createElement("option");
  optAll.value = "__ALL__";
  optAll.textContent = "ALL";
  sel.appendChild(optAll);

  for (const m of meds) {
    const opt = document.createElement("option");
    opt.value = m;
    opt.textContent = m;
    sel.appendChild(opt);
  }

  sel.onchange = () => tick(true);
}

function buildCharts() {
  const ctxH = document.getElementById("chartHour").getContext("2d");
  const ctxD = document.getElementById("chartDow").getContext("2d");

  const ctxRR = document.getElementById("chartRecoveredRisk").getContext("2d");

recoveredRiskChart = new Chart(ctxRR, {
  type: "line",
  data: {
    labels: [],
    datasets: []
  },
  options: {
    responsive: true,
    animation: false,
    scales: {
      x: {
        title: {
          display: true,
          text: "Timestamp"
        },
        ticks: {
          maxRotation: 45,
          minRotation: 45,
          autoSkip: true,
          maxTicksLimit: 12
        }
      },
      y: {
        min: 0,
        max: 100,
        title: {
          display: true,
          text: "Risk %"
        }
      }
    }
  }
});



    const ctxRB = document.getElementById("chartRecoveredRiskBar").getContext("2d");

        recoveredRiskBarChart = new Chart(ctxRB, {
        type: "bar",
        data: {
            labels: [],
            datasets: [{
            label: "Recovered Flash Risk %",
            data: []
            }]
        },
        options: {
            responsive: true,
            animation: false,
            scales: {
            y: {
                min: 0,
                max: 100
            }
            }
        }
        });

  hourChart = new Chart(ctxH, {
    type: "bar",
    data: {
      labels: [...Array(24).keys()].map(h => String(h).padStart(2,"0") + ":00"),
      datasets: [{ label: "Misses", data: Array(24).fill(0) }]
    },
    options: { responsive: true, animation: false }
  });

  dowChart = new Chart(ctxD, {
    type: "bar",
    data: {
      labels: ["Mon","Tue","Wed","Thu","Fri","Sat","Sun"],
      datasets: [{ label: "Misses", data: Array(7).fill(0) }]
    },
    options: { responsive: true, animation: false }
  });
}

async function refreshEvents() {
  const med = getScopeMed();
  const url = med ? `/api/events?limit=50&med=${encodeURIComponent(med)}` : "/api/events?limit=10";
  const res = await fetch(url);
  const events = await res.json();

  const body = document.getElementById("eventsBody");
  body.innerHTML = "";

  for (const e of events) {
    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td>${e.id}</td>
      <td>${e.timestamp}</td>
      <td>${e.med_id}</td>
      <td>${e.status}</td>
      <td>${e.reminder_count}</td>
      <td>${e.delay_minutes}</td>
    `;
    body.appendChild(tr);
  }
}


async function refreshDispenseVerification() {
  const med = getScopeMed();
  const url = med
    ? `/api/dispense_verification?limit=20&med=${encodeURIComponent(med)}`
    : "/api/dispense_verification?limit=10";

  const res = await fetch(url);
  const logs = await res.json();

  const body = document.getElementById("dispenseBody");
  body.innerHTML = "";

  for (const d of logs) {
    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td>${d.id}</td>
      <td>${d.timestamp}</td>
      <td>${d.med_id}</td>
      <td>${d.result}</td>
      <td>${Number(d.pre_weight_g).toFixed(2)}</td>
      <td>${Number(d.post_weight_g).toFixed(2)}</td>
      <td>${Number(d.delta_g).toFixed(2)}</td>
      <td>${Number(d.threshold_g).toFixed(2)}</td>
    `;
    body.appendChild(tr);
  }

  if (logs.length === 0) {
    const tr = document.createElement("tr");
    tr.innerHTML = `<td colspan="8" class="muted">No dispense verification logs yet.</td>`;
    body.appendChild(tr);
  }
}


async function refreshRecoveredLogs() {
  const med = getScopeMed();
  const url = med
    ? `/api/recovered_logs?limit=20&med=${encodeURIComponent(med)}`
    : "/api/recovered_logs?limit=10";

  const res = await fetch(url);
  const logs = await res.json();

  const body = document.getElementById("recoveredBody");
  body.innerHTML = "";

  function riskLabelFromPercent(p) {
    if (p >= 70) return "HIGH";
    if (p >= 40) return "MEDIUM";
    return "LOW";
  }

  for (const r of logs) {
    let remText = r.reminder_count;
    let delayText = r.delay_minutes;
    let valText = r.val;
    let extraText = r.extra;
    let extra2Text = r.extra2;
    let flagsText = r.flags;

    if ((r.event_type || "").toUpperCase() === "RISK_CYCLE") {
      const riskPercent = Number(r.flags || 0);
      const riskLevel = riskLabelFromPercent(riskPercent);

      remText = r.val;                    // reminder_count stored in val
      delayText = "-";                    // not used for risk cycle
      valText = r.val;                    // keep visible if you want
      extraText = `${r.extra} (disp_fail)`;
      extra2Text = `${r.extra2} (ack)`;
      flagsText = `${riskPercent}% (${riskLevel})`;
    }

    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td>${r.id}</td>
      <td>${r.timestamp}</td>
      <td>${r.seq}</td>
      <td>${r.event_type}</td>
      <td>${r.med_id}</td>
      <td>${r.src}</td>
      <td>${r.status}</td>
      <td>${remText}</td>
      <td>${delayText}</td>
      <td>${valText}</td>
      <td>${extraText}</td>
      <td>${extra2Text}</td>
      <td>${flagsText}</td>
      <td>${r.crc}</td>
    `;
    body.appendChild(tr);
  }

  if (logs.length === 0) {
    const tr = document.createElement("tr");
    tr.innerHTML = `<td colspan="14" class="muted">No recovered onboard flash logs yet.</td>`;
    body.appendChild(tr);
  }
}

async function refreshRiskWeights() {
  const res = await fetch("/api/risk_weights");
  const rows = await res.json();

  console.log("risk_weights rows:", rows);

  const body = document.getElementById("weightsBody");
  if (!body) return;

  body.innerHTML = "";

  for (const r of rows) {
    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td>${r.med_id ?? "-"}</td>
      <td>${r.w_missed !== null && r.w_missed !== undefined ? Number(r.w_missed).toFixed(3) : "UNKNOWN"}</td>
      <td>${r.w_reminder !== null && r.w_reminder !== undefined ? Number(r.w_reminder).toFixed(3) : "UNKNOWN"}</td>
      <td>${r.w_failure !== null && r.w_failure !== undefined ? Number(r.w_failure).toFixed(3) : "UNKNOWN"}</td>
      <td>${r.w_ack !== null && r.w_ack !== undefined ? Number(r.w_ack).toFixed(3) : "UNKNOWN"}</td>
      <td>${r.updated_at ?? "-"}</td>
    `;
    body.appendChild(tr);
  }
}

async function refreshRecoveredRiskChart() {
  const med = getScopeMed();
  const url = med
    ? `/api/recovered_risk_trend?med=${encodeURIComponent(med)}`
    : "/api/recovered_risk_trend";

  const res = await fetch(url);
  const rows = await res.json();

  if (!Array.isArray(rows) || rows.length === 0) {
    recoveredRiskChart.data.labels = [];
    recoveredRiskChart.data.datasets = [];
    recoveredRiskChart.update();
    return;
  }

  // Sort all rows by timestamp as plain text
  // (works better here because some timestamps are invalid dates like 2000-00-00 ...)
  rows.sort((a, b) => String(a.timestamp).localeCompare(String(b.timestamp)));

  // Build one global x-axis
  const labels = [...new Set(rows.map(r => r.timestamp))];

  // Find all medications present
  const medIds = [...new Set(rows.map(r => r.med_id))];

  // For each medication, align data to the full label list
  const datasets = medIds.map((medId) => {
    const valuesByTimestamp = {};

    for (const r of rows) {
      if (r.med_id === medId) {
        valuesByTimestamp[r.timestamp] = Number(r.risk_percent || 0);
      }
    }

    return {
      label: medId,
      data: labels.map(ts =>
        valuesByTimestamp.hasOwnProperty(ts) ? valuesByTimestamp[ts] : null
      ),
      fill: false,
      tension: 0.2,
      spanGaps: true
    };
  });

  recoveredRiskChart.data.labels = labels;
  recoveredRiskChart.data.datasets = datasets;
  recoveredRiskChart.update();
}




async function refreshRecoveredRiskBarChart() {
  const res = await fetch("/api/recovered_risk_latest");
  const rows = await res.json();

  const labels = rows.map(r => `${r.med_id} (${r.risk_label})`);
  const values = rows.map(r => Number(r.risk_percent || 0));

  recoveredRiskBarChart.data.labels = labels;
  recoveredRiskBarChart.data.datasets[0].data = values;
  recoveredRiskBarChart.update();
}


async function refreshAnalytics() {
  const med = getScopeMed();
  const url = med ? `/api/analytics?med=${encodeURIComponent(med)}` : "/api/analytics";
  const res = await fetch(url);
  const a = await res.json();

  const b = a.basic;
  document.getElementById("kpi-total").textContent = b.total;
  document.getElementById("kpi-missrate").textContent = pct(b.miss_rate);
  document.getElementById("kpi-delay").textContent = b.avg_delay_minutes.toFixed(3);
  document.getElementById("kpi-rem").textContent = b.avg_reminders.toFixed(3);

  const r = a.risk_next_hour;
  document.getElementById("kpi-risk").textContent =
    `${r.label} (${(r.risk * 100).toFixed(3)}%)`;
  document.getElementById("kpi-model").textContent = r.model;
  document.getElementById("kpi-profile").textContent =
    (r.profile_net_signal || 0).toFixed(3);

  hourChart.data.datasets[0].data = b.miss_by_hour;
  hourChart.update();

  dowChart.data.datasets[0].data = b.miss_by_dow;
  dowChart.update();

  const ms = document.getElementById("medSummaryBody");
  ms.innerHTML = "";
  if (!med) {
    for (const row of (a.per_med_summary || [])) {
      const tr = document.createElement("tr");
      tr.innerHTML = `
        <td>${row.med_id}</td>
        <td>${row.total}</td>
        <td>${pct(row.miss_rate)}</td>
        <td>${row.most_missed_hour === null ? "-" : String(row.most_missed_hour).padStart(2,"0") + ":00"}</td>
        <td>${(row.ai_risk * 100).toFixed(3)}%</td>
        <td>${row.ai_label}</td>
        <td>${(row.profile_net_signal || 0).toFixed(3)}</td>
      `;
      ms.appendChild(tr);
    }
  } else {
    const tr = document.createElement("tr");
    tr.innerHTML = `<td colspan="7" class="muted">Switch scope to ALL to see per-med summary.</td>`;
    ms.appendChild(tr);
  }
}







async function refreshTwin() {
  const res = await fetch("/api/twin/state");
  const t = await res.json();

  document.getElementById("twin-ts").textContent = t.timestamp || "-";
  document.getElementById("twin-mcu-ts").textContent = t.mcu_timestamp_ms ?? "-";

  const reminderValue = t.reminder_state || (t.reminder_active ? "ACTIVE" : "IDLE");
  const reminderEl = document.getElementById("twin-rem");
  if (reminderEl) {
    reminderEl.innerHTML = `<span class="${stateBadgeClass("reminder", reminderValue)}">${reminderValue}</span>`;
  }

  const riskEl = document.getElementById("twin-risk");
  if (riskEl) {
    riskEl.textContent = t.ai_high_risk ? "HIGH" : "LOW";
  }

  const servoAngleEl = document.getElementById("twin-servo-angle");
  if (servoAngleEl) {
    servoAngleEl.textContent =
      (t.servo_state_deg === null || t.servo_state_deg === undefined)
        ? "UNKNOWN"
        : `${t.servo_state_deg} deg`;
  }

  const servoStateEl = document.getElementById("twin-servo-state");
  if (servoStateEl) {
    const servoStateText = t.servo_state || "UNKNOWN";
    servoStateEl.innerHTML =
      `<span class="state-pill state-unknown">${servoStateText}</span>`;
  }

  const trayStateEl = document.getElementById("twin-tray-state");
  if (trayStateEl) {
    const trayStateText = t.tray_state || "UNKNOWN";
    trayStateEl.innerHTML =
      `<span class="${stateBadgeClass("tray", trayStateText)}">${trayStateText}</span>`;
  }

  const weightEl = document.getElementById("twin-weight");
  if (weightEl) {
    weightEl.textContent =
      (t.weight_grams === null || t.weight_grams === undefined)
        ? "UNKNOWN"
        : `${t.weight_grams} g`;
  }

  const weightSourceEl = document.getElementById("twin-weight-source");
  if (weightSourceEl) {
    const sourceText = t.weight_source || "UNKNOWN";
    weightSourceEl.innerHTML =
      `<span class="${stateBadgeClass("source", sourceText)}">${sourceText}</span>`;
  }

  const input = document.getElementById("twinWeightInput");
  const indicator = document.getElementById("takenIndicator");

  if (indicator) {
    indicator.textContent = t.reminder_active ? "💊" : "✅";
  }

  if (!input) {
    return;
  }

  input.disabled = !t.reminder_active;

  if (t.weight_grams === null || t.weight_grams === undefined) {
    return;
  }

  const recentlyManual = (Date.now() - twinWeightLastManualSetMs) < 500;

  if (!twinWeightUserEditing && !recentlyManual) {
    input.value = Number(t.weight_grams).toFixed(1);
  }
}




function stateBadgeClass(type, value) {
  const v = (value || "UNKNOWN").toUpperCase();

  if (type === "reminder") {
    if (v === "IDLE") return "state-pill state-idle";
    if (v === "PREALERT") return "state-pill state-prealert";
    if (v === "ACTIVE") return "state-pill state-active";
    if (v === "GRACE") return "state-pill state-grace";
    return "state-pill state-unknown";
  }

  if (type === "tray") {
    if (v === "EMPTY") return "state-pill state-empty";
    if (v === "PILL_PRESENT") return "state-pill state-pill-present";
    if (v === "REMOVED") return "state-pill state-removed";
    return "state-pill state-unknown";
  }

  if (type === "source") {
    if (v === "REAL") return "state-pill state-real";
    if (v === "TWIN_OVERRIDE") return "state-pill state-override";
    return "state-pill state-unknown";
  }

  return "state-pill state-unknown";
}





async function exportCsv() {
  const med = getScopeMed();
  const url = med ? `/api/export.csv?med=${encodeURIComponent(med)}` : "/api/export.csv";
  window.location.href = url;
}


async function sendCommand(cmd) {
    try {
        const resp = await fetch('/api/command', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ command: cmd })
        });

        const data = await resp.json();

        if (resp.ok) {
            document.getElementById('cmdStatus').innerText =
                `Sent command: ${data.pending_command}`;
        } else {
            document.getElementById('cmdStatus').innerText =
                `Command failed: ${data.message || 'unknown error'}`;
        }
    } catch (err) {
        document.getElementById('cmdStatus').innerText =
            `Command error: ${err}`;
    }
}



async function sendTwinWeight() {
  const input = document.getElementById("twinWeightInput");
  const value = parseFloat(input.value);

  if (Number.isNaN(value)) {
    document.getElementById("cmdStatus").innerText = "Invalid twin weight value.";
    return;
  }

  try {
    const resp = await fetch('/api/command', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ command: `TWIN_WEIGHT:${value.toFixed(1)}` })
    });

    const data = await resp.json();

    if (resp.ok) {
      twinWeightUserEditing = false;
      twinWeightLastManualSetMs = Date.now();
      input.blur();

      document.getElementById('cmdStatus').innerText =
        `Sent command: ${data.pending_command}`;
    } else {
      document.getElementById('cmdStatus').innerText =
        data.message || "Failed to send twin weight command.";
    }
  } catch (err) {
    document.getElementById('cmdStatus').innerText =
      "Network/server error while sending twin weight.";
  }
}





async function clearLogs() {
  if (!confirm("Clear ALL logs?")) return;
  await fetch("/api/clear", { method: "POST" });
  await tick(true);
}

async function tick(forceReloadMeds=false) {
  try {
    if (forceReloadMeds) await loadMeds();
        await Promise.all([
            refreshEvents(),
            refreshAnalytics(),
            refreshTwin(),
            refreshDispenseVerification(),
            refreshRecoveredLogs(),
            refreshRecoveredRiskChart(),
            refreshRecoveredRiskBarChart(),
            refreshRiskWeights()
        ]);
  } catch (e) {
    console.log("Refresh error:", e);
  }
}


async function clearRecovered() {
  if (!confirm("Clear ALL recovered flash logs?")) return;

  await fetch("/api/clear_recovered", { method: "POST" });

  alert("Recovered logs cleared");

  // refresh UI after clearing
  await tick(true);
}

buildCharts();
loadMeds().then(() => tick());
setInterval(() => tick(false), 2000);
</script>
</body>
</html>
"""
    return render_template_string(html)


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=True)

    