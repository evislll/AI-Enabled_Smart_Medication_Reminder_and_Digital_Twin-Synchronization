#include "weight_task.h"
#include "weight.h"
#include "logging.h"
#include "hx711.h"

#include "fsl_debug_console.h"
#include "FreeRTOS.h"
#include "task.h"

#define WEIGHT_TASK_STACK_SIZE        768U
#define WEIGHT_TASK_PRIORITY          (tskIDLE_PRIORITY + 1U)
#define WEIGHT_TASK_PERIOD_MS         300U
#define WEIGHT_STARTUP_SETTLE_MS      5000U
#define WEIGHT_DISCARD_SAMPLES        30U
#define WEIGHT_TARE_SAMPLES           40U
#define WEIGHT_PRINT_DIVIDER          4U
#define DEFAULT_CALIBRATION_FACTOR    470.0f

static TaskHandle_t g_weight_task_handle = NULL;

bool Weight_Task_Init(void)
{
    BaseType_t result = xTaskCreate(Task_Weight,
                                    "WEIGHT",
                                    WEIGHT_TASK_STACK_SIZE,
                                    NULL,
                                    WEIGHT_TASK_PRIORITY,
                                    &g_weight_task_handle);

    return (result == pdPASS);
}


/////////////////////////////////////////////////////////////////////////////////
//-----------------following function is fixed using ChatGpt-------------------//
/////////////////////////////////////////////////////////////////////////////////


void Task_Weight(void *pvParameters)
{
    (void)pvParameters;

    int32_t raw = 0;
    uint8_t print_div = 0U;

    Logging_StreamSystem("WEIGHT_INIT");

    if (!Weight_Init())
    {
        Logging_StreamSystem("WEIGHT_INIT_FAIL");
        vTaskDelete(NULL);
    }

    Logging_StreamSystem("WEIGHT_INIT_OK");

    HX711_PowerDown();
    vTaskDelay(pdMS_TO_TICKS(20));
    HX711_PowerUp();

    Logging_StreamSystem("WEIGHT_SETTLING");
    vTaskDelay(pdMS_TO_TICKS(WEIGHT_STARTUP_SETTLE_MS));

    Logging_StreamSystem("WEIGHT_DISCARD_START");
    for (uint32_t i = 0; i < WEIGHT_DISCARD_SAMPLES; i++)
    {
        (void)HX711_ReadRaw(&raw);
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    Logging_StreamSystem("WEIGHT_DISCARD_DONE");

    Logging_StreamSystem("WEIGHT_TARE_START");
    if (!Weight_Tare(WEIGHT_TARE_SAMPLES))
    {
        PRINTF("WEIGHT_TARE_FAIL,READY=%d,DT=%lu,SCK=%lu\r\n",
               HX711_IsReady() ? 1 : 0,
               (unsigned long)HX711_GetDtState(),
               (unsigned long)HX711_GetSckState());

        Logging_StreamSystem("WEIGHT_TARE_FAIL");

        while (1)
        {
            PRINTF("HXDBG,READY=%d,DT=%lu,SCK=%lu\r\n",
                   HX711_IsReady() ? 1 : 0,
                   (unsigned long)HX711_GetDtState(),
                   (unsigned long)HX711_GetSckState());
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    Logging_StreamSystem("WEIGHT_TARE_OK");

    {
        WeightData tare_data;
        if (Weight_GetData(&tare_data))
        {
            PRINTF("WGT,TARE_OFFSET,%d\r\n", (int)tare_data.offset);
        }
    }

    Weight_SetCalibrationFactor(DEFAULT_CALIBRATION_FACTOR);
    Logging_StreamSystem("WEIGHT_READY");

    while (1)
    {
        if (Weight_Update())
        {
            WeightData data;

            if (Weight_GetData(&data) && data.valid)
            {
                print_div++;

                if (print_div >= WEIGHT_PRINT_DIVIDER)
                {
                	int32_t raw_s        = (int32_t)data.raw;
                	int32_t filt_s       = (int32_t)data.filtered_raw;
                	int32_t offset_s     = (int32_t)data.offset;
                	int32_t raw_net_s    = raw_s - offset_s;
                	int32_t net_s        = filt_s - offset_s;
                	int32_t inst_gx100   = (int32_t)(data.instant_grams * 100.0f);
                	int32_t stable_gx100 = (int32_t)(data.grams * 100.0f);

                	print_div = 0U;

                	PRINTF("WGT: , RAW: %ld, RAW_NET: %c%ld,FILT: %ld, NET: %c%ld, INST_x100: %c%ld, STBL_x100: %c%ld\r\n",
//                			(raw_s < 0) ? '-' : '+',
                			(long)raw_s,
                	       (raw_net_s < 0) ? '-' : '+',
                	       (long)labs(raw_net_s),
//						   (filt_s < 0) ? '-' : '+',
                	       (long)filt_s,
                	       (net_s < 0) ? '-' : '+',
                	       (long)labs(net_s),
                	       (inst_gx100 < 0) ? '-' : '+',
                	       (long)labs(inst_gx100),
                	       (stable_gx100 < 0) ? '-' : '+',
                	       (long)labs(stable_gx100));
                }
            }
        }
        else
        {
            Logging_StreamSystem("WEIGHT_READ_FAIL");
        }

        vTaskDelay(pdMS_TO_TICKS(WEIGHT_TASK_PERIOD_MS));
    }
}
