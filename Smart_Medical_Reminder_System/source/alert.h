#ifndef ALERT_H_
#define ALERT_H_

#include <stdbool.h>

bool Alert_Init(void);
void Alert_StartPattern(void); // short beep + vibration pulse
void Alert_Stop(void);
void Alert_SirenPattern(void);

#endif
