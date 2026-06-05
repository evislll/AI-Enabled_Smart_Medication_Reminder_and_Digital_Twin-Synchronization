#include "servo.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "fsl_common.h"
#include "fsl_clock.h"
#include "fsl_debug_console.h"
#include "fsl_ftm.h"
#include "fsl_port.h"

#include "FreeRTOS.h"
#include "task.h"

#include "logging.h"
#include "rtc.h"

/*
 * FRDM-K66F
 * Servo signal pin: PTC8 (Arduino D3)
 * Assumed mux: PTC8 -> FTM3_CH4
 *
 * SG90:
 *   50 Hz period = 20 ms
 *   Pulse range tuned for wider motion
 */

#define SERVO_PORT              PORTC
#define SERVO_PIN               8U

#define SERVO_FTM_BASE          FTM3
#define SERVO_FTM_CHANNEL       kFTM_Chnl_4
#define SERVO_FTM_CHANNEL_NUM   4U

#define SERVO_PWM_FREQ_HZ       50U
#define SERVO_PERIOD_US         20000U

#define SERVO_MIN_PULSE_US      700U
#define SERVO_MAX_PULSE_US      2400U

#define SERVO_HOME_ANGLE        0U
#define SERVO_DISPENSE_ANGLE    100U

#define SERVO_LOG_MED_ID            "SERVO"
#define SERVO_FAIL_NOT_INITIALIZED  (-1)
#define SERVO_SUCCESS_CYCLE_DONE    (1)

static uint32_t s_ftmSourceClockHz = 0U;
static uint32_t s_ftmModValue = 0U;
static bool s_servoInitialized = false;
static int16_t s_lastServoAngle = -1;

//----------------TWIN--------------------//

static ServoState s_servoState = SERVO_STATE_UNKNOWN;

//----------------TWIN-------------------//

static uint32_t Servo_AngleToPulseUs(uint8_t angle)
{
    if (angle > 180U)
    {
        angle = 180U;
    }

    return SERVO_MIN_PULSE_US +
           (((uint32_t)angle * (SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US)) / 180U);
}

/////////////////////////////////////////////////////////////////////
//------------------line 75- 97 created using ChatGpt--------------//
/////////////////////////////////////////////////////////////////////


static uint32_t Servo_PulseUsToCnV(uint32_t pulseUs)
{
    uint64_t cnv = ((uint64_t)pulseUs * (uint64_t)(s_ftmModValue + 1U)) / SERVO_PERIOD_US;

    if (cnv > s_ftmModValue)
    {
        cnv = s_ftmModValue;
    }

    return (uint32_t)cnv;
}

static void Servo_ApplyCnV(uint32_t cnv)
{
    FTM_UpdateChnlEdgeLevelSelect(SERVO_FTM_BASE,
                                  SERVO_FTM_CHANNEL,
                                  kFTM_HighTrue);

    SERVO_FTM_BASE->CONTROLS[SERVO_FTM_CHANNEL_NUM].CnV = cnv;

    FTM_SetSoftwareTrigger(SERVO_FTM_BASE, true);
}

static void Servo_PinInit(void)
{
    CLOCK_EnableClock(kCLOCK_PortC);

    port_pin_config_t portConfig = {
        .pullSelect = kPORT_PullDisable,
        .slewRate = kPORT_FastSlewRate,
        .passiveFilterEnable = kPORT_PassiveFilterDisable,
        .openDrainEnable = kPORT_OpenDrainDisable,
        .driveStrength = kPORT_LowDriveStrength,
        .mux = kPORT_MuxAlt3,
        .lockRegister = kPORT_UnlockRegister
    };

    PORT_SetPinConfig(SERVO_PORT, SERVO_PIN, &portConfig);
}

static void Servo_LogEvent(LogType type, DoseStatus status, int16_t value, int16_t extra)
{

    RtcTime now;

    uint16_t year = 0;
    uint8_t month = 0;
    uint8_t day = 0;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;

    if (RTC_GetTime(&now))
    {
        hour = now.hour;
        minute = now.minute;
        second = now.second;
        year = now.year;
            month = now.month;
            day = now.day;

    }

    (void)Logging_LogSimpleEvent(type,
                                 SERVO_LOG_MED_ID,
                                 status,
                                 0U,
                                 0U,
								 year,
								 month,
								 day,
                                 hour,
                                 minute,
                                 second,
                                 value,
                                 extra);
}



/////////////////////////////////////////////////////////////////////////////////
//---------------following function is Created using ChatGpt-------------------//
/////////////////////////////////////////////////////////////////////////////////



