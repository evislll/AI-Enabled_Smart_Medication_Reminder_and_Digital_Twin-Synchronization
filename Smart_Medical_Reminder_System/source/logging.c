#include "logging.h"
#include "fsl_debug_console.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "flash_logger.h"

#include <string.h>



#define LOGGING_QUEUE_LENGTH      16U
#define LOGGING_TASK_STACK        1024U
#define LOGGING_TASK_PRIORITY     (tskIDLE_PRIORITY + 1)



static LogEntry g_log[LOG_MAX];
static uint8_t g_head = 0U;
static uint8_t g_count = 0U;
static uint32_t g_seq_counter = 1U;
static QueueHandle_t g_loggingQueue = NULL;
static bool g_loggingTaskReady = false;

/*
 * Packed timestamp layout (32-bit):
 * bits 31..26 = year offset from 2000 (0..63 => 2000..2063)
 * bits 25..22 = month (1..12)
 * bits 21..17 = day (1..31)
 * bits 16..12 = hour (0..23)
 * bits 11..6  = minute (0..59)
 * bits 5..0   = second (0..59)
 */


///////////////////////////////////////////////////////////////////////////////////////////////
//-------------Took help from ChatGpt to create functions from line (42 - 171)---------------//
///////////////////////////////////////////////////////////////////////////////////////////////




static uint32_t Logging_PackTimestamp(uint16_t year,
                                      uint8_t month,
                                      uint8_t day,
                                      uint8_t hour,
                                      uint8_t minute,
                                      uint8_t second)
{
    uint32_t y;

    if (year < 2000U)
    {
        y = 0U;
    }
    else
    {
        y = (uint32_t)(year - 2000U);
        if (y > 63U)
        {
            y = 63U;
        }
    }

    return ((y & 0x3FU) << 26) |
           (((uint32_t)month  & 0x0FU) << 22) |
           (((uint32_t)day    & 0x1FU) << 17) |
           (((uint32_t)hour   & 0x1FU) << 12) |
           (((uint32_t)minute & 0x3FU) <<  6) |
           (((uint32_t)second & 0x3FU) <<  0);
}

static void Logging_UnpackTimestamp(uint32_t ts,
                                    uint16_t *year,
                                    uint8_t *month,
                                    uint8_t *day,
                                    uint8_t *hour,
                                    uint8_t *minute,
                                    uint8_t *second)
{
    if (year != NULL)
    {
        *year = (uint16_t)(2000U + ((ts >> 26) & 0x3FU));
    }

    if (month != NULL)
    {
        *month = (uint8_t)((ts >> 22) & 0x0FU);
    }

    if (day != NULL)
    {
        *day = (uint8_t)((ts >> 17) & 0x1FU);
    }

    if (hour != NULL)
    {
        *hour = (uint8_t)((ts >> 12) & 0x1FU);
    }

    if (minute != NULL)
    {
        *minute = (uint8_t)((ts >> 6) & 0x3FU);
    }

    if (second != NULL)
    {
        *second = (uint8_t)(ts & 0x3FU);
    }
}

static uint8_t Logging_GetHourFromTimestamp(uint32_t ts)
{
    return (uint8_t)((ts >> 12) & 0x1FU);
}

static uint16_t Logging_CalcCRC(const LogEntry *e)
{
    const uint8_t *data = (const uint8_t *)e;
    uint16_t sum = 0U;
    uint32_t i;

    if (e == NULL)
    {
        return 0U;
    }

    for (i = 0U; i < (sizeof(LogEntry) - sizeof(e->crc)); i++)
    {
        sum = (uint16_t)(sum + data[i]);
    }

    return sum;
}




static void Logging_Task(void *pvParameters)
{
    LogEntry entry;

    (void)pvParameters;

    PRINTF("SYS,LOG_TASK_START\r\n");

    for (;;)
    {
        if (xQueueReceive(g_loggingQueue, &entry, portMAX_DELAY) == pdPASS)
        {
            if (Logging_AddEvent(&entry))
            {
                LogEntry stored;

                if (Logging_GetEvent(0, &stored))
                {
                    Logging_StreamEvent(&stored);

                    if (!FlashLogger_Append(&stored))
                    {
                        PRINTF("SYS,FLASH_APPEND_FAIL\r\n");
                    }
                }
            }
            else
            {
                PRINTF("SYS,LOG_ADD_FAIL\r\n");
            }
        }
    }
}



