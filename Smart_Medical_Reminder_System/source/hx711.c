#include "hx711.h"

#include "fsl_common.h"
#include "fsl_port.h"
#include "fsl_gpio.h"

#include "FreeRTOS.h"
#include "task.h"

#define HX711_DT_GPIO   GPIOE
#define HX711_DT_PORT   PORTE
#define HX711_DT_PIN    7U

#define HX711_SCK_GPIO  GPIOE
#define HX711_SCK_PORT  PORTE
#define HX711_SCK_PIN   8U

#define HX711_READY_TIMEOUT_MS  1000U

static bool g_hx711_initialized = false;

static inline void HX711_SCK_High(void)
{
    GPIO_PinWrite(HX711_SCK_GPIO, HX711_SCK_PIN, 1U);
}

static inline void HX711_SCK_Low(void)
{
    GPIO_PinWrite(HX711_SCK_GPIO, HX711_SCK_PIN, 0U);
}

static inline uint32_t HX711_DT_Read(void)
{
    return GPIO_PinRead(HX711_DT_GPIO, HX711_DT_PIN);
}

/* Slower delay for safer HX711 bit-banged timing */
static inline void HX711_ShortDelay(void)
{
    for (volatile int i = 0; i < 100; i++)
    {
        __NOP();
    }
}


//////////////////////////////////////////////////////////////////////////////////
//-----------------ChatGpt created function from line 47-147--------------------//
//////////////////////////////////////////////////////////////////////////////////



bool HX711_Init(void)
{
    gpio_pin_config_t dt_config = {kGPIO_DigitalInput, 0};
    gpio_pin_config_t sck_config = {kGPIO_DigitalOutput, 0};

    CLOCK_EnableClock(kCLOCK_PortE);

    port_pin_config_t dt_port_config = {
        .pullSelect = kPORT_PullDisable,   /* if your SDK differs, adjust this enum only */
        .slewRate = kPORT_FastSlewRate,
        .passiveFilterEnable = kPORT_PassiveFilterDisable,
        .openDrainEnable = kPORT_OpenDrainDisable,
        .driveStrength = kPORT_LowDriveStrength,
        .mux = kPORT_MuxAsGpio,
        .lockRegister = kPORT_UnlockRegister
    };

    PORT_SetPinConfig(HX711_DT_PORT, HX711_DT_PIN, &dt_port_config);
    PORT_SetPinMux(HX711_SCK_PORT, HX711_SCK_PIN, kPORT_MuxAsGpio);

    GPIO_PinInit(HX711_DT_GPIO, HX711_DT_PIN, &dt_config);
    GPIO_PinInit(HX711_SCK_GPIO, HX711_SCK_PIN, &sck_config);

    HX711_SCK_Low();

    g_hx711_initialized = true;
    return true;
}

bool HX711_IsReady(void)
{
    if (!g_hx711_initialized)
    {
        return false;
    }

    /* HX711 ready when DOUT goes LOW */
    return (HX711_DT_Read() == 0U);
}

bool HX711_ReadRaw(int32_t *value)
{
    if ((!g_hx711_initialized) || (value == NULL))
    {
        return false;
    }

    TickType_t start_tick = xTaskGetTickCount();

    while (!HX711_IsReady())
    {
        if ((xTaskGetTickCount() - start_tick) > pdMS_TO_TICKS(HX711_READY_TIMEOUT_MS))
        {
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    uint32_t data = 0U;

    taskENTER_CRITICAL();
    {
        /* Read 24 bits, MSB first */
        for (int i = 0; i < 24; i++)
        {
            HX711_SCK_High();
            HX711_ShortDelay();
            HX711_ShortDelay();

            data = (data << 1U) | (HX711_DT_Read() ? 1U : 0U);

            HX711_SCK_Low();
            HX711_ShortDelay();
            HX711_ShortDelay();
        }

        /* 25th pulse selects Channel A, Gain 128 */
        HX711_SCK_High();
        HX711_ShortDelay();
        HX711_ShortDelay();
        HX711_SCK_Low();
        HX711_ShortDelay();
        HX711_ShortDelay();
    }
    taskEXIT_CRITICAL();

    /* Sign-extend 24-bit two's complement to 32-bit signed */
    if (data & 0x800000UL)
    {
        data |= 0xFF000000UL;
    }

    *value = (int32_t)data;
    return true;
}

void HX711_PowerDown(void)
{
    if (!g_hx711_initialized)
    {
        return;
    }

    HX711_SCK_Low();
    HX711_SCK_High();

    /* Keep SCK high > 60 us to power down HX711 */
    vTaskDelay(pdMS_TO_TICKS(1));
}

void HX711_PowerUp(void)
{
    if (!g_hx711_initialized)
    {
        return;
    }

    HX711_SCK_Low();
    vTaskDelay(pdMS_TO_TICKS(1));
}

uint32_t HX711_GetDtState(void)
{
    return HX711_DT_Read();
}

uint32_t HX711_GetSckState(void)
{
    return GPIO_PinRead(HX711_SCK_GPIO, HX711_SCK_PIN);
}