bool Servo_Init(void)
{
    ftm_config_t ftmInfo;
    ftm_chnl_pwm_signal_param_t pwmParam;
    status_t status;

    Servo_PinInit();

    CLOCK_EnableClock(kCLOCK_Ftm3);

    s_ftmSourceClockHz = CLOCK_GetFreq(kCLOCK_BusClk);

    FTM_GetDefaultConfig(&ftmInfo);
    ftmInfo.prescale = kFTM_Prescale_Divide_128;
    ftmInfo.bdmMode = kFTM_BdmMode_3;
    ftmInfo.reloadPoints = 0U;
    ftmInfo.pwmSyncMode = kFTM_SoftwareTrigger;
    ftmInfo.extTriggers = 0U;

    FTM_Init(SERVO_FTM_BASE, &ftmInfo);

    pwmParam.chnlNumber = SERVO_FTM_CHANNEL;
    pwmParam.level = kFTM_HighTrue;
    pwmParam.dutyCyclePercent = 7U;
    pwmParam.firstEdgeDelayPercent = 0U;

    status = FTM_SetupPwm(SERVO_FTM_BASE,
                          &pwmParam,
                          1U,
                          kFTM_EdgeAlignedPwm,
                          SERVO_PWM_FREQ_HZ,
                          s_ftmSourceClockHz);

    if (status != kStatus_Success)
    {
        return false;
    }

    s_ftmModValue = SERVO_FTM_BASE->MOD;

    FTM_StartTimer(SERVO_FTM_BASE, kFTM_SystemClock);

    s_servoInitialized = true;

    //----------TWIN-------------//
//    s_lastServoAngle = SERVO_HOME_ANGLE;
    s_lastServoAngle = SERVO_HOME_ANGLE;
    s_servoState = SERVO_STATE_HOME;
    //----------TWIN------------//
    return true;
}


/////////////////////////////////////////////////////////////////////////////////
//---------------following function is fixed using ChatGpt-------------------//
/////////////////////////////////////////////////////////////////////////////////



void Servo_SetAngle(uint8_t angle)
{
    uint32_t pulseUs;
    uint32_t cnv;

    if (!s_servoInitialized)
    {
        return;
    }

    pulseUs = Servo_AngleToPulseUs(angle);
    cnv = Servo_PulseUsToCnV(pulseUs);

    Servo_ApplyCnV(cnv);
    s_lastServoAngle = (int16_t)angle;
    if (angle == SERVO_HOME_ANGLE)
    {
        s_servoState = SERVO_STATE_HOME;
    }
}


/////////////////////////////////////////////////////////////////////////////////
//---------------following function is fixed using ChatGpt-------------------//
/////////////////////////////////////////////////////////////////////////////////


void Servo_DispenseCycle(void)
{
    if (!s_servoInitialized)
    {
        PRINTF("Dispense failed: servo not initialized\r\n");
        Servo_LogEvent(LOG_TYPE_DISPENSE_FAIL,
                       DOSE_MISSED,
                       SERVO_FAIL_NOT_INITIALIZED,
                       0);
        return;
    }

    Servo_LogEvent(LOG_TYPE_DISPENSE_START,
                   DOSE_PENDING,
                   SERVO_DISPENSE_ANGLE,
                   0);

    PRINTF("Dispense: HOME\r\n");
    s_servoState = SERVO_STATE_HOME;
    Servo_SetAngle(SERVO_HOME_ANGLE);
    vTaskDelay(pdMS_TO_TICKS(800));

    PRINTF("Dispense: ROTATE\r\n");
    s_servoState = SERVO_STATE_DISPENSING;
    Servo_SetAngle(SERVO_DISPENSE_ANGLE);
    vTaskDelay(pdMS_TO_TICKS(1000));

    PRINTF("Dispense: RETURN\r\n");
    s_servoState = SERVO_STATE_RETURNING;
    Servo_SetAngle(SERVO_HOME_ANGLE);
    vTaskDelay(pdMS_TO_TICKS(800));

    s_servoState = SERVO_STATE_HOME;

    /*
     * Important:
     * Mechanical servo motion success is NOT the same as medication taken.
     * Actual TAKEN/MISSED is decided later by weight-based tray verification.
     */
    Servo_LogEvent(LOG_TYPE_DISPENSE_SUCCESS,
                   DOSE_PENDING,
                   SERVO_SUCCESS_CYCLE_DONE,
                   SERVO_DISPENSE_ANGLE);
}



int16_t Servo_GetLastAngle(void)
{
    return s_lastServoAngle;
}




//-------TWIN--------------------//

ServoState Servo_GetState(void)
{
    return s_servoState;
}

const char *Servo_GetStateString(void)
{
    switch (s_servoState)
    {
        case SERVO_STATE_HOME:
            return "HOME";

        case SERVO_STATE_DISPENSING:
            return "DISPENSING";

        case SERVO_STATE_RETURNING:
            return "RETURNING";

        case SERVO_STATE_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}

//---------------TWIN----------------//

