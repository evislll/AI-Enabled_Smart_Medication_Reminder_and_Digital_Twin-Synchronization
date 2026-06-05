#ifndef WEIGHT_H
#define WEIGHT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    int32_t raw;
    int32_t filtered_raw;
    int32_t offset;
    float calibration_factor;
    float grams;
    float instant_grams;
    bool valid;
} WeightData;

bool Weight_Init(void);
bool Weight_Tare(uint16_t sample_count);
bool Weight_Update(void);

void Weight_SetCalibrationFactor(float factor);
float Weight_GetCalibrationFactor(void);
bool Weight_GetData(WeightData *out);
int16_t Weight_GetLastReading(void);

float Weight_ComputeCalibrationFactor(int32_t raw_with_mass,
                                      int32_t offset,
                                      float known_mass_grams);


//---------twin override----------//

bool Weight_SetTwinOverride(float grams);
void Weight_ClearTwinOverride(void);
bool Weight_IsTwinOverrideActive(void);





#endif
