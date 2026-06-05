#include "reminder.h"
#include "alert.h"
#include "logging.h"
#include "display.h"
#include "rtc.h"
#include "servo.h"
#include "weight.h"
#include "flash_logger.h"
#include "fsl_debug_console.h"
#include "dispense_verify.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

#include "FreeRTOS.h"
#include "task.h"
#include "risk.h"
#include "server_logger.h"

#define MAX_SCHEDULE  8
#define SNOOZE_DELAY_SECONDS  30U

/* Risk-adaptive reminder behavior */
#define REMINDER_REPEAT_SEC_LOW      10U
#define REMINDER_REPEAT_SEC_MEDIUM    8U
#define REMINDER_REPEAT_SEC_HIGH      5U
#define REMINDER_MAX_ATTEMPTS         3U   /* keep unchanged for now */

#define FORCE_TEST_RISK             0
#define TEST_RISK_LEVEL             RISK_HIGH

/* AI pre-alert behavior */
#define PREALERT_DURATION_SECONDS     2U

/* Post-alarm grace window */
#define POST_TIMEOUT_VERIFY_SECONDS  30U

/* Tray / weight detection tuning */
#define TRAY_PILL_PRESENT_THRESHOLD_GRAMS   1.0f
#define TRAY_EMPTY_TOLERANCE_GRAMS          0.8f

/* Logging tags for LOG_TYPE_WEIGHT_CHANGE.extra */
#define WT_PHASE_BASELINE_CAPTURED   1
#define WT_PHASE_PILL_PRESENT        2
#define WT_PHASE_PILL_REMOVED        3
#define WT_PHASE_VERIFY_STARTED      4
#define WT_PHASE_VERIFY_TAKEN        5
#define WT_PHASE_VERIFY_MISSED       6
#define WT_PHASE_WEIGHT_INVALID      7

typedef enum {
    ST_IDLE = 0,
    ST_PREALERT,
    ST_REMINDING,
    ST_POST_TIMEOUT_VERIFY
} RemState;


//////////////////////////////////////////////////////////////////////////////////////////////////////////
//-------------------This file is created iteratively after various changes-----------------------------//
//-------------------Initially I created this file but then had to integrate----------------------------//
//-------------------different functions one by one and we took Help for some functions-----------------//
//-------------------to integrate it properly-----------------------------------------------------------//
//////////////////////////////////////////////////////////////////////////////////////////////////////////



static DoseTime g_schedule[MAX_SCHEDULE];
static uint8_t  g_schedule_count = 0;

/* Snooze trigger time */
static bool     g_snooze_pending = false;
static uint8_t  g_snooze_hour = 0;
static uint8_t  g_snooze_min  = 0;
static uint8_t  g_snooze_sec  = 0;

static RemState g_state = ST_IDLE;
static bool     g_active = false;

static uint8_t  g_target_hour = 0;
static uint8_t  g_target_min  = 0;

static uint32_t g_active_seconds = 0;
static uint8_t  g_attempts = 0;

/* One risk state per scheduled medicine */
static RiskState_t g_risk_states[MAX_SCHEDULE];
static RiskState_t* g_active_risk = NULL;

static uint32_t g_repeat_seconds = REMINDER_REPEAT_SEC_LOW;

static char g_active_med_id[MED_ID_MAX_LEN];

/* Pre-alert context */
static uint32_t g_prealert_seconds = 0;
static char     g_prealert_med_id[MED_ID_MAX_LEN];
static uint8_t  g_prealert_hour = 0;
static uint8_t  g_prealert_min  = 0;

/* Post-timeout verification context */
static uint32_t g_verify_wait_seconds = 0;

/* Weight / tray context for current dose */
static bool  g_dispense_done_for_current_dose = false;
static bool  g_pill_present_seen = false;
static bool  g_weight_valid_for_current_dose = false;
static float g_tray_base_grams = 0.0f;
static float g_last_tray_grams = 0.0f;

/* Day rollover tracking */
static bool    g_day_valid = false;
static uint8_t g_last_dow  = 0;

/* ISR flags */
static volatile bool g_snooze_flag = false;

/* ------------------------------------------------------------ */
/* Risk helpers                                                 */
/* ------------------------------------------------------------ */

static RiskState_t* get_risk_state_for_med(const char* med_id)
{
    if (med_id == NULL) {
        return NULL;
    }

    for (uint8_t i = 0; i < g_schedule_count; i++)
    {
        if (strncmp(g_schedule[i].med_id, med_id, MED_ID_MAX_LEN) == 0)
        {
            return &g_risk_states[i];
        }
    }

    return NULL;
}

static RiskLevel_t get_active_risk_level(void)
{
    return (g_active_risk != NULL) ? g_active_risk->risk_level : RISK_LOW;
}

static uint8_t get_active_risk_percent(void)
{
    return (g_active_risk != NULL) ? Risk_GetPercent(g_active_risk) : 0U;
}

static uint8_t get_active_missed_streak(void)
{
    return (g_active_risk != NULL) ? g_active_risk->missed_streak : 0U;
}

static void refresh_active_risk_state(void)
{
    if (g_active_risk != NULL)
    {
        Risk_RefreshCurrentLevel(g_active_risk);
    }
}

