// alert.c


#include "alert.h"
#include "fsl_gpio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "fsl_port.h"
#include "fsl_clock.h"
#include "fsl_ftm.h"
#include "fsl_debug_console.h"


// =====================
// USER CONFIG
// =====================




// --- Vibration motor GPIO pin ---
#define VIBE_GPIO        GPIOC
#define VIBE_PORT        PORTC
#define VIBE_PIN         16U      // PTC16

// --- Buzzer PWM (FTM) ---
#define BUZZ_FTM         FTM0
#define BUZZ_FTM_CH      1U       // FTM0_CH1 on PTC2
#define BUZZ_FTM_CLK_EN()  CLOCK_EnableClock(kCLOCK_Ftm0)
#define BUZZ_FREQ_HZ     2000U    // 2kHz tone
#define BUZZ_DUTY_PCT    50U




/////////////////////////////////////////////////////////////////////////////////////////////////////////////
//---------used copilot to figure out how to use the functions created in fsl_gpio.h and fsl_ftm.h---------//
//---------like stoptimer, pinwrite etc.-------------------------------------------------------------------//
/////////////////////////////////////////////////////////////////////////////////////////////////////////////



static bool s_buzz_inited = false;

static void buzzer_pwm_init_if_needed(void)
{
    if (s_buzz_inited) return;

    BUZZ_FTM_CLK_EN();

    ftm_config_t cfg;
    FTM_GetDefaultConfig(&cfg);


    FTM_Init(BUZZ_FTM, &cfg);

    // Edge-aligned PWM setup
    ftm_chnl_pwm_signal_param_t pwm;
    pwm.chnlNumber           = (ftm_chnl_t)BUZZ_FTM_CH;
    pwm.level                = kFTM_HighTrue;
    pwm.dutyCyclePercent     = BUZZ_DUTY_PCT;
    pwm.firstEdgeDelayPercent = 0U;

    uint32_t ftmClockHz = CLOCK_GetFreq(kCLOCK_BusClk);

    // Setup PWM. The "mode" is edge-aligned, and we’re setting frequency + duty.
    FTM_SetupPwm(BUZZ_FTM,
                 &pwm,
                 1U,
                 kFTM_EdgeAlignedPwm,
                 BUZZ_FREQ_HZ,
                 ftmClockHz);

    // Keep stopped by default
    FTM_StopTimer(BUZZ_FTM);

    s_buzz_inited = true;
}

static void buzzer_pwm_start(void)
{
    buzzer_pwm_init_if_needed();

    // Start the FTM counter (clock source = system clock)
    FTM_StartTimer(BUZZ_FTM, kFTM_SystemClock);
}

static void buzzer_pwm_stop(void)
{
    if (!s_buzz_inited) return;
    FTM_StopTimer(BUZZ_FTM);


}

bool Alert_Init(void)
{
    // Vibration motor GPIO out
	CLOCK_EnableClock(kCLOCK_PortC);
	    PORT_SetPinMux(PORTC, 16U, kPORT_MuxAsGpio);
    gpio_pin_config_t motorCfg = {
        .pinDirection = kGPIO_DigitalOutput,
        .outputLogic  = 0U
    };
    GPIO_PinInit(VIBE_GPIO, VIBE_PIN, &motorCfg);


    PORT_SetPinMux(PORTC, 2U, kPORT_MuxAsGpio);
    gpio_pin_config_t buzzCfg = {
        .pinDirection = kGPIO_DigitalOutput,
        .outputLogic  = 1U
    };
    GPIO_PinInit(GPIOC, 2U, &buzzCfg);

    return true;
}

void Alert_StartPattern(void)
{

	PRINTF("Motor On\r\n");
    GPIO_PinWrite(VIBE_GPIO, VIBE_PIN, 1U);
    GPIO_PinWrite(GPIOC, 2U, 0U);  // buzzer ON

//    buzzer_pwm_start();
}

void Alert_Stop(void)
{
	PRINTF("Motor OFF\r\n");
    GPIO_PinWrite(VIBE_GPIO, VIBE_PIN, 0U);
    GPIO_PinWrite(GPIOC, 2U, 1U);  // buzzer OFF
//    buzzer_pwm_stop();
}


void Alert_SirenPattern(void)
{
    // buzzer ON (active-LOW)
    GPIO_PinWrite(GPIOC, 2U, 0U);

    // motor pulse 1
    GPIO_PinWrite(GPIOC, 16U, 1U);
    vTaskDelay(pdMS_TO_TICKS(300));

    GPIO_PinWrite(GPIOC, 16U, 0U);
    vTaskDelay(pdMS_TO_TICKS(400));

    // motor pulse 2 (longer)
    GPIO_PinWrite(GPIOC, 16U, 1U);
    vTaskDelay(pdMS_TO_TICKS(600));

    GPIO_PinWrite(GPIOC, 16U, 0U);
    vTaskDelay(pdMS_TO_TICKS(500));

    // buzzer OFF
    GPIO_PinWrite(GPIOC, 2U, 1U);

    vTaskDelay(pdMS_TO_TICKS(1000));
}
