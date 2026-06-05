#include "weight.h"
#include "hx711.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#define WEIGHT_FILTER_SIZE             5U
#define WEIGHT_SOFT_LIMIT_COUNTS       15000
#define WEIGHT_OUTLIER_LIMIT_COUNTS    80000
#define WEIGHT_DEADBAND_GRAMS          0.2f
#define WEIGHT_STABLE_TOL_GRAMS        0.4f
#define WEIGHT_STABLE_REQUIRED_COUNT   2U

typedef struct
{
    int32_t raw;
    int32_t filtered_raw;
    int32_t offset;
    float calibration_factor;

    float grams;           /* stable display grams */
    float instant_grams;   /* latest filtered grams */

    bool valid;
} WeightState;

static WeightState g_state;
static SemaphoreHandle_t g_weight_mutex = NULL;
static bool g_initialized = false;

static int32_t g_filter_buffer[WEIGHT_FILTER_SIZE];
static uint8_t g_filter_index = 0U;
static uint8_t g_filter_count = 0U;
static int64_t g_filter_sum = 0;

static int32_t g_last_accepted_raw = 0;
static bool g_has_last_raw = false;

static float g_candidate_grams = 0.0f;
static uint8_t g_stable_count = 0U;
static bool g_has_candidate = false;



//---------twin override-------//

static bool g_twin_override_active = false;
static float g_twin_override_grams = 0.0f;



////////////////////////////////////////////////////////////////////////////////////////////////////
//---------------this file is fixed using chat gpt after digital twin was added-------------------//
////////////////////////////////////////////////////////////////////////////////////////////////////



static void Weight_FilterReset(void)
{
    memset(g_filter_buffer, 0, sizeof(g_filter_buffer));
    g_filter_index = 0U;
    g_filter_count = 0U;
    g_filter_sum = 0;
}

static int32_t Weight_FilterPush(int32_t sample)
{
    g_filter_sum -= g_filter_buffer[g_filter_index];
    g_filter_buffer[g_filter_index] = sample;
    g_filter_sum += sample;

    g_filter_index++;
    if (g_filter_index >= WEIGHT_FILTER_SIZE)
    {
        g_filter_index = 0U;
    }

    if (g_filter_count < WEIGHT_FILTER_SIZE)
    {
        g_filter_count++;
    }

    return (int32_t)(g_filter_sum / (int32_t)g_filter_count);
}

static void Weight_FilterPrime(int32_t sample)
{
    Weight_FilterReset();

    for (uint8_t i = 0U; i < WEIGHT_FILTER_SIZE; i++)
    {
        (void)Weight_FilterPush(sample);
    }
}

static void Weight_StableReset(void)
{
    g_candidate_grams = 0.0f;
    g_stable_count = 0U;
    g_has_candidate = false;
}

bool Weight_Init(void)
{
    if (!HX711_Init())
    {
        return false;
    }

    if (g_weight_mutex == NULL)
    {
        g_weight_mutex = xSemaphoreCreateMutex();
        if (g_weight_mutex == NULL)
        {
            return false;
        }
    }

    Weight_FilterReset();
    Weight_StableReset();

    g_last_accepted_raw = 0;
    g_has_last_raw = false;

    if (xSemaphoreTake(g_weight_mutex, portMAX_DELAY) == pdTRUE)
    {
        g_state.raw = 0;
        g_state.filtered_raw = 0;
        g_state.offset = 0;
        g_state.calibration_factor = 1000.0f;   /* replace after real calibration */
        g_state.grams = 0.0f;
        g_state.instant_grams = 0.0f;
        g_state.valid = false;
        xSemaphoreGive(g_weight_mutex);
    }

    g_initialized = true;
    return true;
}