static void rebuild_risk_states_from_flash(void)
{
    uint32_t count;
    uint32_t i;

    /* Start clean before replay */
    for (i = 0; i < MAX_SCHEDULE; i++)
    {
        Risk_Init(&g_risk_states[i]);
    }

    count = FlashLogger_GetCount();

    PRINTF("SYS,RISK_REBUILD_START,count=%lu\r\n", (unsigned long)count);

    for (i = 0; i < count; i++)
    {
        LogEntry entry;
        RiskState_t *rs = NULL;
        bool taken = false;
        bool missed = false;
        bool weight_ack = false;
        uint8_t reminder_count = 0U;
        bool dispense_success = true;

        if (!FlashLogger_ReadAt(i, &entry))
        {
            continue;
        }

        if ((LogType)entry.type != LOG_TYPE_RISK_CYCLE)
        {
            continue;
        }

        rs = get_risk_state_for_med(entry.med_id);
        if (rs == NULL)
        {
            /* Flash may contain logs for meds not in current schedule */
            continue;
        }

        reminder_count = (entry.value >= 0) ? (uint8_t)entry.value : 0U;
        dispense_success = (entry.extra == 0) ? true : false;
        weight_ack = (entry.extra2 != 0) ? true : false;

        taken = ((DoseStatus)entry.status == DOSE_TAKEN);
        missed = ((DoseStatus)entry.status == DOSE_MISSED);

        Risk_OnReminderAttempt(rs, reminder_count);
        Risk_OnDispenseVerify(rs, dispense_success);
        Risk_OnWeightAck(rs, weight_ack);
        Risk_CloseDoseCycle(rs, taken, missed, weight_ack);

        PRINTF("SYS,RISK_REPLAY,med=%s,rem=%u,disp_fail=%d,ack=%d,status=%d,risk=%u\r\n",
               entry.med_id,
               (unsigned int)reminder_count,
               (int)entry.extra,
               (int)entry.extra2,
               (int)entry.status,
               (unsigned int)Risk_GetPercent(rs));
    }

    PRINTF("SYS,RISK_REBUILD_DONE\r\n");
}

/* ------------------------------------------------------------ */
/* Time helpers                                                 */
/* ------------------------------------------------------------ */

static void log_simple_reminder_event(LogType type,
                                      const char* med_id,
                                      DoseStatus status,
                                      uint8_t reminder_count,
                                      uint16_t delay_minutes,
                                      const RtcTime* now,
                                      int16_t value,
                                      int16_t extra)
{
    if (now == NULL) {
        return;
    }

    (void)Logging_LogSimpleEvent(type,
            med_id,
            status,
            reminder_count,
            delay_minutes,
            now->year,
            now->month,
            now->day,
            now->hour,
            now->minute,
            now->second,
            value,
            extra);
}

static void start_alert_for_current_risk(const RtcTime* now)
{
    RiskLevel_t level;

    refresh_active_risk_state();
    level = get_active_risk_level();

    if (level >= RISK_HIGH)
    {
        PRINTF("SYS,ALERT_ESCALATED,HIGH_RISK\r\n");

        log_simple_reminder_event(LOG_TYPE_RETRY_ATTEMPT,
                                  g_active_med_id,
                                  DOSE_PENDING,
                                  g_attempts,
                                  0U,
                                  now,
                                  91,
                                  (int16_t)level);

        /* simple embedded-safe escalation:
           trigger normal alert pattern again */
        Alert_StartPattern();
        vTaskDelay(pdMS_TO_TICKS(150));
        Alert_StartPattern();
    }
    else
    {
        Alert_StartPattern();
    }
}

static void apply_risk_policy_from_current_state(void)
{
    RiskLevel_t level;
    RiskPolicy_t policy;

    refresh_active_risk_state();
    level = get_active_risk_level();
    policy = Risk_GetPolicy(level);

    g_repeat_seconds = policy.repeat_seconds;

    PRINTF("SYS,APPLY_POLICY,level=%d,repeat=%lu,max_attempts=%u\r\n",
           (int)level,
           (unsigned long)g_repeat_seconds,
           (unsigned int)policy.max_attempts);
}

static bool time_matches(const RtcTime* t, uint8_t h, uint8_t m)
{
    return (t->hour == h && t->minute == m && t->second == 0U);
}

static bool time_matches_hms(const RtcTime* t, uint8_t h, uint8_t m, uint8_t s)
{
    return (t->hour == h && t->minute == m && t->second == s);
}

static uint32_t now_to_seconds(const RtcTime* t)
{
    return ((uint32_t)t->hour * 3600U) +
           ((uint32_t)t->minute * 60U) +
           (uint32_t)t->second;
}

static uint32_t dose_to_seconds(uint8_t h, uint8_t m)
{
    return ((uint32_t)h * 3600U) + ((uint32_t)m * 60U);
}

static uint16_t calculate_delay_minutes(const RtcTime* now,
                                        uint8_t target_hour,
                                        uint8_t target_min)
{
    uint32_t now_sec;
    uint32_t target_sec;
    uint32_t diff_sec;

    if (now == NULL) {
        return 0U;
    }

    now_sec = now_to_seconds(now);
    target_sec = dose_to_seconds(target_hour, target_min);

    diff_sec = (now_sec + 86400U - target_sec) % 86400U;
    return (uint16_t)(diff_sec / 60U);
}

static void compute_snooze_target(const RtcTime* now,
                                  uint32_t add_seconds,
                                  uint8_t* out_h,
                                  uint8_t* out_m,
                                  uint8_t* out_s)
{
    uint32_t total = (uint32_t)now->hour * 3600U +
                     (uint32_t)now->minute * 60U +
                     (uint32_t)now->second;

    total += add_seconds;
    total %= 86400U;

    *out_h = (uint8_t)(total / 3600U);
    total %= 3600U;
    *out_m = (uint8_t)(total / 60U);
    *out_s = (uint8_t)(total % 60U);
}

/* ------------------------------------------------------------ */
/* Daily scheduling helpers                                     */
/* ------------------------------------------------------------ */

static uint32_t reminder_timeout_seconds(void)
{
    RiskLevel_t level;
    RiskPolicy_t policy;

    refresh_active_risk_state();
    level = get_active_risk_level();
    policy = Risk_GetPolicy(level);

    return (policy.repeat_seconds * policy.max_attempts);
}

static void reset_daily_flags(void)
{
    for (uint8_t i = 0; i < g_schedule_count; i++) {
        g_schedule[i].prealert_sent_today = false;
        g_schedule[i].reminder_sent_today = false;
    }
}

static void handle_day_rollover(const RtcTime* now)
{
    if (!g_day_valid) {
        g_last_dow = now->dow;
        g_day_valid = true;
        return;
    }

    if (now->dow != g_last_dow) {
        reset_daily_flags();
        g_last_dow = now->dow;
        PRINTF("Reminder day rollover detected. Daily flags reset.\r\n");
    }
}

static DoseTime* schedule_hit(const RtcTime* now)
{
    for (uint8_t i = 0; i < g_schedule_count; i++) {
        if (!g_schedule[i].reminder_sent_today &&
            time_matches(now, g_schedule[i].hour, g_schedule[i].minute))
        {
            return &g_schedule[i];
        }
    }
    return NULL;
}

