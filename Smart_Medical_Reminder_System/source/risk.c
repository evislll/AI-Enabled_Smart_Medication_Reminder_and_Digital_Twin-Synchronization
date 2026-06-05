#include "risk.h"
#include "flash_logger.h"
#include <math.h>
#include "fsl_debug_console.h"


#define RISK_WEIGHTS_MAGIC 0x5249534BU  /* "RISK" */
#define RISK_WEIGHT_SAVE_THRESHOLD 0.01f

static uint32_t Risk_SimpleCrc(const RiskWeightsFlash_t *fw)
{
    const uint8_t *p = (const uint8_t *)fw;
    uint32_t sum = 0U;
    size_t i;

    for (i = 0; i < sizeof(RiskWeightsFlash_t) - sizeof(uint32_t); i++)
    {
        sum += p[i];
    }

    return sum;
}


static bool Risk_WeightsChangedEnough(const RiskState_t *s)
{
    RiskWeightsFlash_t fw;

    if (s == NULL)
    {
        return false;
    }

    /* If nothing saved yet, force first save */
    if (!FlashLogger_LoadRiskWeights(&fw))
    {
        return true;
    }

    if (fabsf(s->w_missed   - fw.w_missed)   > RISK_WEIGHT_SAVE_THRESHOLD) return true;
    if (fabsf(s->w_reminder - fw.w_reminder) > RISK_WEIGHT_SAVE_THRESHOLD) return true;
    if (fabsf(s->w_failure  - fw.w_failure)  > RISK_WEIGHT_SAVE_THRESHOLD) return true;
    if (fabsf(s->w_ack      - fw.w_ack)      > RISK_WEIGHT_SAVE_THRESHOLD) return true;

    return false;
}



bool Risk_SaveWeights(const RiskState_t *s)
{
    RiskWeightsFlash_t fw;

    if (s == NULL)
    {
        return false;
    }

    if (!Risk_WeightsChangedEnough(s))
        {
            PRINTF("SYS,WEIGHTS_SAVE_SKIP_NO_CHANGE\r\n");
            return true;
        }

    fw.w_missed   = s->w_missed;
    fw.w_reminder = s->w_reminder;
    fw.w_failure  = s->w_failure;
    fw.w_ack      = s->w_ack;
    fw.magic      = RISK_WEIGHTS_MAGIC;
    fw.crc        = 0U;
    fw.crc        = Risk_SimpleCrc(&fw);

    return FlashLogger_SaveRiskWeights(&fw);
}

bool Risk_LoadWeights(RiskState_t *s)
{
    RiskWeightsFlash_t fw;
    uint32_t crc;

    if (s == NULL)
    {
        return false;
    }

    if (!FlashLogger_LoadRiskWeights(&fw))
    {
        return false;
    }

    if (fw.magic != RISK_WEIGHTS_MAGIC)
    {
        return false;
    }

    crc = fw.crc;
    fw.crc = 0U;

    if (crc != Risk_SimpleCrc(&fw))
    {
        return false;
    }

    s->w_missed   = fw.w_missed;
    s->w_reminder = fw.w_reminder;
    s->w_failure  = fw.w_failure;
    s->w_ack      = fw.w_ack;

    return true;
}


static uint8_t clamp_u8(uint8_t x, uint8_t maxv)
{
    return (x > maxv) ? maxv : x;
}

