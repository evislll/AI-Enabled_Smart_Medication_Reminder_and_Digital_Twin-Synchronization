#ifndef SERVER_LOGGER_H_
#define SERVER_LOGGER_H_

#include <stdbool.h>
#include "dispense_verify.h"

#ifdef __cplusplus
extern "C" {
#endif

bool ServerLogger_Enqueue(const char *path, const char *json);
bool ServerLogger_Init(void);
bool ServerLogger_SendDispenseVerification(const DispenseVerifyResult *result);
bool ServerLogger_PostTwin(const char *json);
bool ServerLogger_SendDoseEvent(const char *med_id,
                                const char *status,
                                uint8_t reminder_count,
                                uint16_t delay_minutes);


#ifdef __cplusplus
}
#endif

#endif /* SERVER_LOGGER_H_ */