const char *Logging_LogTypeToString(LogType type)
{
    switch (type)
    {
        case LOG_TYPE_DOSE_EVENT:         return "DOSE_EVENT";
        case LOG_TYPE_REMINDER_TRIGGERED: return "REMINDER_TRIGGERED";
        case LOG_TYPE_REMINDER_ACK:       return "REMINDER_ACK";
        case LOG_TYPE_REMINDER_SNOOZED:   return "REMINDER_SNOOZED";
        case LOG_TYPE_PREALERT_TRIGGERED: return "PREALERT_TRIGGERED";
        case LOG_TYPE_DISPENSE_START:     return "DISPENSE_START";
        case LOG_TYPE_DISPENSE_SUCCESS:   return "DISPENSE_SUCCESS";
        case LOG_TYPE_DISPENSE_FAIL:      return "DISPENSE_FAIL";
        case LOG_TYPE_RETRY_ATTEMPT:      return "RETRY_ATTEMPT";
        case LOG_TYPE_WEIGHT_CHANGE:      return "WEIGHT_CHANGE";
        case LOG_TYPE_RISK_CYCLE:         return "RISK_CYCLE";
        case LOG_TYPE_SYSTEM:             return "SYSTEM";
        default:                          return "UNKNOWN";
    }
}

const char *Logging_StatusToString(DoseStatus status)
{
    switch (status)
    {
        case DOSE_PENDING: return "PENDING";
        case DOSE_TAKEN:   return "TAKEN";
        case DOSE_MISSED:  return "MISSED";
        case DOSE_SNOOZED: return "SNOOZED";
        default:           return "UNKNOWN";
    }
}

const char *Logging_SourceToString(LogSource source)
{
    switch (source)
    {
        case LOG_SRC_REMINDER:  return "REMINDER";
        case LOG_SRC_DISPENSER: return "DISPENSER";
        case LOG_SRC_WEIGHT:    return "WEIGHT";
        case LOG_SRC_ALERT:     return "ALERT";
        case LOG_SRC_SYSTEM:    return "SYSTEM";
        default:                return "UNKNOWN";
    }
}



bool Logging_TaskInit(void)
{
    BaseType_t task_ok;

    if (g_loggingTaskReady)
    {
        return true;
    }

    g_loggingQueue = xQueueCreate(LOGGING_QUEUE_LENGTH, sizeof(LogEntry));
    if (g_loggingQueue == NULL)
    {
        PRINTF("SYS,LOG_QUEUE_CREATE_FAIL\r\n");
        return false;
    }

    task_ok = xTaskCreate(Logging_Task,
                          "log_task",
                          LOGGING_TASK_STACK,
                          NULL,
                          LOGGING_TASK_PRIORITY,
                          NULL);

    if (task_ok != pdPASS)
    {
        PRINTF("SYS,LOG_TASK_CREATE_FAIL\r\n");
        return false;
    }

    g_loggingTaskReady = true;
    PRINTF("SYS,LOG_TASK_INIT_OK\r\n");
    return true;
}



bool Logging_IsReady(void)
{
    return g_loggingTaskReady;
}




void Logging_Init(void)
{
    memset(g_log, 0, sizeof(g_log));
    g_head = 0U;
    g_count = 0U;
    g_seq_counter = 1U;

    g_loggingQueue = NULL;
    g_loggingTaskReady = false;

    PRINTF("SYS,LOG_INIT,size=%u\r\n", (unsigned int)sizeof(LogEntry));
}



///////////////////////////////////////////////////////////////////////////////////
//------------------cretaed Logging_AddEvent wit ChatGpt-------------------------//
///////////////////////////////////////////////////////////////////////////////////



bool Logging_AddEvent(const LogEntry *e)
{
    LogEntry temp;

    if (e == NULL)
    {
        return false;
    }

    temp = *e;
    temp.crc = 0U;
    temp.crc = Logging_CalcCRC(&temp);

    g_log[g_head] = temp;
    g_head = (uint8_t)((g_head + 1U) % LOG_MAX);

    if (g_count < LOG_MAX)
    {
        g_count++;
    }

    return true;
}




uint8_t Logging_Count(void)
{
    return g_count;
}