static DoseTime* prealert_hit(const RtcTime* now)
{
    uint32_t now_sec = now_to_seconds(now);

    for (uint8_t i = 0; i < g_schedule_count; i++) {

        uint32_t dose_sec = dose_to_seconds(g_schedule[i].hour, g_schedule[i].minute);
        uint32_t pre_sec =
            (dose_sec + 86400U - (uint32_t)g_schedule[i].prealert_lead_sec) % 86400U;

        if (!g_schedule[i].ai_high_risk) {
            continue;
        }

        if (g_schedule[i].prealert_lead_sec == 0U) {
            continue;
        }

        if (g_schedule[i].prealert_sent_today) {
            continue;
        }

        if (g_schedule[i].reminder_sent_today) {
            continue;
        }

        if (now_sec == pre_sec) {
            return &g_schedule[i];
        }
    }

    return NULL;
}

/* ------------------------------------------------------------ */
/* Logging helpers                                              */
/* ------------------------------------------------------------ */

static void log_dose_event(const DoseEvent* e, const RtcTime* now)
{
    if ((e == NULL) || (now == NULL)) {
        return;
    }

    (void)Logging_LogDoseEvent(e, now->year,
                                   now->month,
                                   now->day,
                                   now->hour,
                                   now->minute,
                                   now->second);
}

/* ------------------------------------------------------------ */
/* Weight / tray helpers                                        */
/* ------------------------------------------------------------ */

static bool tray_read_grams(float* out_grams)
{
    WeightData wd;

    if (out_grams == NULL) {
        return false;
    }

    if (!Weight_GetData(&wd)) {
        return false;
    }

    if (!wd.valid) {
        return false;
    }

    *out_grams = wd.grams;
    return true;
}

static bool tray_has_payload(float grams)
{
    return (grams >= (g_tray_base_grams + TRAY_PILL_PRESENT_THRESHOLD_GRAMS));
}

static bool tray_is_back_to_base(float grams)
{
    return (grams <= (g_tray_base_grams + TRAY_EMPTY_TOLERANCE_GRAMS));
}

static void reset_current_dose_tracking(void)
{
    g_dispense_done_for_current_dose = false;
    g_pill_present_seen = false;
    g_weight_valid_for_current_dose = false;
    g_tray_base_grams = 0.0f;
    g_last_tray_grams = 0.0f;
    g_verify_wait_seconds = 0U;
}

static void begin_new_dose_tracking(const DoseTime* hit)
{
    float base_grams = 0.0f;

    reset_current_dose_tracking();

    g_target_hour = hit->hour;
    g_target_min  = hit->minute;

    strncpy(g_active_med_id, hit->med_id, sizeof(g_active_med_id) - 1U);
    g_active_med_id[sizeof(g_active_med_id) - 1U] = '\0';

    if (tray_read_grams(&base_grams)) {
        g_tray_base_grams = base_grams;
        g_last_tray_grams = base_grams;
        g_weight_valid_for_current_dose = true;

        PRINTF("Tray baseline captured for %s: %d x0.01g\r\n",
               g_active_med_id,
               (int)(g_tray_base_grams * 100.0f));
    } else {
        g_tray_base_grams = 0.0f;
        g_last_tray_grams = 0.0f;
        g_weight_valid_for_current_dose = false;

        PRINTF("Tray baseline capture failed for %s\r\n", g_active_med_id);
    }
}

static int weight_to_milli(float v)
{
    if ((v != v) || (v < -1000.0f) || (v > 1000.0f))
    {
        return 0;
    }

    if (v >= 0.0f)
    {
        return (int)(v * 1000.0f + 0.5f);
    }
    else
    {
        return (int)(v * 1000.0f - 0.5f);
    }
}

static void post_risk_weights_to_server(const char *med_id, const RiskState_t *rs)
{
    char json[192];
    int w_missed_milli;
    int w_reminder_milli;
    int w_failure_milli;
    int w_ack_milli;

    if ((med_id == NULL) || (rs == NULL))
    {
        PRINTF("SYS,RISK_WEIGHTS_POST_SKIP_NULL\r\n");
        return;
    }

    w_missed_milli   = weight_to_milli(rs->w_missed);
    w_reminder_milli = weight_to_milli(rs->w_reminder);
    w_failure_milli  = weight_to_milli(rs->w_failure);
    w_ack_milli      = weight_to_milli(rs->w_ack);

    snprintf(json, sizeof(json),
             "{"
             "\"med_id\":\"%s\","
             "\"w_missed_milli\":%d,"
             "\"w_reminder_milli\":%d,"
             "\"w_failure_milli\":%d,"
             "\"w_ack_milli\":%d"
             "}",
             med_id,
             w_missed_milli,
             w_reminder_milli,
             w_failure_milli,
             w_ack_milli);

    PRINTF("SYS,RISK_WEIGHTS_POST_JSON,%s\r\n", json);

    if (!ServerLogger_Enqueue("/api/risk_weights/update", json))
    {
        PRINTF("SYS,RISK_WEIGHTS_POST_QUEUE_FAIL,med=%s\r\n", med_id);
    }
    else
    {
        PRINTF("SYS,RISK_WEIGHTS_POST_QUEUE_OK,med=%s\r\n", med_id);
    }
}

static void update_weight_observation(const RtcTime* now)
{
    float grams = 0.0f;

    if (!tray_read_grams(&grams)) {
        return;
    }

    g_last_tray_grams = grams;
    g_weight_valid_for_current_dose = true;

    if (!g_pill_present_seen && tray_has_payload(grams)) {
        g_pill_present_seen = true;

        PRINTF("Tray payload detected for %s: %d x0.01g (base=%d x0.01g)\r\n",
               g_active_med_id,
               (int)(grams * 100.0f),
               (int)(g_tray_base_grams * 100.0f));

        log_simple_reminder_event(LOG_TYPE_WEIGHT_CHANGE,
                                  g_active_med_id,
                                  DOSE_PENDING,
                                  g_attempts,
                                  0U,
                                  now,
                                  (int16_t)(grams * 100.0f),
                                  WT_PHASE_PILL_PRESENT);
    }
}

