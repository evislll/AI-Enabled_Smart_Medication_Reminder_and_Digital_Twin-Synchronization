#ifndef FLASH_LOGGER_H_
#define FLASH_LOGGER_H_

#include <stdbool.h>
#include <stdint.h>
#include "logging.h"
#include "risk.h"


#ifdef __cplusplus
extern "C" {
#endif


//////////////////////////////////////////////////////////////////////////
//----------created all function headers by myself but------------------//
//----------had take help of ChatGpt to figure out addresses----------//
//////////////////////////////////////////////////////////////////////////



#define FLASH_LOGGER_BASE_ADDR   (0x001E0000u)
#define FLASH_LOGGER_SECTOR_SIZE (0x1000u)


bool FlashLogger_Init(void);
bool FlashLogger_Append(const LogEntry *entry);
void RecoveredLog_SendToServer(const LogEntry *e);
bool FlashLogger_ReadAt(uint32_t index, LogEntry *out);
uint32_t FlashLogger_GetCount(void);
void FlashLogger_DumpAll(void);
bool FlashLogger_EraseAll(void);

bool FlashLogger_SaveRiskWeights(const RiskWeightsFlash_t *fw);
bool FlashLogger_LoadRiskWeights(RiskWeightsFlash_t *fw);



/* safe read-only probe for testing */
void FlashLogger_ReadProbe(void);

#ifdef __cplusplus
}
#endif

#endif /* FLASH_LOGGER_H_ */