bool Logging_GetEvent(uint8_t index_from_newest, LogEntry *out)
{
    int idx;

    if (out == NULL)
    {
        return false;
    }

    if (index_from_newest >= g_count)
    {
        return false;
    }

    idx = (int)g_head - 1 - (int)index_from_newest;

    while (idx < 0)
    {
        idx += LOG_MAX;
    }

    idx %= LOG_MAX;
    *out = g_log[idx];
    return true;
}



float Logging_MissRateForHour(uint8_t hour)
{
    uint16_t total = 0U;
    uint16_t missed = 0U;
    uint8_t i;

    for (i = 0U; i < g_count; i++)
    {
        LogEntry e;

        if (Logging_GetEvent(i, &e))
        {
            /*
             * For DOSE_EVENT we store scheduled hour in value
             * so ML logic can still use it later.
             */
            if ((e.type == LOG_TYPE_DOSE_EVENT) && ((uint8_t)e.value == hour))
            {
                total++;

                if ((DoseStatus)e.status == DOSE_MISSED)
                {
                    missed++;
                }
            }
        }
    }

    if (total == 0U)
    {
        return 0.0f;
    }

    return (float)missed / (float)total;
}


bool Logging_LogDoseEvent(const DoseEvent *dose_event,
                          uint16_t year,
                          uint8_t month,
                          uint8_t day,
                          uint8_t hour,
                          uint8_t minute,
                          uint8_t second)
{
    if (dose_event == NULL)
    {
        return false;
    }

    return Logging_SubmitEvent(
        LOG_TYPE_DOSE_EVENT,
        LOG_SRC_REMINDER,
        dose_event->med_id,
        dose_event->status,
        dose_event->reminder_count,
        dose_event->delay_minutes,
        year,
        month,
        day,
        hour,
        minute,
        second,
        (int16_t)dose_event->hour,             /* value  */
        (int16_t)dose_event->reminder_count,   /* extra  */
        (int16_t)dose_event->delay_minutes,    /* extra2 */
        (uint16_t)(dose_event->dow & 0x0007U)  /* flags  */
    );
}




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
                            int16_t extra)
{
    LogSource source;

    switch (type)
    {
        case LOG_TYPE_REMINDER_TRIGGERED:
        case LOG_TYPE_REMINDER_ACK:
        case LOG_TYPE_REMINDER_SNOOZED:
        case LOG_TYPE_PREALERT_TRIGGERED:
            source = LOG_SRC_REMINDER;
            break;

        case LOG_TYPE_DISPENSE_START:
        case LOG_TYPE_DISPENSE_SUCCESS:
        case LOG_TYPE_DISPENSE_FAIL:
        case LOG_TYPE_RETRY_ATTEMPT:
            source = LOG_SRC_DISPENSER;
            break;

        case LOG_TYPE_WEIGHT_CHANGE:
            source = LOG_SRC_WEIGHT;
            break;

        case LOG_TYPE_SYSTEM:
            source = LOG_SRC_SYSTEM;
            break;

        default:
            source = LOG_SRC_UNKNOWN;
            break;
    }

    return Logging_SubmitEvent(
        type,
        source,
        med_id,
        status,
        reminder_count,
        delay_minutes,
        year,
        month,
        day,
        hour,
        minute,
        second,
        value,
        extra,
        (int16_t)delay_minutes,
        (uint16_t)reminder_count
    );
}






void Logging_StreamEvent(const LogEntry *e)
{
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    const char *med_str;
    uint16_t reminder_count = 0U;
    uint16_t delay_minutes = 0U;

    if (e == NULL)
    {
        return;
    }

    Logging_UnpackTimestamp(e->timestamp, &year, &month, &day, &hour, &minute, &second);

    med_str = (e->med_id[0] != '\0') ? e->med_id : "NA";

    /*
     * Current compact mapping:
     * - DOSE_EVENT:
     *      extra  = reminder_count
     *      extra2 = delay_minutes
     * - SIMPLE events:
     *      flags  = reminder_count
     *      extra2 = delay_minutes
     */
    if ((LogType)e->type == LOG_TYPE_DOSE_EVENT)
    {
        reminder_count = (uint16_t)e->extra;
        delay_minutes  = (uint16_t)e->extra2;
    }
    else
    {
        reminder_count = e->flags;
        delay_minutes  = (uint16_t)e->extra2;
    }

    PRINTF("LOG,SEQ=%lu,TS=%04u-%02u-%02u %02u:%02u:%02u,TYPE=%s,MED=%s,SRC=%s,STATUS=%s,REM=%u,DELAY=%u,VAL=%d,EXTRA=%d,EXTRA2=%d,FLAGS=%u,CRC=%u\r\n",
           (unsigned long)e->seq,
           (unsigned int)year,
           (unsigned int)month,
           (unsigned int)day,
           (unsigned int)hour,
           (unsigned int)minute,
           (unsigned int)second,
           Logging_LogTypeToString((LogType)e->type),
           med_str,
           Logging_SourceToString((LogSource)e->source),
           Logging_StatusToString((DoseStatus)e->status),
           (unsigned int)reminder_count,
           (unsigned int)delay_minutes,
           (int)e->value,
           (int)e->extra,
           (int)e->extra2,
           (unsigned int)e->flags,
           (unsigned int)e->crc);
}