static void log_risk_cycle_event(const RtcTime* now,
                                 bool taken,
                                 bool missed,
                                 bool weight_ack)
{
    int16_t reminder_count = 0;
    int16_t dispense_failed = 0;
    int16_t weight_ack_i16 = 0;
    uint16_t risk_percent = 0U;
    DoseStatus status = DOSE_PENDING;

    if ((now == NULL) || (g_active_risk == NULL))
    {
        return;
    }

    reminder_count = (int16_t)g_active_risk->current_reminder_count;
    dispense_failed = (int16_t)g_active_risk->current_dispense_failed;
    weight_ack_i16 = weight_ack ? 1 : 0;
    risk_percent = (uint16_t)Risk_GetPercent(g_active_risk);

    if (taken)
    {
        status = DOSE_TAKEN;
    }
    else if (missed)
    {
        status = DOSE_MISSED;
    }

    (void)Logging_SubmitEvent(
        LOG_TYPE_RISK_CYCLE,
        LOG_SRC_REMINDER,
        g_active_med_id,
        status,
        (uint8_t)reminder_count,
        0U,
        now->year,
        now->month,
        now->day,
        now->hour,
        now->minute,
        now->second,
        reminder_count,
        dispense_failed,
        weight_ack_i16,
        risk_percent
    );
}

static void finalize_current_dose(DoseStatus final_status, const RtcTime* now)
{
    DoseEvent e;
    uint16_t delay;
    bool taken = (final_status == DOSE_TAKEN);
    bool missed = (final_status == DOSE_MISSED);
    bool weight_ack = taken;

    delay = calculate_delay_minutes(now, g_target_hour, g_target_min);

    memset(&e, 0, sizeof(e));
    e.hour = g_target_hour;
    e.minute = g_target_min;
    e.dow = now->dow;
    e.status = final_status;
    e.reminder_count = g_attempts;
    e.delay_minutes = delay;
    strncpy(e.med_id, g_active_med_id, sizeof(e.med_id) - 1U);
    e.med_id[sizeof(e.med_id) - 1U] = '\0';

    log_dose_event(&e, now);

    if (g_active_risk != NULL) {
        Risk_CloseDoseCycle(g_active_risk, taken, missed, weight_ack);
        log_risk_cycle_event(now, taken, missed, weight_ack);
        PRINTF("SYS,BEFORE_POST_WEIGHTS,med=%s\r\n", g_active_med_id);
        post_risk_weights_to_server(g_active_med_id, g_active_risk);
        PRINTF("SYS,AFTER_POST_WEIGHTS,med=%s\r\n", g_active_med_id);
    }

    PRINTF("SYS,LOCAL_RISK,score_x100=%u,level=%d,missed_streak=%u\r\n",
           (unsigned)get_active_risk_percent(),
           (int)get_active_risk_level(),
           (unsigned)get_active_missed_streak());

    PRINTF("SYS,FINALIZE_DOSE,status=%d,med=%s\r\n", (int)final_status, g_active_med_id);

    Alert_Stop();
    PRINTF("SYS,ALERT_STOP\r\n");

    Display_HoldLatestEvent(5000);

    g_state = ST_IDLE;
    g_active = false;
    g_snooze_pending = false;
    g_active_risk = NULL;

    PRINTF("SYS,REMINDER_STATE_IDLE\r\n");

    Weight_ClearTwinOverride();
    reset_current_dose_tracking();
}

static bool try_finish_taken_by_weight(const RtcTime* now)
{
    float grams = 0.0f;

    if (!tray_read_grams(&grams)) {
        return false;
    }

    g_last_tray_grams = grams;
    g_weight_valid_for_current_dose = true;

    if (!g_pill_present_seen) {
        if (tray_has_payload(grams)) {
            g_pill_present_seen = true;

            PRINTF("Tray payload detected for %s: %d x0.01g (base=%d x0.01g)\r\n",
                   g_active_med_id,
                   (int)(grams * 100.0f),
                   (int)(g_tray_base_grams * 100.0f));

            log_simple_reminder_event(LOG_TYPE_WEIGHT_CHANGE,
                                      g_active_med_id,
                                      DOSE_PENDING,
                                      g_attempts,
                                      0U,
                                      now,
                                      (int16_t)(grams * 100.0f),
                                      WT_PHASE_PILL_PRESENT);
        } else {
            return false;
        }
    }

    if (tray_is_back_to_base(grams)) {
        uint16_t delay = calculate_delay_minutes(now, g_target_hour, g_target_min);

        PRINTF("SYS,TAKEN_DETECTED_DURING_REMINDER\r\n");
        PRINTF("Medication TAKEN confirmed by tray removal for %s: %d x0.01g -> base %d x0.01g\r\n",
               g_active_med_id,
               (int)(grams * 100.0f),
               (int)(g_tray_base_grams * 100.0f));

        log_simple_reminder_event(LOG_TYPE_WEIGHT_CHANGE,
                                  g_active_med_id,
                                  DOSE_TAKEN,
                                  g_attempts,
                                  delay,
                                  now,
                                  (int16_t)(grams * 100.0f),
                                  WT_PHASE_PILL_REMOVED);

        if (g_active_risk != NULL) {
            Risk_OnWeightAck(g_active_risk, true);
        }

        finalize_current_dose(DOSE_TAKEN, now);
        return true;
    }

    return false;
}

/* ------------------------------------------------------------ */
/* State transitions                                            */
/* ------------------------------------------------------------ */

