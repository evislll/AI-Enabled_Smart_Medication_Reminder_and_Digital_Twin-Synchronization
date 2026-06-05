import serial
import requests
import time
from datetime import datetime

print("BRIDGE VERSION: EVENTS_TWIN_ONLY")

COM_PORT = "COM5"
BAUD_RATE = 115200

FLASK_EVENTS_URL = "http://127.0.0.1:5000/api/events"
FLASK_TWIN_URL = "http://127.0.0.1:5000/api/twin/update"

# Optional behavior
AUTO_ACK_ON_ACTIVE = False   # True = send CMD,ACK when reminder becomes active
SERIAL_TIMEOUT = 0.2

# Track reminder state transitions
was_active = False


# ------------------------------------------------------------
# TWIN parsing
# ------------------------------------------------------------
def parse_twin_line(line: str):
    """
    Parse:
      TWIN,<timestamp_ms>,<reminder_active>,<ai_high_risk>,<servo_state_deg>,<weight_grams>

    Example:
      TWIN,62605,0,0,0,-1
    """
    if not line.startswith("TWIN,"):
        return None

    parts = line.strip().split(",")
    if len(parts) != 6:
        return None

    try:
        return {
            "timestamp_ms": int(parts[1]),
            "reminder_active": int(parts[2]),
            "ai_high_risk": int(parts[3]),
            "servo_state_deg": int(parts[4]),
            "weight_grams": int(parts[5]),
        }
    except Exception:
        return None


def post_twin_to_flask(twin_data: dict):
    try:
        requests.post(FLASK_TWIN_URL, json=twin_data, timeout=3)
    except Exception as e:
        print("POST TWIN error:", e)


def pretty_print_twin(twin_data: dict, raw_line: str):
    reminder_text = "ACTIVE" if int(twin_data["reminder_active"]) == 1 else "IDLE"
    risk_text = "HIGH" if int(twin_data["ai_high_risk"]) == 1 else "LOW"

    servo = twin_data["servo_state_deg"]
    weight = twin_data["weight_grams"]

    servo_text = "UNKNOWN" if servo < 0 else str(servo)
    weight_text = "UNKNOWN" if weight < 0 else str(weight)

    # Keep these commented unless you want noisy console output
    # print("--------------------------------------------------")
    # print("TWIN PARSED:")
    # print(f"  Reminder: {reminder_text}")
    # print(f"  AI Risk:  {risk_text}")
    # print(f"  Servo:    {servo_text}")
    # print(f"  Weight:   {weight_text}")
    # print(f"  RAW:      {raw_line}")
    # print("--------------------------------------------------")


# ------------------------------------------------------------
# LOG line parsing -> final dose event payload
# ------------------------------------------------------------


#################################################################################
##-------------------AI Generated from line 90 to line 167---------------------##
#################################################################################


def parse_kv_log_line(line: str):
    """
    Parse MCU line like:
    LOG,SEQ=5,TS=0000-00-00 13:48:05,TYPE=DOSE_EVENT,MED=MedB,STATUS=TAKEN,REM=1,DELAY=0,VAL=0,EXTRA=0
    """
    if not line.startswith("LOG,"):
        return None

    try:
        parts = line.strip().split(",")
        data = {}

        for part in parts[1:]:
            if "=" not in part:
                continue
            k, v = part.split("=", 1)
            data[k.strip()] = v.strip()

        return data
    except Exception:
        return None


def log_to_event_payload(log_data: dict):
    """
    Convert parsed LOG line into Flask /api/events payload
    ONLY for final medication outcome logs.
    """
    if not log_data:
        return None

    event_type = log_data.get("TYPE", "").upper()
    if event_type != "DOSE_EVENT":
        return None

    status = log_data.get("STATUS", "").upper()
    if status not in ("TAKEN", "MISSED", "SNOOZED"):
        return None

    ts = log_data.get("TS", "").strip()
    med_id = log_data.get("MED", "").strip() or "unknown"

    try:
        reminder_count = int(log_data.get("REM", "0"))
    except Exception:
        reminder_count = 0

    try:
        delay_minutes = int(log_data.get("DELAY", "0"))
    except Exception:
        delay_minutes = 0

    # If MCU timestamp has fake date 0000-00-00, replace with PC timestamp
    if ts.startswith("0000-00-00") or not ts:
        timestamp = datetime.now().isoformat(timespec="seconds")
    else:
        try:
            timestamp = datetime.fromisoformat(
                ts.replace(" ", "T")
            ).isoformat(timespec="seconds")
        except Exception:
            timestamp = datetime.now().isoformat(timespec="seconds")

    return {
        "timestamp": timestamp,
        "med_id": med_id,
        "status": status,
        "reminder_count": reminder_count,
        "delay_minutes": delay_minutes,
    }


def post_event_to_flask(event: dict):
    try:
        requests.post(FLASK_EVENTS_URL, json=event, timeout=3)
    except Exception as e:
        print("POST EVENT error:", e)


# ------------------------------------------------------------
# Optional serial command helper
# ------------------------------------------------------------
def send_serial_command(ser, cmd: str):
    try:
        payload = cmd.strip() + "\r\n"
        ser.write(payload.encode("utf-8"))
        ser.flush()
        print("TX:", cmd)
    except Exception as e:
        print("Serial TX error:", e)


# ------------------------------------------------------------
# Main bridge loop
# ------------------------------------------------------------
def main():
    global was_active

    print(f"Opening serial port {COM_PORT} @ {BAUD_RATE}...")
    ser = serial.Serial(COM_PORT, BAUD_RATE, timeout=SERIAL_TIMEOUT)
    time.sleep(2.0)
    print("Serial opened.")

    while True:
        try:
            raw = ser.readline()
            if not raw:
                continue

            try:
                line = raw.decode("utf-8", errors="ignore").strip()
            except Exception:
                continue

            if not line:
                continue

            print("RX:", line)

            # ------------------------------------------------
            # TWIN handling
            # ------------------------------------------------
            twin_data = parse_twin_line(line)
            if twin_data is not None:
                pretty_print_twin(twin_data, line)
                post_twin_to_flask(twin_data)


#################################################################################
##-------------------AI Generated from line 223 to line 245---------------------##
#################################################################################


                # Optional test behavior:
                # send ACK automatically on rising edge of reminder_active
                is_active = int(twin_data["reminder_active"]) == 1

                if AUTO_ACK_ON_ACTIVE and is_active and not was_active:
                    send_serial_command(ser, "CMD,ACK")

                was_active = is_active
                continue

            # ------------------------------------------------
            # LOG handling
            # ------------------------------------------------
            log_data = parse_kv_log_line(line)
            if log_data is not None:
                event_payload = log_to_event_payload(log_data)
                if event_payload is not None:
                    print("POST EVENT:", event_payload)
                    post_event_to_flask(event_payload)
                continue

            # ------------------------------------------------
            # Other runtime prints can just pass through
            # ------------------------------------------------
            # Example:
            # SYS,...
            # INFO,...
            # HXRAW,...
            # DISPENSE VERIFY...
            # etc.

        except KeyboardInterrupt:
            print("Stopped by user.")
            break
        except Exception as e:
            print("Bridge loop error:", e)
            time.sleep(0.5)

    try:
        ser.close()
    except Exception:
        pass


if __name__ == "__main__":
    main()