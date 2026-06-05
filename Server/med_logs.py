#################################################################################
##-------------------Formatted and structured using ChatGpt--------------------##
##-------------------but we created its main functionality ourselves-----------##
#################################################################################



import csv
import sqlite3

CSV_FILE = "med_logs.csv"
DB_NAME = "med_logs.db"

conn = sqlite3.connect(DB_NAME)
cur = conn.cursor()
cur.execute("""
    CREATE TABLE IF NOT EXISTS events (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        timestamp TEXT NOT NULL,
        med_id TEXT NOT NULL,
        status TEXT NOT NULL,
        reminder_count INTEGER NOT NULL,
        delay_minutes INTEGER NOT NULL
    )
""")

with open(CSV_FILE, "r", encoding="utf-8") as f:
    reader = csv.DictReader(f)
    rows = [
        (
            r["timestamp"],
            r["med_id"],
            r["status"],
            int(r["reminder_count"]),
            int(r["delay_minutes"]),
        )
        for r in reader
    ]

cur.executemany("""
    INSERT INTO events (timestamp, med_id, status, reminder_count, delay_minutes)
    VALUES (?, ?, ?, ?, ?)
""", rows)

conn.commit()
conn.close()

print(f"Imported {len(rows)} rows into {DB_NAME}")