static void start_prealert(const DoseTime* hit, const RtcTime* now)
{
    if ((hit == NULL) || (now == NULL)) return;

    g_state = ST_PREALERT;
    g_active = true;
    g_prealert_seconds = 0U;

    strncpy(g_prealert_med_id, hit->med_id, sizeof(g_prealert_med_id) - 1U);
    g_prealert_med_id[sizeof(g_prealert_med_id) - 1U] = '\0';

    g_prealert_hour = hit->hour;
    g_prealert_min  = hit->minute;

    Display_ExitLogViewer();
    Display_ShowReminderScreen(hit->hour, hit->minute);
    start_alert_for_current_risk(now);

    PRINTF("AI PRE-ALERT triggered for %s at %02u:%02u:%02u "
           "for scheduled dose %02u:%02u (lead=%u sec)\r\n",
           g_prealert_med_id,
           now->hour, now->minute, now->second,
           hit->hour, hit->minute,
           (unsigned)hit->prealert_lead_sec);

    log_simple_reminder_event(LOG_TYPE_PREALERT_TRIGGERED,
                              g_prealert_med_id,
                              DOSE_PENDING,
                              0U,
                              0U,
                              now,
                              (int16_t)hit->prealert_lead_sec,
                              0);
}

static void start_initial_reminder(const DoseTime* hit, const RtcTime* now)
{
    if ((hit == NULL) || (now == NULL)) return;

    begin_new_dose_tracking(hit);
    g_active_risk = get_risk_state_for_med(hit->med_id);
    refresh_active_risk_state();

#if FORCE_TEST_RISK
    if (g_active_risk != NULL) {
        g_active_risk->risk_level = TEST_RISK_LEVEL;
    }
    Reminder_SetAiProfile("MedA", true, 15);
#endif

    g_state = ST_REMINDING;
    g_active = true;
    g_active_seconds = 0U;
    g_attempts = 1U;

    if (g_active_risk != NULL) {
        Risk_OnReminderAttempt(g_active_risk, g_attempts);
        refresh_active_risk_state();
    }

    apply_risk_policy_from_current_state();

    Display_ExitLogViewer();
    Display_ShowReminderScreen(hit->hour, hit->minute);
    start_alert_for_current_risk(now);

    PRINTF("SYS,REMINDER_ACTIVE_ON\r\n");
    PRINTF("SYS,ALERT_START\r\n");
    PRINTF("SYS,REMINDER_ATTEMPT_%d\r\n", (int)g_attempts);

    PRINTF("SYS,REM_POLICY,start,level=%d,repeat_sec=%lu\r\n",
           (int)get_active_risk_level(),
           (unsigned long)g_repeat_seconds);

    PRINTF("Reminder triggered for %s at %02u:%02u\r\n",
           g_active_med_id, hit->hour, hit->minute);

    log_simple_reminder_event(LOG_TYPE_REMINDER_TRIGGERED,
                              g_active_med_id,
                              DOSE_PENDING,
                              g_attempts,
                              0U,
                              now,
                              hit->hour,
                              hit->minute);

    log_simple_reminder_event(LOG_TYPE_WEIGHT_CHANGE,
                              g_active_med_id,
                              DOSE_PENDING,
                              g_attempts,
                              0U,
                              now,
                              (int16_t)(g_tray_base_grams * 100.0f),
                              g_weight_valid_for_current_dose ? WT_PHASE_BASELINE_CAPTURED
                                                               : WT_PHASE_WEIGHT_INVALID);

    /* Dispense once only at first reminder trigger, with closed-loop verification */
    {
        DispenseVerifyResult vr;

        if (DispenseVerify_Run(g_active_med_id, &vr))
        {
            g_dispense_done_for_current_dose = true;

            if (g_active_risk != NULL) {
                Risk_OnDispenseVerify(g_active_risk, vr.success);
                refresh_active_risk_state();
                apply_risk_policy_from_current_state();
            }

            PRINTF("\r\n=== DISPENSE VERIFY ===\r\n");
            PRINTF("MED: %s\r\n", vr.med_id);
            PRINTF("RESULT: %s\r\n", vr.result);
            PRINTF("PRE_x100: %d\r\n", (int)(vr.pre_weight_g * 100.0f));
            PRINTF("POST_x100: %d\r\n", (int)(vr.post_weight_g * 100.0f));
            PRINTF("DELTA_x100: %d\r\n", (int)(vr.delta_g * 100.0f));
            PRINTF("THRESH_x100: %d\r\n", (int)(vr.threshold_g * 100.0f));

            log_simple_reminder_event(LOG_TYPE_WEIGHT_CHANGE,
                                      g_active_med_id,
                                      vr.success ? DOSE_PENDING : DOSE_MISSED,
                                      g_attempts,
                                      0U,
                                      now,
                                      (int16_t)(vr.delta_g * 100.0f),
                                      vr.success ? 81 : 82);
        }
        else
        {
            g_dispense_done_for_current_dose = false;

            if (g_active_risk != NULL) {
                Risk_OnDispenseVerify(g_active_risk, false);
                refresh_active_risk_state();
                apply_risk_policy_from_current_state();
            }

            PRINTF("\r\n=== DISPENSE VERIFY ERROR ===\r\n");
            PRINTF("MED: %s\r\n", vr.med_id);
            PRINTF("STATUS: %s\r\n", DispenseVerify_StatusToString(vr.status));
            PRINTF("NOTE: %s\r\n", vr.note);

            log_simple_reminder_event(LOG_TYPE_WEIGHT_CHANGE,
                                      g_active_med_id,
                                      DOSE_MISSED,
                                      g_attempts,
                                      0U,
                                      now,
                                      0,
                                      83);
        }
    }
}

static void start_snoozed_reminder(const RtcTime* now)
{
    if (g_active_risk == NULL) {
        g_active_risk = get_risk_state_for_med(g_active_med_id);
    }

    refresh_active_risk_state();

    g_state = ST_REMINDING;
    g_active = true;
    g_active_seconds = 0U;
    g_snooze_pending = false;

    /* No re-dispense on snooze */
    if (g_attempts == 0U) {
        g_attempts = 1U;
    }

    if (g_active_risk != NULL) {
        Risk_OnReminderAttempt(g_active_risk, g_attempts);
        refresh_active_risk_state();
    }

    apply_risk_policy_from_current_state();

    Display_ExitLogViewer();
    Display_ShowReminderScreen(g_target_hour, g_target_min);
    start_alert_for_current_risk(now);

    PRINTF("SYS,REMINDER_ACTIVE_ON\r\n");
    PRINTF("SYS,ALERT_START\r\n");
    PRINTF("SYS,REMINDER_ATTEMPT_%d\r\n", (int)g_attempts);

    PRINTF("Snooze reminder triggered at %02u:%02u:%02u for %s (no re-dispense)\r\n",
           now->hour, now->minute, now->second, g_active_med_id);

    log_simple_reminder_event(LOG_TYPE_REMINDER_TRIGGERED,
                              g_active_med_id,
                              DOSE_SNOOZED,
                              g_attempts,
                              0U,
                              now,
                              g_target_hour,
                              g_target_min);
}

