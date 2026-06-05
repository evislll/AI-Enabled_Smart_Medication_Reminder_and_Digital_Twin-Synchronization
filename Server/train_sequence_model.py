import sqlite3
import os
import joblib
from datetime import datetime

import numpy as np
from sklearn.linear_model import LogisticRegression
from sklearn.model_selection import train_test_split
from sklearn.metrics import classification_report, roc_auc_score


DB_NAME = "med_logs.db"
MODEL_FILE = "med_model_sequence.pkl"
WINDOW_SIZE = 5


def parse_dt(ts: str):
    try:
        return datetime.fromisoformat(ts.replace("Z", "").replace(" ", "T"))
    except Exception:
        return None


def clamp(x, lo=0.0, hi=1.0):
    return max(lo, min(hi, x))


def status_to_code(status: str) -> float:
    s = (status or "").upper()
    if s == "TAKEN":
        return 0.0
    if s == "SNOOZED":
        return 0.5
    if s == "MISSED":
        return 1.0
    return 0.0


#################################################################################
##-------------------AI Generated from line 45 to line 138---------------------##
#################################################################################



def load_events():
    conn = sqlite3.connect(DB_NAME)
    cur = conn.cursor()
    cur.execute("""
        SELECT timestamp, med_id, status, reminder_count, delay_minutes
        FROM events
        ORDER BY med_id ASC, id ASC
    """)
    rows = cur.fetchall()
    conn.close()

    events = []
    for r in rows:
        events.append({
            "timestamp": r[0],
            "med_id": r[1],
            "status": r[2],
            "reminder_count": int(r[3]),
            "delay_minutes": int(r[4])
        })
    return events


def build_sequence_features(history, target_event):
    """
    history: list of exactly WINDOW_SIZE past events
    target_event: the event we are trying to predict
    """

    x = []

    # Temporal context of target event
    dt = parse_dt(target_event["timestamp"])
    hour = dt.hour if dt else 0
    dow = dt.weekday() if dt else 0

    x.append(hour / 23.0)
    for i in range(7):
        x.append(1.0 if dow == i else 0.0)

    # Last N statuses
    for ev in history:
        x.append(status_to_code(ev["status"]))

    # Last N reminders
    for ev in history:
        x.append(clamp(ev["reminder_count"] / 10.0, 0.0, 1.0))

    # Last N delays
    for ev in history:
        x.append(clamp(ev["delay_minutes"] / 60.0, 0.0, 1.0))

    # Rolling summary features from the same window
    miss_count = sum(1 for ev in history if (ev["status"] or "").upper() == "MISSED")
    snooze_count = sum(1 for ev in history if (ev["status"] or "").upper() == "SNOOZED")
    taken_count = sum(1 for ev in history if (ev["status"] or "").upper() == "TAKEN")

    avg_rem = sum(ev["reminder_count"] for ev in history) / len(history)
    avg_delay = sum(ev["delay_minutes"] for ev in history) / len(history)

    x.append(miss_count / len(history))
    x.append(snooze_count / len(history))
    x.append(taken_count / len(history))
    x.append(clamp(avg_rem / 10.0, 0.0, 1.0))
    x.append(clamp(avg_delay / 60.0, 0.0, 1.0))

    return x


def build_dataset(events):
    # group by medication
    grouped = {}
    for ev in events:
        grouped.setdefault(ev["med_id"], []).append(ev)

    X = []
    y = []

    for med_id, med_events in grouped.items():
        if len(med_events) <= WINDOW_SIZE:
            continue

        for i in range(WINDOW_SIZE, len(med_events)):
            history = med_events[i - WINDOW_SIZE:i]
            target = med_events[i]

            x = build_sequence_features(history, target)
            label = 1 if (target["status"] or "").upper() == "MISSED" else 0

            X.append(x)
            y.append(label)

    return np.array(X, dtype=float), np.array(y, dtype=int)


def main():
    if not os.path.exists(DB_NAME):
        print(f"Database not found: {DB_NAME}")
        return

    events = load_events()
    X, y = build_dataset(events)

    if len(X) < 50:
        print("Not enough sequence samples to train a useful model.")
        return

    print("Dataset shape:", X.shape)
    print("Positive rate:", y.mean())

    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=0.2, random_state=42, stratify=y
    )

    model = LogisticRegression(max_iter=2000)
    model.fit(X_train, y_train)

    y_pred = model.predict(X_test)
    y_prob = model.predict_proba(X_test)[:, 1]

    print("\nClassification Report:")
    print(classification_report(y_test, y_pred, digits=4))

    try:
        auc = roc_auc_score(y_test, y_prob)
        print(f"ROC-AUC: {auc:.4f}")
    except Exception as e:
        print("ROC-AUC could not be computed:", e)

    joblib.dump(model, MODEL_FILE)
    print(f"Saved sequence model to {MODEL_FILE}")


if __name__ == "__main__":
    main()