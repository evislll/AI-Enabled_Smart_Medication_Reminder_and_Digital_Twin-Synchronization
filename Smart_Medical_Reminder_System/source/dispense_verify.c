//dipense_verify.c
//dipense_verify.c
#include "dispense_verify.h"
#include "reminder.h"

#include <string.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "fsl_debug_console.h"

#include "servo.h"
#include "weight.h"
#include "logging.h"
#include "server_logger.h"
#include "risk.h"

/*
 * ============================================================
 * ADAPTER LAYER
 * ============================================================
 */

static bool Adapter_WeightIsReady(void)
{
    WeightData data;

    if (!Weight_GetData(&data))
    {
        return false;
    }

    return data.valid;
}

static bool Adapter_GetStableWeightG(float *out_g)
{
    if (out_g == NULL)
    {
        return false;
    }

    WeightData data;

    if (!Weight_GetData(&data))
    {
        return false;
    }

    if (!data.valid)
    {
        return false;
    }

    *out_g = data.grams;
    return true;
}

static bool Adapter_ServoDispenseOnce(void)
{
    Servo_DispenseCycle();
    return true;
}

static void Adapter_LogInfo(const char *msg)
{
    if (msg == NULL)
    {
        return;
    }

    PRINTF("SYS,%s\r\n", msg);
}

/*
 * ============================================================
 * MODULE STATE
 * ============================================================
 */

static DispenseVerifyConfig s_cfg;
static bool s_initialized = false;

/*
 * ============================================================
 * INTERNAL HELPERS
 * ============================================================
 */

static void SafeCopyString(char *dst, size_t dst_size, const char *src)
{
    if ((dst == NULL) || (dst_size == 0U))
    {
        return;
    }

    if (src == NULL)
    {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, dst_size - 1U);
    dst[dst_size - 1U] = '\0';
}


//--------------Generated using Chatgpt from line 109 - 163-----------------//


static void SetDefaultResult(DispenseVerifyResult *r)
{
    if (r == NULL)
    {
        return;
    }

    memset(r, 0, sizeof(*r));
    SafeCopyString(r->med_id, sizeof(r->med_id), "UNKNOWN");
    SafeCopyString(r->result, sizeof(r->result), "DISPENSE_ERROR");
    SafeCopyString(r->note, sizeof(r->note), "not executed");
    r->status = DISPENSE_VERIFY_ERR_NULL;
    r->success = false;
    r->pre_weight_g = 0.0f;
    r->post_weight_g = 0.0f;
    r->delta_g = 0.0f;
    r->threshold_g = s_cfg.threshold_g;
}

static bool WaitAndReadStableWeight(float *out_g, uint32_t timeout_ms)
{
    TickType_t start_ticks;
    TickType_t now_ticks;
    uint32_t elapsed_ms = 0U;

    if (out_g == NULL)
    {
        return false;
    }

    start_ticks = xTaskGetTickCount();

    for (;;)
    {
        if (Adapter_GetStableWeightG(out_g))
        {
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(s_cfg.stable_recheck_ms));

        now_ticks = xTaskGetTickCount();
        elapsed_ms = (uint32_t)((now_ticks - start_ticks) * portTICK_PERIOD_MS);

        if (elapsed_ms >= timeout_ms)
        {
            return false;
        }
    }
}




static void FinalizeResult(DispenseVerifyResult *r,
                           const char *med_id,
                           float pre_g,
                           float post_g,
                           DispenseVerifyStatus status,
                           const char *note)
{
    if (r == NULL)
    {
        return;
    }

    SafeCopyString(r->med_id, sizeof(r->med_id), (med_id != NULL) ? med_id : "UNKNOWN");
    SafeCopyString(r->note, sizeof(r->note), note);

    r->pre_weight_g = pre_g;
    r->post_weight_g = post_g;
    r->delta_g = post_g - pre_g;
    r->threshold_g = s_cfg.threshold_g;
    r->status = status;

    if (status == DISPENSE_VERIFY_OK)
    {
        if (r->delta_g >= r->threshold_g)
        {
            r->success = true;
            SafeCopyString(r->result, sizeof(r->result), "DISPENSE_SUCCESS");
        }
        else
        {
            r->success = false;
            SafeCopyString(r->result, sizeof(r->result), "DISPENSE_FAIL");
        }
    }
    else
    {
        r->success = false;
        SafeCopyString(r->result, sizeof(r->result), "DISPENSE_ERROR");
    }
}