/* ------------------------------------------------------------ */
/* Public API                                                   */
/* ------------------------------------------------------------ */

void Reminder_AckFromISR(void)
{
    /* SW2 no longer controls TAKEN logic. Keep as no-op. */
}

/* ----------------------- TWIN ------------------- */

ReminderPhase Reminder_GetPhase(void)
{
    switch (g_state)
    {
        case ST_PREALERT:
            return REMINDER_PHASE_PREALERT;

        case ST_REMINDING:
            return REMINDER_PHASE_ACTIVE;

        case ST_POST_TIMEOUT_VERIFY:
            return REMINDER_PHASE_GRACE;

        case ST_IDLE:
        default:
            return REMINDER_PHASE_IDLE;
    }
}

const char *Reminder_GetPhaseString(void)
{
    switch (Reminder_GetPhase())
    {
        case REMINDER_PHASE_PREALERT:
            return "PREALERT";

        case REMINDER_PHASE_ACTIVE:
            return "ACTIVE";

        case REMINDER_PHASE_GRACE:
            return "GRACE";

        case REMINDER_PHASE_IDLE:
        default:
            return "IDLE";
    }
}

/* ------------------ TWIN --------------------------------- */

bool Reminder_RemoteAck(void)
{
    if (g_state == ST_PREALERT)
    {
        Alert_Stop();
        PRINTF("SYS,ALERT_STOP\r\n");

        g_state = ST_IDLE;
        g_active = false;
        g_prealert_seconds = 0U;

        PRINTF("SYS,REMOTE_ACK,PREALERT\r\n");
        PRINTF("SYS,REMINDER_STATE_IDLE\r\n");
        return true;
    }

    if (g_state == ST_REMINDING)
    {
        Alert_Stop();
        PRINTF("SYS,ALERT_STOP\r\n");

        g_state = ST_IDLE;
        g_active = false;

        PRINTF("SYS,REMOTE_ACK,REMINDER\r\n");
        PRINTF("SYS,REMINDER_STATE_IDLE\r\n");
        return true;
    }

    return false;
}

bool Reminder_RemoteSnooze(void)
{
    if (g_state != ST_REMINDING)
    {
        return false;
    }

    g_snooze_flag = true;

    PRINTF("SYS,REMOTE_SNOOZE,REQUESTED\r\n");
    return true;
}

bool Reminder_RemoteTaken(void)
{
    RtcTime now;

    if ((g_state != ST_PREALERT) && (g_state != ST_REMINDING))
    {
        return false;
    }

    memset(&now, 0, sizeof(now));
    (void)RTC_GetTime(&now);

    Alert_Stop();
    PRINTF("SYS,ALERT_STOP\r\n");

    g_prealert_seconds = 0U;
    g_active = false;

    PRINTF("SYS,REMOTE_TAKEN,REQUESTED\r\n");

    finalize_current_dose(DOSE_TAKEN, &now);

    g_state = ST_IDLE;

    PRINTF("SYS,REMOTE_TAKEN,APPLIED\r\n");
    PRINTF("SYS,REMINDER_STATE_IDLE\r\n");

    return true;
}

bool Reminder_GetActiveDoseEvent(DoseEvent *out)
{
    RtcTime now;

    if (out == NULL)
    {
        return false;
    }

    if (!g_active)
    {
        return false;
    }

    memset(out, 0, sizeof(DoseEvent));

    if (!RTC_GetTime(&now))
    {
        return false;
    }

    out->hour = g_target_hour;
    out->minute = g_target_min;
    out->dow = now.dow;

    out->status = DOSE_PENDING;
    out->reminder_count = g_attempts;

    out->delay_minutes = calculate_delay_minutes(&now,
                                                 g_target_hour,
                                                 g_target_min);

    strncpy(out->med_id, g_active_med_id, MED_ID_MAX_LEN - 1U);
    out->med_id[MED_ID_MAX_LEN - 1U] = '\0';

    return true;
}

void Reminder_SnoozeFromISR(void)
{
    g_snooze_flag = true;
}

uint8_t Reminder_GetScheduleCount(void)
{
    return g_schedule_count;
}

void Reminder_Init(void)
{
    g_state = ST_IDLE;
    g_active = false;
    g_snooze_flag = false;
    g_schedule_count = 0U;
    g_active_seconds = 0U;
    g_attempts = 0U;
    g_repeat_seconds = REMINDER_REPEAT_SEC_LOW;

    g_snooze_pending = false;
    g_snooze_hour = 0U;
    g_snooze_min = 0U;
    g_snooze_sec = 0U;

    g_prealert_seconds = 0U;
    g_prealert_hour = 0U;
    g_prealert_min = 0U;

    g_day_valid = false;
    g_last_dow = 0U;

    g_active_risk = NULL;

    memset(g_active_med_id, 0, sizeof(g_active_med_id));
    memset(g_prealert_med_id, 0, sizeof(g_prealert_med_id));
    memset(g_schedule, 0, sizeof(g_schedule));

    reset_current_dose_tracking();

    for (uint8_t i = 0; i < MAX_SCHEDULE; i++)
    {
        Risk_Init(&g_risk_states[i]);
    }
}