static float clamp_f(float x, float lo, float hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static void Risk_PushRecord(RiskState_t *s, const RiskCycleRecord_t *r)
{
    s->hist[s->head] = *r;
    s->head = (uint8_t)((s->head + 1U) % RISK_HISTORY_LEN);
    if (s->count < RISK_HISTORY_LEN)
    {
        s->count++;
    }
}

void Risk_Init(RiskState_t *s)
{
    uint8_t i;

    for (i = 0U; i < RISK_HISTORY_LEN; i++)
    {
        s->hist[i].reminder_count = 0U;
        s->hist[i].dispense_failed = 0U;
        s->hist[i].dose_taken = 0U;
        s->hist[i].dose_missed = 0U;
        s->hist[i].weight_ack = 0U;
    }

    s->head = 0U;
    s->count = 0U;
    s->current_reminder_count = 0U;
    s->current_dispense_failed = 0U;
    s->current_weight_ack = 0U;
    s->missed_streak = 0U;
    s->risk_score = 0.0f;
    s->risk_level = RISK_LOW;

    /* initial weights */
    s->w_missed   = 0.40f;
    s->w_reminder = 0.20f;
    s->w_failure  = 0.20f;
    s->w_ack      = 0.20f;

//    (void)Risk_LoadWeights(s);
}

void Risk_OnReminderAttempt(RiskState_t *s, uint8_t attempts)
{
    s->current_reminder_count = attempts;
}

void Risk_OnDispenseVerify(RiskState_t *s, bool success)
{
    s->current_dispense_failed = success ? 0U : 1U;
}

void Risk_OnWeightAck(RiskState_t *s, bool acked)
{
    s->current_weight_ack = acked ? 1U : 0U;
}


//----------------------Improved this function using ChatGpt---------------------//


float Risk_ComputeScore(RiskState_t *s)
{
    uint8_t i;
    uint8_t reminder_sum = 0U;
    uint8_t failure_sum = 0U;
    uint8_t ack_sum = 0U;
    float missed_score;
    float reminder_score;
    float failure_score;
    float ack_score;
    float risk;
    float weight_sum;

    for (i = 0U; i < s->count; i++)
    {
        reminder_sum += s->hist[i].reminder_count;
        failure_sum += s->hist[i].dispense_failed;
        ack_sum += s->hist[i].weight_ack;
    }

    missed_score = (float)clamp_u8(s->missed_streak, 3U) / 3.0f;
    reminder_score = (float)clamp_u8(reminder_sum, 10U) / 10.0f;

    if (s->count > 0U)
    {
        failure_score = (float)failure_sum / (float)s->count;
        ack_score = 1.0f - ((float)ack_sum / (float)s->count);
    }
    else
    {
        failure_score = 0.0f;
        ack_score = 0.0f;
    }

    weight_sum = s->w_missed + s->w_reminder + s->w_failure + s->w_ack;
    if (weight_sum <= 0.0001f)
    {
        weight_sum = 1.0f;
    }

    risk = (s->w_missed   * missed_score +
            s->w_reminder * reminder_score +
            s->w_failure  * failure_score +
            s->w_ack      * ack_score) / weight_sum;

    risk = clamp_f(risk, 0.0f, 1.0f);
    return risk;
}

RiskLevel_t Risk_GetLevel(float score)
{
    if (score >= 0.70f) return RISK_HIGH;
    if (score >= 0.40f) return RISK_MEDIUM;
    return RISK_LOW;
}

uint8_t Risk_GetPercent(const RiskState_t *s)
{
    float score;
    uint8_t percent;

    if (s == 0)
    {
        return 0U;
    }

    score = s->risk_score;
    score = clamp_f(score, 0.0f, 1.0f);

    percent = (uint8_t)(score * 100.0f + 0.5f);
    return percent;
}



//--------------------Created using ChatGpt line 258 - 360------------------------//

void Risk_UpdateWeights(RiskState_t *s, bool taken, bool missed)
{
    float target;
    float error;
    float lr = 0.05f;

    uint8_t i;
    uint8_t reminder_sum = 0U;
    uint8_t failure_sum = 0U;
    uint8_t ack_sum = 0U;

    float missed_score;
    float reminder_score;
    float failure_score;
    float ack_score;
    float total;

    if (s == 0)
    {
        return;
    }

    for (i = 0U; i < s->count; i++)
    {
        reminder_sum += s->hist[i].reminder_count;
        failure_sum += s->hist[i].dispense_failed;
        ack_sum += s->hist[i].weight_ack;
    }

    missed_score = (float)clamp_u8(s->missed_streak, 3U) / 3.0f;
    reminder_score = (float)clamp_u8(reminder_sum, 10U) / 10.0f;

    if (s->count > 0U)
    {
        failure_score = (float)failure_sum / (float)s->count;
        ack_score = 1.0f - ((float)ack_sum / (float)s->count);
    }
    else
    {
        failure_score = 0.0f;
        ack_score = 0.0f;
    }

    target = missed ? 1.0f : 0.0f;
    error = target - s->risk_score;

    s->w_missed   += lr * error * missed_score;
    s->w_reminder += lr * error * reminder_score;
    s->w_failure  += lr * error * failure_score;
    s->w_ack      += lr * error * ack_score;

    s->w_missed   = clamp_f(s->w_missed,   0.05f, 0.70f);
    s->w_reminder = clamp_f(s->w_reminder, 0.05f, 0.50f);
    s->w_failure  = clamp_f(s->w_failure,  0.05f, 0.50f);
    s->w_ack      = clamp_f(s->w_ack,      0.05f, 0.50f);

    total = s->w_missed + s->w_reminder + s->w_failure + s->w_ack;
    if (total > 0.0001f)
    {
        s->w_missed   /= total;
        s->w_reminder /= total;
        s->w_failure  /= total;
        s->w_ack      /= total;
    }

    (void)taken;
}

void Risk_CloseDoseCycle(RiskState_t *s, bool taken, bool missed, bool weight_ack)
{
    RiskCycleRecord_t r;

    r.reminder_count  = s->current_reminder_count;
    r.dispense_failed = s->current_dispense_failed;
    r.dose_taken      = taken ? 1U : 0U;
    r.dose_missed     = missed ? 1U : 0U;
    r.weight_ack      = weight_ack ? 1U : 0U;

    Risk_PushRecord(s, &r);

    if (missed)
    {
        if (s->missed_streak < 255U)
        {
            s->missed_streak++;
        }
    }
    else if (taken)
    {
        s->missed_streak = 0U;
    }

    s->risk_score = Risk_ComputeScore(s);
    Risk_UpdateWeights(s, taken, missed);
    (void)Risk_SaveWeights(s);
    s->risk_score = Risk_ComputeScore(s);
    s->risk_level = Risk_GetLevel(s->risk_score);

    s->current_reminder_count = 0U;
    s->current_dispense_failed = 0U;
    s->current_weight_ack = 0U;
}

RiskPolicy_t Risk_GetPolicy(RiskLevel_t level)
{
    RiskPolicy_t p;

    switch (level)
    {
        case RISK_HIGH:
            p.buzzer_ms = 2000U;
            p.repeat_seconds = 5U;
            p.max_attempts = 4U;
            p.allow_retry_once = true;
            p.enhanced_assist = true;
            break;

        case RISK_MEDIUM:
            p.buzzer_ms = 1200U;
            p.repeat_seconds = 8U;
            p.max_attempts = 3U;
            p.allow_retry_once = true;
            p.enhanced_assist = false;
            break;

        case RISK_LOW:
        default:
            p.buzzer_ms = 700U;
            p.repeat_seconds = 10U;
            p.max_attempts = 3U;
            p.allow_retry_once = true;
            p.enhanced_assist = false;
            break;
    }

    return p;
}

void Risk_RefreshCurrentLevel(RiskState_t *s)
{
    if (s == NULL)
    {
        return;
    }

    s->risk_score = Risk_ComputeScore(s);
    s->risk_level = Risk_GetLevel(s->risk_score);
}




