#ifndef LOGGING_H_
#define LOGGING_H_

#include <stdbool.h>
#include <stdint.h>
#include "reminder.h"
#include "FreeRTOS.h"
#include "queue.h"

#define LOG_MAX 32U


////////////////////////////////////////////////////////////////////////////////////////////////
//------------This file was started as very small file that we created ourself----------------//
//------------but then had to add lot of structs and enums and we took help from--------------//
//------------ChatGpt to rearrange them in a required order-----------------------------------//
////////////////////////////////////////////////////////////////////////////////////////////////




typedef enum
{
    LOG_TYPE_DOSE_EVENT = 0,
    LOG_TYPE_REMINDER_TRIGGERED,
    LOG_TYPE_REMINDER_ACK,
    LOG_TYPE_REMINDER_SNOOZED,
    LOG_TYPE_PREALERT_TRIGGERED,
    LOG_TYPE_DISPENSE_START,
    LOG_TYPE_DISPENSE_SUCCESS,
    LOG_TYPE_DISPENSE_FAIL,
    LOG_TYPE_RETRY_ATTEMPT,
    LOG_TYPE_WEIGHT_CHANGE,
	LOG_TYPE_RISK_CYCLE,
    LOG_TYPE_SYSTEM
} LogType;

typedef enum
{
    LOG_SRC_UNKNOWN = 0,
    LOG_SRC_REMINDER,
    LOG_SRC_DISPENSER,
    LOG_SRC_WEIGHT,
    LOG_SRC_ALERT,
    LOG_SRC_SYSTEM
} LogSource;

typedef struct __attribute__((packed))
{
    uint32_t seq;
    uint32_t timestamp;   /* packed timestamp */

    uint16_t type;        /* LogType */
    uint8_t  status;      /* DoseStatus or generic status */
    uint8_t  source;      /* LogSource */

    int16_t  value;       /* generic payload */
    int16_t  extra;       /* generic payload */
    int16_t  extra2;      /* generic payload */

    uint16_t flags;       /* optional bit flags */
    char     med_id[8];   /* small compact med name */
    uint16_t crc;         /* integrity for flash */
} LogEntry;

void Logging_Init(void);

bool Logging_AddEvent(const LogEntry *e);
uint8_t Logging_Count(void);
bool Logging_GetEvent(uint8_t index_from_newest, LogEntry *out);

float Logging_MissRateForHour(uint8_t hour);

bool Logging_LogDoseEvent(const DoseEvent *dose_event,
                          uint16_t year,
                          uint8_t month,
                          uint8_t day,
                          uint8_t hour,
                          uint8_t minute,
                          uint8_t second);

bool Logging_LogSimpleEvent(LogType type,
                            const char *med_id,
                            DoseStatus status,
                            uint8_t reminder_count,
                            uint16_t delay_minutes,
                            uint16_t year,
                            uint8_t month,
                            uint8_t day,
                            uint8_t hour,
                            uint8_t minute,
                            uint8_t second,
                            int16_t value,
                            int16_t extra);

void Logging_StreamEvent(const LogEntry *e);
void Logging_StreamSystem(const char *msg);

const char *Logging_LogTypeToString(LogType type);
const char *Logging_StatusToString(DoseStatus status);
const char *Logging_SourceToString(LogSource source);

uint8_t Logging_GetHourFromEntry(const LogEntry *e);
void Logging_GetDateTimeFromEntry(const LogEntry *e,
                                  uint16_t *year,
                                  uint8_t *month,
                                  uint8_t *day,
                                  uint8_t *hour,
                                  uint8_t *minute,
                                  uint8_t *second);
bool Logging_SubmitEvent(LogType type,
                         LogSource source,
                         const char *med_id,
                         DoseStatus status,
                         uint8_t reminder_count,
                         uint16_t delay_minutes,
                         uint16_t year,
                         uint8_t month,
                         uint8_t day,
                         uint8_t hour,
                         uint8_t minute,
                         uint8_t second,
                         int16_t value,
                         int16_t extra,
                         int16_t extra2,
                         uint16_t flags);

bool Logging_TaskInit(void);
bool Logging_IsReady(void);


#endif /* LOGGING_H_ */