void Reminder_SetSchedule(const DoseTime* times, uint8_t count)
{
    PRINTF("SYS,SETSCHEDULE_ENTER,count=%u\r\n", (unsigned int)count);

    if (!times) return;
    if (count > MAX_SCHEDULE) count = MAX_SCHEDULE;

    for (uint8_t i = 0; i < count; i++) {
        g_schedule[i].hour   = times[i].hour;
        g_schedule[i].minute = times[i].minute;

        strncpy(g_schedule[i].med_id, times[i].med_id, MED_ID_MAX_LEN - 1U);
        g_schedule[i].med_id[MED_ID_MAX_LEN - 1U] = '\0';

        g_schedule[i].ai_high_risk        = false;
        g_schedule[i].prealert_lead_sec   = 0U;
        g_schedule[i].prealert_sent_today = false;
        g_schedule[i].reminder_sent_today = false;

        Risk_Init(&g_risk_states[i]);

        if (Risk_LoadWeights(&g_risk_states[i]))
        {
            PRINTF("SYS,WEIGHTS_LOADED,med=%s\r\n", g_schedule[i].med_id);
        }
        else
        {
            PRINTF("SYS,WEIGHTS_DEFAULT,med=%s\r\n", g_schedule[i].med_id);
        }

        Risk_RefreshCurrentLevel(&g_risk_states[i]);

        PRINTF("Schedule[%u]: med=%s time=%02u:%02u\r\n",
               i,
               g_schedule[i].med_id,
               g_schedule[i].hour,
               g_schedule[i].minute);
    }

    g_schedule_count = count;

    PRINTF("Reminder_SetSchedule: loaded %u entries\r\n", g_schedule_count);
}

bool Reminder_SetAiProfile(const char* med_id, bool high_risk, uint16_t lead_sec)
{
    if (!med_id) return false;

    for (uint8_t i = 0; i < g_schedule_count; i++) {
        if (strncmp(g_schedule[i].med_id, med_id, MED_ID_MAX_LEN) == 0) {
            g_schedule[i].ai_high_risk = high_risk;
            g_schedule[i].prealert_lead_sec = high_risk ? lead_sec : 0U;

            PRINTF("AI profile set: med=%s high_risk=%u lead_sec=%u\r\n",
                   g_schedule[i].med_id,
                   (unsigned)g_schedule[i].ai_high_risk,
                   (unsigned)g_schedule[i].prealert_lead_sec);
            return true;
        }
    }

    PRINTF("AI profile failed: med_id not found: %s\r\n", med_id);
    return false;
}

void Reminder_ClearAllAiProfiles(void)
{
    for (uint8_t i = 0; i < g_schedule_count; i++) {
        g_schedule[i].ai_high_risk = false;
        g_schedule[i].prealert_lead_sec = 0U;
        g_schedule[i].prealert_sent_today = false;
    }
}

bool Reminder_IsActive(void)
{
    return g_active;
}

bool Reminder_GetNextScheduledDose(const RtcTime* now, DoseTime* out)
{
    if ((now == NULL) || (out == NULL) || (g_schedule_count == 0U)) {
        return false;
    }

    {
        uint32_t now_sec = now_to_seconds(now);
        uint32_t best_delta = 0xFFFFFFFFU;
        int best_idx = -1;

        for (uint8_t i = 0; i < g_schedule_count; i++) {
            uint32_t dose_sec = dose_to_seconds(g_schedule[i].hour, g_schedule[i].minute);
            uint32_t delta;

            if (dose_sec >= now_sec) {
                delta = dose_sec - now_sec;
            } else {
                delta = (86400U - now_sec) + dose_sec;
            }

            if (delta < best_delta) {
                best_delta = delta;
                best_idx = (int)i;
            }
        }

        if (best_idx < 0) {
            return false;
        }

        *out = g_schedule[best_idx];
        return true;
    }
}

bool Reminder_IsNextDoseHighRisk(void)
{
    RtcTime now;
    DoseTime nextDose;

    if (!RTC_GetTime(&now)) {
        return false;
    }

    if (!Reminder_GetNextScheduledDose(&now, &nextDose)) {
        return false;
    }

    return nextDose.ai_high_risk;
}

RiskLevel_t Reminder_GetActiveRiskLevel(void)
{
    refresh_active_risk_state();
    return get_active_risk_level();
}

static bool consume_snooze(void)
{
    bool v = g_snooze_flag;
    g_snooze_flag = false;
    return v;
}

