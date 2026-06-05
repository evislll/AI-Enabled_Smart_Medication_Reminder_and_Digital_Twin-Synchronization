#include "weight.h"
#include "servo.h"
#include "reminder.h"
#include "server_logger.h"
#include "fsl_debug_console.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

#define TWIN_TRAY_EMPTY_THRESHOLD_G     1
#define TWIN_SERVO_HOME_ANGLE_DEG       5


typedef enum
{
    TWIN_TRAY_UNKNOWN = 0,
    TWIN_TRAY_EMPTY,
    TWIN_TRAY_PILL_PRESENT,
    TWIN_TRAY_REMOVED
} TwinTrayState;

static TwinTrayState s_twin_tray_state = TWIN_TRAY_UNKNOWN;
static bool s_tray_had_pill = false;



static const char *Twin_GetReminderState(void)
{
    return Reminder_GetPhaseString();
}

static const char *Twin_GetServoState(void)
{
    return Servo_GetStateString();
}

static const char *Twin_GetTrayState(int weight_g)
{
    if (weight_g < 0)
    {
        return "UNKNOWN";
    }

    if (weight_g <= TWIN_TRAY_EMPTY_THRESHOLD_G)
    {
        return "EMPTY";
    }

    return "PILL_PRESENT";
}

static const char *Twin_GetWeightSource(void)
{
    return Weight_IsTwinOverrideActive() ? "TWIN_OVERRIDE" : "REAL";
}


static const char *Twin_ResolveTrayState(const char *reminder_state, int weight_g)
{
    bool tray_empty;
    bool reminder_window_active;

    if (weight_g < 0)
    {
        s_twin_tray_state = TWIN_TRAY_UNKNOWN;
        return "UNKNOWN";
    }

    tray_empty = (weight_g <= TWIN_TRAY_EMPTY_THRESHOLD_G);

    reminder_window_active =
        (strcmp(reminder_state, "ACTIVE") == 0) ||
        (strcmp(reminder_state, "GRACE") == 0);

    if (strcmp(reminder_state, "IDLE") == 0)
    {
        s_tray_had_pill = false;
        s_twin_tray_state = tray_empty ? TWIN_TRAY_EMPTY : TWIN_TRAY_PILL_PRESENT;
    }
    else
    {
        if (!tray_empty)
        {
            s_tray_had_pill = true;
            s_twin_tray_state = TWIN_TRAY_PILL_PRESENT;
        }
        else
        {
            if (s_tray_had_pill && reminder_window_active)
            {
                s_twin_tray_state = TWIN_TRAY_REMOVED;
            }
            else
            {
                s_twin_tray_state = TWIN_TRAY_EMPTY;
            }
        }
    }

    switch (s_twin_tray_state)
    {
        case TWIN_TRAY_EMPTY:
            return "EMPTY";

        case TWIN_TRAY_PILL_PRESENT:
            return "PILL_PRESENT";

        case TWIN_TRAY_REMOVED:
            return "REMOVED";

        case TWIN_TRAY_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}


////////////////////////////////////////////////////////////////////////////////////////////
//---------------following function is fixed and enhanced using ChatGpt-------------------//
////////////////////////////////////////////////////////////////////////////////////////////


static void Task_TwinBroadcast(void *pv)
{
    char json[320];
    bool ok;

    uint32_t timestamp_ms;
    int reminder_active;
    int ai_high_risk;
    int servo_state_deg;
    int weight_grams;

    const char *reminder_state;
    const char *servo_state;
    const char *tray_state;
    const char *weight_source;

    (void)pv;

    vTaskDelay(pdMS_TO_TICKS(15000));

    while (1)
    {
        timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        reminder_active = Reminder_IsActive() ? 1 : 0;
        ai_high_risk = Reminder_IsNextDoseHighRisk() ? 1 : 0;
        servo_state_deg = Servo_GetLastAngle();
        weight_grams = Weight_GetLastReading();

        reminder_state = Twin_GetReminderState();
//        servo_state = Twin_GetServoState(servo_state_deg);
        servo_state = Twin_GetServoState();
//        tray_state = Twin_GetTrayState(weight_grams);
        tray_state = Twin_ResolveTrayState(reminder_state, weight_grams);
        weight_source = Twin_GetWeightSource();

//        g_twin_state.timestamp_ms = timestamp_ms;
//        g_twin_state.reminder_active = reminder_active;
//        g_twin_state.ai_high_risk = ai_high_risk;
//        g_twin_state.servo_state_deg = servo_state_deg;
//        g_twin_state.weight_grams = weight_grams;

        snprintf(json, sizeof(json),
                 "{"
                 "\"timestamp_ms\":%lu,"
                 "\"reminder_active\":%d,"
                 "\"ai_high_risk\":%d,"
                 "\"servo_state_deg\":%d,"
                 "\"weight_grams\":%d,"
                 "\"reminder_state\":\"%s\","
                 "\"servo_state\":\"%s\","
                 "\"tray_state\":\"%s\","
                 "\"weight_source\":\"%s\""
                 "}",
                 (unsigned long)timestamp_ms,
                 reminder_active,
                 ai_high_risk,
                 servo_state_deg,
                 weight_grams,
                 reminder_state,
                 servo_state,
                 tray_state,
                 weight_source);

        PRINTF("SYS,TWIN_QUEUE\r\n");

        ok = ServerLogger_PostTwin(json);

        PRINTF("SYS,TWIN_SENT,ok=%d\r\n", ok ? 1 : 0);

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
