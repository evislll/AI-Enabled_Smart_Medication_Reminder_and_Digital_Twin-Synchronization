#ifndef RTC_H_
#define RTC_H_

#include <stdbool.h>
#include <stdint.h>
#include "reminder.h"   // for RtcTime type

bool RTC_AppInit(void);
bool RTC_GetTime(RtcTime* out);
bool RTC_SetTime(const RtcTime* in);
//void RTC_SetFromCompileTime(void);

#endif