/*
 * ============================================================
 * STEP 1 HELPER: retry once if risk >= MEDIUM
 * ============================================================
 */
static bool TryRetryOnceIfAllowed(const char *med_id, uint8_t *retry_used)
{
    RiskLevel_t active_level;

    if (retry_used == NULL)
    {
        return false;
    }

    if (*retry_used != 0U)
    {
        return false;
    }

    active_level = Reminder_GetActiveRiskLevel();

    if (active_level < RISK_MEDIUM)
    {
        return false;
    }

    *retry_used = 1U;

    PRINTF("SYS,RETRY_ATTEMPT,MED=%s,RISK=%d\r\n",
           (med_id != NULL) ? med_id : "UNKNOWN",
           (int)active_level);

    Adapter_LogInfo("DISPENSE_VERIFY_RETRY_ATTEMPT");

    vTaskDelay(pdMS_TO_TICKS(500));

    return true;
}

/*
 * ============================================================
 * PUBLIC API
 * ============================================================
 */

void DispenseVerify_Init(void)
{
    s_cfg.threshold_g = 0.30f;
    s_cfg.pre_stable_timeout_ms = 2000U;
    s_cfg.post_stable_timeout_ms = 3000U;
//    s_cfg.settle_delay_ms = 1500U;
    s_cfg.settle_delay_ms = 2500U;
    s_cfg.stable_recheck_ms = 100U;

    s_initialized = true;
}

bool DispenseVerify_SetConfig(const DispenseVerifyConfig *cfg)
{
    if (cfg == NULL)
    {
        return false;
    }

    s_cfg = *cfg;
    s_initialized = true;
    return true;
}

void DispenseVerify_GetConfig(DispenseVerifyConfig *out_cfg)
{
    if (out_cfg == NULL)
    {
        return;
    }

    if (!s_initialized)
    {
        DispenseVerify_Init();
    }

    *out_cfg = s_cfg;
}



//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//-------------------we wrote this function in skeleton form and then finally completed using chatgpt---------------//
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