bool Weight_Tare(uint16_t sample_count)
{
    if ((!g_initialized) || (sample_count == 0U))
    {
        return false;
    }

    int64_t sum = 0;
    uint16_t good_count = 0;
    uint16_t attempts = 0;
    int32_t raw = 0;

    Weight_FilterReset();
    Weight_StableReset();

    g_last_accepted_raw = 0;
    g_has_last_raw = false;

    while ((good_count < sample_count) && (attempts < (sample_count * 10U)))
    {
        attempts++;

        if (HX711_ReadRaw(&raw))
        {
            if ((!g_has_last_raw) ||
                (abs(raw - g_last_accepted_raw) <= WEIGHT_OUTLIER_LIMIT_COUNTS))
            {
                sum += raw;
                good_count++;
                g_last_accepted_raw = raw;
                g_has_last_raw = true;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (good_count < sample_count)
    {
        return false;
    }

    if (xSemaphoreTake(g_weight_mutex, portMAX_DELAY) == pdTRUE)
    {
        g_state.offset = (int32_t)(sum / (int32_t)good_count);
        g_state.raw = 0;
        g_state.filtered_raw = g_state.offset;
        g_state.grams = 0.0f;
        g_state.instant_grams = 0.0f;
        g_state.valid = false;
        xSemaphoreGive(g_weight_mutex);
    }

    /* Start moving average at tare baseline instead of zeros */
    Weight_FilterPrime((int32_t)(sum / (int32_t)good_count));
    Weight_StableReset();

    return true;
}

bool Weight_Update(void)
{
    if (!g_initialized)
    {
        return false;
    }

    int32_t raw = 0;
    if (!HX711_ReadRaw(&raw))
    {
        if (xSemaphoreTake(g_weight_mutex, portMAX_DELAY) == pdTRUE)
        {
            g_state.valid = false;
            xSemaphoreGive(g_weight_mutex);
        }
        return false;
    }

    if (g_has_last_raw)
    {
        int32_t diff = abs(raw - g_last_accepted_raw);

        /* Hard reject impossible jump */
        if (diff > WEIGHT_OUTLIER_LIMIT_COUNTS)
        {
//            if (xSemaphoreTake(g_weight_mutex, portMAX_DELAY) == pdTRUE)
//            {
//                g_state.valid = false;
//                xSemaphoreGive(g_weight_mutex);
//            }
            return true;
        }

        /* Soft damp medium jump */
        if (diff > WEIGHT_SOFT_LIMIT_COUNTS)
        {
            raw = (raw + g_last_accepted_raw) / 2;
        }
    }

    g_last_accepted_raw = raw;
    g_has_last_raw = true;

    int32_t filtered = Weight_FilterPush(raw);

    if (xSemaphoreTake(g_weight_mutex, portMAX_DELAY) == pdTRUE)
    {
        float instant_grams =
            ((float)(filtered - g_state.offset)) / g_state.calibration_factor;
        if (instant_grams < 0.0f && instant_grams > -1.0f)
        {
            instant_grams = 0.0f;
        }

        g_state.raw = raw;
        g_state.filtered_raw = filtered;
        g_state.instant_grams = instant_grams;

        /* Deadband relative to current displayed grams */
        if (fabsf(instant_grams - g_state.grams) <= WEIGHT_DEADBAND_GRAMS)
        {
            Weight_StableReset();
            g_state.valid = true;
            xSemaphoreGive(g_weight_mutex);
            return true;
        }

        /* Stable candidate logic */
        if (!g_has_candidate)
        {
            g_candidate_grams = instant_grams;
            g_stable_count = 1U;
            g_has_candidate = true;
        }
        else
        {
            if (fabsf(instant_grams - g_candidate_grams) <= WEIGHT_STABLE_TOL_GRAMS)
            {
                g_stable_count++;
            }
            else
            {
                g_candidate_grams = instant_grams;
                g_stable_count = 1U;
            }
        }

        if (g_stable_count >= WEIGHT_STABLE_REQUIRED_COUNT)
        {
            g_state.grams = g_candidate_grams;
            Weight_StableReset();
        }

        g_state.valid = true;
        xSemaphoreGive(g_weight_mutex);
    }

    return true;
}

void Weight_SetCalibrationFactor(float factor)
{
    if ((!g_initialized) || (factor == 0.0f))
    {
        return;
    }

    if (xSemaphoreTake(g_weight_mutex, portMAX_DELAY) == pdTRUE)
    {
        g_state.calibration_factor = factor;
        xSemaphoreGive(g_weight_mutex);
    }
}

float Weight_GetCalibrationFactor(void)
{
    float factor = 1.0f;

    if (!g_initialized)
    {
        return factor;
    }

    if (xSemaphoreTake(g_weight_mutex, portMAX_DELAY) == pdTRUE)
    {
        factor = g_state.calibration_factor;
        xSemaphoreGive(g_weight_mutex);
    }

    return factor;
}

bool Weight_GetData(WeightData *out)
{
    if ((!g_initialized) || (out == NULL))
    {
        return false;
    }

    if (xSemaphoreTake(g_weight_mutex, portMAX_DELAY) == pdTRUE)
    {
        out->raw = g_state.raw;
        out->filtered_raw = g_state.filtered_raw;
        out->offset = g_state.offset;
        out->calibration_factor = g_state.calibration_factor;
//        out->grams = g_state.grams;    /* stable displayed grams */

        //--twin----//
        if (g_twin_override_active)
        {
            out->grams = g_twin_override_grams;
        }
        else
        {
            out->grams = g_state.grams;
        }

        out->instant_grams = g_state.instant_grams;/* latest filtered grams */
        out->valid = g_state.valid;
        xSemaphoreGive(g_weight_mutex);
        return true;
    }

    return false;
}




float Weight_ComputeCalibrationFactor(int32_t raw_with_mass,
                                      int32_t offset,
                                      float known_mass_grams)
{
    if (known_mass_grams == 0.0f)
    {
        return 1000.0f;
    }

    return ((float)(raw_with_mass - offset)) / known_mass_grams;
}




int16_t Weight_GetLastReading(void)
{
    WeightData data;

    if (!Weight_GetData(&data) || !data.valid)
    {
        return -1;
    }

    return (int16_t)(data.grams + ((data.grams >= 0.0f) ? 0.5f : -0.5f));
}



//-------------TWIN----------------//


bool Weight_SetTwinOverride(float grams)
{
    if (!g_initialized)
    {
        return false;
    }

    if (xSemaphoreTake(g_weight_mutex, portMAX_DELAY) == pdTRUE)
    {
        g_twin_override_grams = grams;
        g_twin_override_active = true;
        xSemaphoreGive(g_weight_mutex);
        return true;
    }

    return false;
}

void Weight_ClearTwinOverride(void)
{
    if (!g_initialized)
    {
        return;
    }

    if (xSemaphoreTake(g_weight_mutex, portMAX_DELAY) == pdTRUE)
    {
        g_twin_override_active = false;
        xSemaphoreGive(g_weight_mutex);
    }
}

bool Weight_IsTwinOverrideActive(void)
{
    return g_twin_override_active;
}
