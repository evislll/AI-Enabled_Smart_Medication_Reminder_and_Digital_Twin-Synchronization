#ifndef SERVO_H_
#define SERVO_H_

#include <stdbool.h>
#include <stdint.h>

//-------------TWIN-------------//
typedef enum
{
    SERVO_STATE_UNKNOWN = 0,
    SERVO_STATE_HOME,
    SERVO_STATE_DISPENSING,
    SERVO_STATE_RETURNING
} ServoState;

ServoState Servo_GetState(void);
const char *Servo_GetStateString(void);
//-----------TWIN---------//

bool Servo_Init(void);
void Servo_SetAngle(uint8_t angle);
void Servo_DispenseCycle(void);
int16_t Servo_GetLastAngle(void);

#endif

