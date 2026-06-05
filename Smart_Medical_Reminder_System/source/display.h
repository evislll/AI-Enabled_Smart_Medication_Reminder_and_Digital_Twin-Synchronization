
#ifndef DISPLAY_H_
#define DISPLAY_H_

#include <stdint.h>
#include <stdbool.h>
#include "reminder.h"

bool Display_Init(void);
void Display_ShowBoot(void);
void Display_ShowTime(const RtcTime* t, const DoseTime* nextDose);

void Display_ShowReminderScreen(uint8_t h, uint8_t m);
void Display_ShowReminderAttempt(uint8_t attempt);
void Display_ShowTakenScreen(void);
void Display_ShowMissedScreen(void);
void Display_ShowSnoozedScreen(void);

// Optional: show last event + risk
void Display_ShowLogAndRisk(void);

// New: prove onboard log storage
void Display_ShowLatestEvent(void);
void Display_ShowRecentLogs(uint8_t max_logs);


bool Display_IsHoldActive(void);
void Display_HoldLatestEvent(uint32_t hold_ms);


bool Display_IsLogViewerActive(void);
void Display_ShowNextStoredLog(void);
void Display_ExitLogViewer(void);

#endif


