#ifndef REMINDER_H_
#define REMINDER_H_

#include <stdint.h>
#include <stdbool.h>
#include "risk.h"

#define MED_ID_MAX_LEN  16

typedef struct {
    uint8_t hour;
    uint8_t minute;
    char med_id[MED_ID_MAX_LEN];   // alphanumeric medication ID

    // AI/adaptive reminder fields
    bool     ai_high_risk;         // true -> enable pre-alert
    uint16_t prealert_lead_sec;    // 30 for demo, 300 for real 5 min
    bool     prealert_sent_today;  // internal daily flag
    bool     reminder_sent_today;  // internal daily flag
} DoseTime;

typedef enum {
    DOSE_PENDING = 0,
    DOSE_TAKEN   = 1,
    DOSE_MISSED  = 2,
    DOSE_SNOOZED = 3
} DoseStatus;

typedef struct {
    uint8_t hour;
    uint8_t minute;
    uint8_t dow;                  // 0=Mon..6=Sun (optional)
    DoseStatus status;
    uint8_t reminder_count;
    uint16_t delay_minutes;
    char med_id[MED_ID_MAX_LEN];  // alphanumeric medication ID
} DoseEvent;

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t dow;
} RtcTime;


//--------------twin------------------//

typedef enum {
    REMINDER_PHASE_IDLE = 0,
    REMINDER_PHASE_PREALERT,
    REMINDER_PHASE_ACTIVE,
    REMINDER_PHASE_GRACE
} ReminderPhase;

ReminderPhase Reminder_GetPhase(void);
const char *Reminder_GetPhaseString(void);

//--------------twin------------------//


void Reminder_Init(void);
void Reminder_SetSchedule(const DoseTime* times, uint8_t count);

void Reminder_OnSecondTick(const RtcTime* now);
bool Reminder_IsActive(void);

/*
 * Kept for compatibility with existing button code.
 * SW2 no longer controls TAKEN logic.
 */
void Reminder_AckFromISR(void);

/* SW3 still means snooze */
void Reminder_SnoozeFromISR(void);

uint8_t Reminder_GetScheduleCount(void);

/*
 * Apply AI result for a specific medication.
 * Example:
 *   Reminder_SetAiProfile("MED_A", true, 30);
 *   Reminder_SetAiProfile("MED_B", false, 0);
 */
bool Reminder_SetAiProfile(const char* med_id, bool high_risk, uint16_t lead_sec);

/*
 * Optional helper: clear all AI flags for all meds.
 */
void Reminder_ClearAllAiProfiles(void);

bool Reminder_GetNextScheduledDose(const RtcTime* now, DoseTime* out);
bool Reminder_IsNextDoseHighRisk(void);
RiskLevel_t Reminder_GetActiveRiskLevel(void);
void Reminder_RebuildRiskFromFlash(void);

/*
 * Remote ACK from server command.
 * Stops active pre-alert/reminder without marking dose as TAKEN.
 * Returns true if something active was acknowledged, false otherwise.
 */
bool Reminder_RemoteAck(void);

/*
 * Remote SNOOZE from server command.
 * Delays an active reminder using the normal snooze behavior.
 * Returns true if an active reminder was snoozed, false otherwise.
 */
bool Reminder_RemoteSnooze(void);

bool Reminder_RemoteTaken(void);
bool Reminder_GetActiveDoseEvent(DoseEvent *out);


void Reminder_LoadAllRiskWeights(void);


RiskState_t* Reminder_GetActiveRisk(void);
const char* Reminder_GetActiveMedId(void);

#endif