void Reminder_OnSecondTick(const RtcTime* now)
{
    if (!now) return;

    handle_day_rollover(now);

    if (g_state == ST_IDLE) {
        if (g_snooze_pending &&
            time_matches_hms(now, g_snooze_hour, g_snooze_min, g_snooze_sec))
        {
            start_snoozed_reminder(now);
            return;
        }

        /* Check pre-alert first */
        {
            DoseTime* phit = prealert_hit(now);

            if (phit) {
                phit->prealert_sent_today = true;
                start_prealert(phit, now);
                return;
            }
        }

        /* Then exact dose trigger */
        {
            DoseTime* hit = schedule_hit(now);
            if (hit) {
                hit->reminder_sent_today = true;
                start_initial_reminder(hit, now);
                return;
            }
        }

        return;
    }

    if (g_state == ST_PREALERT) {
        g_prealert_seconds++;

        (void)consume_snooze();

        if (g_prealert_seconds >= PREALERT_DURATION_SECONDS) {
            Alert_Stop();
            g_state = ST_IDLE;
            g_active = false;

            PRINTF("AI PRE-ALERT completed for %s\r\n", g_prealert_med_id);
            return;
        }

        return;
    }

    if (g_state == ST_REMINDING) {
        g_active_seconds++;

        PRINTF("DBG,REM,t=%lu,repeat=%lu,attempts=%u,state=%d\r\n",
               (unsigned long)g_active_seconds,
               (unsigned long)g_repeat_seconds,
               (unsigned int)g_attempts,
               (int)g_state);

        update_weight_observation(now);

        /* If user takes medicine while alarm is active, stop immediately */
        if (try_finish_taken_by_weight(now)) {
            return;
        }

        if (consume_snooze()) {
            DoseEvent e = {0};

            Display_ShowSnoozedScreen();
            Alert_Stop();
            PRINTF("SYS,ALERT_STOP\r\n");

            log_simple_reminder_event(LOG_TYPE_REMINDER_SNOOZED,
                                      g_active_med_id,
                                      DOSE_SNOOZED,
                                      g_attempts,
                                      0U,
                                      now,
                                      SNOOZE_DELAY_SECONDS,
                                      0);

            e.hour = g_target_hour;
            e.minute = g_target_min;
            e.dow = now->dow;
            e.status = DOSE_SNOOZED;
            e.reminder_count = g_attempts;
            e.delay_minutes = 0U;
            strncpy(e.med_id, g_active_med_id, sizeof(e.med_id) - 1U);
            e.med_id[sizeof(e.med_id) - 1U] = '\0';

            log_dose_event(&e, now);
            Display_HoldLatestEvent(5000);

            compute_snooze_target(now, SNOOZE_DELAY_SECONDS,
                                  &g_snooze_hour, &g_snooze_min, &g_snooze_sec);

            g_snooze_pending = true;

            PRINTF("Snoozed %s until %02u:%02u:%02u\r\n",
                   g_active_med_id, g_snooze_hour, g_snooze_min, g_snooze_sec);

            g_state = ST_IDLE;
            g_active = false;
            PRINTF("SYS,REMINDER_STATE_IDLE\r\n");
            return;
        }

        {
            RiskLevel_t level;
            RiskPolicy_t policy;

            refresh_active_risk_state();
            level = get_active_risk_level();
            policy = Risk_GetPolicy(level);

            if ((g_active_seconds < reminder_timeout_seconds()) &&
                (g_repeat_seconds > 0U) &&
                ((g_active_seconds % g_repeat_seconds) == 0U) &&
                (g_attempts < policy.max_attempts))
            {
                g_attempts++;

                if (g_active_risk != NULL) {
                    Risk_OnReminderAttempt(g_active_risk, g_attempts);
                    refresh_active_risk_state();
                    level = get_active_risk_level();
                    policy = Risk_GetPolicy(level);
                }

                g_repeat_seconds = policy.repeat_seconds;

                start_alert_for_current_risk(now);
                Display_ExitLogViewer();
                Display_ShowReminderAttempt(g_attempts);

                PRINTF("SYS,ALERT_START\r\n");
                PRINTF("SYS,REMINDER_ATTEMPT_%d\r\n", (int)g_attempts);

                PRINTF("SYS,REM_POLICY,repeat,level=%d,repeat_sec=%lu,max_attempts=%u\r\n",
                       (int)level,
                       (unsigned long)g_repeat_seconds,
                       (unsigned int)policy.max_attempts);

                PRINTF("Reminder repeat %u for %s\r\n", g_attempts, g_active_med_id);

                log_simple_reminder_event(LOG_TYPE_RETRY_ATTEMPT,
                                          g_active_med_id,
                                          DOSE_PENDING,
                                          g_attempts,
                                          0U,
                                          now,
                                          (int16_t)g_attempts,
                                          0);
            }
        }

        /* Alarm phase ended: stop alert and enter 30-second grace period */
        if (g_active_seconds >= reminder_timeout_seconds()) {
            Alert_Stop();
            PRINTF("SYS,ALERT_STOP\r\n");

            g_state = ST_POST_TIMEOUT_VERIFY;
            g_active = false;
            g_verify_wait_seconds = 0U;

            PRINTF("Reminder timeout reached for %s. Starting %u-second post-timeout grace window.\r\n",
                   g_active_med_id, (unsigned)POST_TIMEOUT_VERIFY_SECONDS);

            log_simple_reminder_event(LOG_TYPE_WEIGHT_CHANGE,
                                      g_active_med_id,
                                      DOSE_PENDING,
                                      g_attempts,
                                      0U,
                                      now,
                                      (int16_t)(g_last_tray_grams * 100.0f),
                                      WT_PHASE_VERIFY_STARTED);
            return;
        }

        return;
    }

    if (g_state == ST_POST_TIMEOUT_VERIFY) {
        g_verify_wait_seconds++;

        update_weight_observation(now);

        /* If removed during 30-second grace period, mark TAKEN */
        if (g_weight_valid_for_current_dose &&
            g_pill_present_seen &&
            tray_is_back_to_base(g_last_tray_grams))
        {
            uint16_t delay = calculate_delay_minutes(now, g_target_hour, g_target_min);

            PRINTF("SYS,TAKEN_DETECTED_DURING_GRACE\r\n");
            PRINTF("Post-timeout grace result: TAKEN for %s\r\n", g_active_med_id);

            log_simple_reminder_event(LOG_TYPE_WEIGHT_CHANGE,
                                      g_active_med_id,
                                      DOSE_TAKEN,
                                      g_attempts,
                                      delay,
                                      now,
                                      (int16_t)(g_last_tray_grams * 100.0f),
                                      WT_PHASE_VERIFY_TAKEN);

            if (g_active_risk != NULL) {
                Risk_OnWeightAck(g_active_risk, true);
            }

            finalize_current_dose(DOSE_TAKEN, now);
            return;
        }

        /* If still not removed after 30 seconds, mark MISSED */
        if (g_verify_wait_seconds >= POST_TIMEOUT_VERIFY_SECONDS) {
            PRINTF("SYS,MISSED_AFTER_GRACE\r\n");
            PRINTF("Post-timeout grace result: MISSED for %s\r\n", g_active_med_id);

            log_simple_reminder_event(LOG_TYPE_WEIGHT_CHANGE,
                                      g_active_med_id,
                                      DOSE_MISSED,
                                      g_attempts,
                                      0U,
                                      now,
                                      (int16_t)(g_last_tray_grams * 100.0f),
                                      g_weight_valid_for_current_dose ? WT_PHASE_VERIFY_MISSED
                                                                      : WT_PHASE_WEIGHT_INVALID);

            if (g_active_risk != NULL) {
                Risk_OnWeightAck(g_active_risk, false);
            }

            finalize_current_dose(DOSE_MISSED, now);
            return;
        }

        return;
    }
}


void Reminder_RebuildRiskFromFlash(void)
{
    rebuild_risk_states_from_flash();
}


void Reminder_LoadAllRiskWeights(void)
{
    uint8_t i;

    for (i = 0; i < g_schedule_count; i++)
    {
        if (Risk_LoadWeights(&g_risk_states[i]))
        {
            PRINTF("SYS,WEIGHTS_LOADED,med=%s\r\n", g_schedule[i].med_id);
        }
        else
        {
            PRINTF("SYS,WEIGHTS_DEFAULT,med=%s\r\n", g_schedule[i].med_id);
        }

        Risk_RefreshCurrentLevel(&g_risk_states[i]);
    }
}