void Logging_StreamSystem(const char *msg)
{
    if (msg == NULL)
    {
        return;
    }

    PRINTF("SYS,%s\r\n", msg);
}


uint8_t Logging_GetHourFromEntry(const LogEntry *e)
{
    if (e == NULL)
    {
        return 0U;
    }

    return (uint8_t)((e->timestamp >> 12) & 0x1FU);
}



///////////////////////////////////////////////////////////////////////////////////////////////////////
//--------------------had to figure out the way to write the conditions properly---------------------//
//--------------------but wrote the function myself--------------------------------------------------//
///////////////////////////////////////////////////////////////////////////////////////////////////////





void Logging_GetDateTimeFromEntry(const LogEntry *e,
                                  uint16_t *year,
                                  uint8_t *month,
                                  uint8_t *day,
                                  uint8_t *hour,
                                  uint8_t *minute,
                                  uint8_t *second)
{
    if (e == NULL)
    {
        if (year != NULL)   *year = 0U;
        if (month != NULL)  *month = 0U;
        if (day != NULL)    *day = 0U;
        if (hour != NULL)   *hour = 0U;
        if (minute != NULL) *minute = 0U;
        if (second != NULL) *second = 0U;
        return;
    }

    if (year != NULL)
    {
        *year = (uint16_t)(2000U + ((e->timestamp >> 26) & 0x3FU));
    }

    if (month != NULL)
    {
        *month = (uint8_t)((e->timestamp >> 22) & 0x0FU);
    }

    if (day != NULL)
    {
        *day = (uint8_t)((e->timestamp >> 17) & 0x1FU);
    }

    if (hour != NULL)
    {
        *hour = (uint8_t)((e->timestamp >> 12) & 0x1FU);
    }

    if (minute != NULL)
    {
        *minute = (uint8_t)((e->timestamp >> 6) & 0x3FU);
    }

    if (second != NULL)
    {
        *second = (uint8_t)(e->timestamp & 0x3FU);
    }
}


//-----------------corrected using ChatGpt----------------------//



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
                         uint16_t flags)
{
    LogEntry entry;

    memset(&entry, 0, sizeof(entry));

    entry.seq = g_seq_counter++;
    entry.timestamp = Logging_PackTimestamp(year, month, day, hour, minute, second);
    entry.type = (uint16_t)type;
    entry.status = (uint8_t)status;
    entry.source = (uint8_t)source;

    entry.value = value;
    entry.extra = extra;
    entry.extra2 = extra2;

    /*
     * For now keep both explicit flags and reminder/delay info.
     * This helps preserve your current behavior while unifying the path.
     */
    entry.flags = flags;

    memset(entry.med_id, 0, sizeof(entry.med_id));
    if (med_id != NULL)
    {
        strncpy(entry.med_id, med_id, sizeof(entry.med_id) - 1U);
        entry.med_id[sizeof(entry.med_id) - 1U] = '\0';
    }

    /*
     * Temporary compact convention:
     * - reminder_count stored in flags lower byte if caller wants
     * - delay_minutes stored in extra2 if caller wants
     *
     * We are not forcing one rigid meaning yet because your existing code
     * already uses value/extra/extra2 differently per event.
     */
    (void)reminder_count;
    (void)delay_minutes;

    if (!g_loggingTaskReady || (g_loggingQueue == NULL))
    {
        PRINTF("SYS,LOG_NOT_READY\r\n");
        return false;
    }

    if (xQueueSend(g_loggingQueue, &entry, 0) != pdPASS)
    {
        PRINTF("SYS,LOG_QUEUE_FULL\r\n");
        return false;
    }

    return true;
}