bool DispenseVerify_Run(const char *med_id, DispenseVerifyResult *out)
{
    float pre_g = 0.0f;
    float post_g = 0.0f;
    uint8_t retry_used = 0U;

retry_attempt:

    if (!s_initialized)
    {
        DispenseVerify_Init();
    }

    if (out == NULL)
    {
        return false;
    }

    SetDefaultResult(out);
    SafeCopyString(out->med_id, sizeof(out->med_id), (med_id != NULL) ? med_id : "UNKNOWN");
    out->threshold_g = s_cfg.threshold_g;

    if (!Adapter_WeightIsReady())
    {
        FinalizeResult(out,
                       med_id,
                       0.0f,
                       0.0f,
                       DISPENSE_VERIFY_ERR_NOT_READY,
                       "weight subsystem not ready");
        Adapter_LogInfo("DISPENSE_VERIFY_WEIGHT_NOT_READY");

        if (ServerLogger_SendDispenseVerification(out))
        {
            Adapter_LogInfo("DISPENSE_VERIFY_SERVER_LOG_OK");
        }
        else
        {
            Adapter_LogInfo("DISPENSE_VERIFY_SERVER_LOG_FAIL");
        }

        return false;
    }

    Adapter_LogInfo("DISPENSE_VERIFY_PRE_CAPTURE_START");

    if (!WaitAndReadStableWeight(&pre_g, s_cfg.pre_stable_timeout_ms))
    {
        FinalizeResult(out,
                       med_id,
                       0.0f,
                       0.0f,
                       DISPENSE_VERIFY_ERR_PRE_STABLE_TIMEOUT,
                       "pre stable weight timeout");
        Adapter_LogInfo("DISPENSE_VERIFY_PRE_TIMEOUT");

        if (ServerLogger_SendDispenseVerification(out))
        {
            Adapter_LogInfo("DISPENSE_VERIFY_SERVER_LOG_OK");
        }
        else
        {
            Adapter_LogInfo("DISPENSE_VERIFY_SERVER_LOG_FAIL");
        }

        return false;
    }

    Adapter_LogInfo("DISPENSE_VERIFY_PRE_CAPTURE_OK");

    if (!Adapter_ServoDispenseOnce())
    {
        FinalizeResult(out,
                       med_id,
                       pre_g,
                       pre_g,
                       DISPENSE_VERIFY_ERR_SERVO_FAIL,
                       "servo dispense failed");
        Adapter_LogInfo("DISPENSE_VERIFY_SERVO_FAIL");

        if (ServerLogger_SendDispenseVerification(out))
        {
            Adapter_LogInfo("DISPENSE_VERIFY_SERVER_LOG_OK");
        }
        else
        {
            Adapter_LogInfo("DISPENSE_VERIFY_SERVER_LOG_FAIL");
        }

        return false;
    }

    Adapter_LogInfo("DISPENSE_VERIFY_SERVO_OK");

    vTaskDelay(pdMS_TO_TICKS(s_cfg.settle_delay_ms));

    Adapter_LogInfo("DISPENSE_VERIFY_POST_CAPTURE_START");

    if (!WaitAndReadStableWeight(&post_g, s_cfg.post_stable_timeout_ms))
    {
        FinalizeResult(out,
                       med_id,
                       pre_g,
                       pre_g,
                       DISPENSE_VERIFY_ERR_POST_STABLE_TIMEOUT,
                       "post stable weight timeout");

        Adapter_LogInfo("DISPENSE_VERIFY_POST_TIMEOUT");

        if (ServerLogger_SendDispenseVerification(out))
        {
            Adapter_LogInfo("DISPENSE_VERIFY_SERVER_LOG_OK");
        }
        else
        {
            Adapter_LogInfo("DISPENSE_VERIFY_SERVER_LOG_FAIL");
        }

        return false;
    }

    FinalizeResult(out,
                   med_id,
                   pre_g,
                   post_g,
                   DISPENSE_VERIFY_OK,
                   "verification complete");

    /* ✅ CORRECT PLACE — after normal verification */
    if (!out->success)
    {
        float confirm_g = 0.0f;

        vTaskDelay(pdMS_TO_TICKS(1000));

        if (Adapter_GetStableWeightG(&confirm_g))
        {
            out->post_weight_g = confirm_g;
            out->delta_g = confirm_g - pre_g;

            if (out->delta_g >= out->threshold_g)
            {
                out->success = true;
                SafeCopyString(out->result, sizeof(out->result), "DISPENSE_SUCCESS");
                SafeCopyString(out->note, sizeof(out->note), "delayed settle success");
            }
        }
    }

    if (out->success)
    {
        Adapter_LogInfo("DISPENSE_VERIFY_SUCCESS");
    }
    else
    {
        Adapter_LogInfo("DISPENSE_VERIFY_FAIL");

        if (TryRetryOnceIfAllowed(med_id, &retry_used))
        {
            goto retry_attempt;
        }
    }

    if (ServerLogger_SendDispenseVerification(out))
    {
        Adapter_LogInfo("DISPENSE_VERIFY_SERVER_LOG_OK");
    }
    else
    {
        Adapter_LogInfo("DISPENSE_VERIFY_SERVER_LOG_FAIL");
    }

    return true;
}






const char *DispenseVerify_StatusToString(DispenseVerifyStatus status)
{
    switch (status)
    {
        case DISPENSE_VERIFY_OK:
            return "OK";

        case DISPENSE_VERIFY_ERR_NULL:
            return "NULL";

        case DISPENSE_VERIFY_ERR_NOT_READY:
            return "NOT_READY";

        case DISPENSE_VERIFY_ERR_PRE_STABLE_TIMEOUT:
            return "PRE_STABLE_TIMEOUT";

        case DISPENSE_VERIFY_ERR_POST_STABLE_TIMEOUT:
            return "POST_STABLE_TIMEOUT";

        case DISPENSE_VERIFY_ERR_SERVO_FAIL:
            return "SERVO_FAIL";

        default:
            return "UNKNOWN";
    }
}




