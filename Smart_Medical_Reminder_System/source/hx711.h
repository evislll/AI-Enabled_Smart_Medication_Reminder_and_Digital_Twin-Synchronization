#ifndef HX711_H
#define HX711_H

#include <stdbool.h>
#include <stdint.h>

bool HX711_Init(void);
bool HX711_IsReady(void);
bool HX711_ReadRaw(int32_t *value);
void HX711_PowerDown(void);
void HX711_PowerUp(void);

/* Optional debug helpers */
uint32_t HX711_GetDtState(void);
uint32_t HX711_GetSckState(void);

#endif
