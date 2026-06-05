//#ifndef RISK_H
//#define RISK_H
//
//#include <stdint.h>
//#include <stdbool.h>
//
//#define RISK_HISTORY_LEN 5U
//
//typedef enum
//{
//    RISK_LOW = 0,
//    RISK_MEDIUM,
//    RISK_HIGH
//} RiskLevel_t;
//
//typedef struct
//{
//    uint8_t reminder_count;      // attempts in that cycle
//    uint8_t dispense_failed;     // 1 if dispense verify failed
//    uint8_t dose_taken;          // 1 if dose ended taken
//    uint8_t dose_missed;         // 1 if dose ended missed
//    uint8_t weight_ack;          // 1 if taken confirmed by tray/weight
//} RiskCycleRecord_t;
//
//typedef struct
//{
//    RiskCycleRecord_t hist[RISK_HISTORY_LEN];
//    uint8_t head;
//    uint8_t count;
//
//    uint8_t current_reminder_count;
//    uint8_t current_dispense_failed;
//    uint8_t current_weight_ack;
//
//    uint8_t missed_streak;
//
//    float risk_score;
//    RiskLevel_t risk_level;
//} RiskState_t;
//
//typedef struct
//{
//    uint16_t buzzer_ms;
//    uint16_t repeat_seconds;
//    uint8_t max_attempts;
//    bool allow_retry_once;
//    bool enhanced_assist;
//} RiskPolicy_t;
//
//void Risk_Init(RiskState_t *s);
//void Risk_OnReminderAttempt(RiskState_t *s, uint8_t attempts);
//void Risk_OnDispenseVerify(RiskState_t *s, bool success);
//void Risk_OnWeightAck(RiskState_t *s, bool acked);
//void Risk_CloseDoseCycle(RiskState_t *s, bool taken, bool missed, bool weight_ack);
//
//float Risk_ComputeScore(RiskState_t *s);
//RiskLevel_t Risk_GetLevel(float score);
//RiskPolicy_t Risk_GetPolicy(RiskLevel_t level);
//uint8_t Risk_GetPercent(const RiskState_t *s);
//
//#endif
//
//
//
//
//








#ifndef RISK_H
#define RISK_H

#include <stdint.h>
#include <stdbool.h>

#define RISK_HISTORY_LEN 5U

typedef enum
{
    RISK_LOW = 0,
    RISK_MEDIUM,
    RISK_HIGH
} RiskLevel_t;

typedef struct
{
    uint8_t reminder_count;      // attempts in that cycle
    uint8_t dispense_failed;     // 1 if dispense verify failed
    uint8_t dose_taken;          // 1 if dose ended taken
    uint8_t dose_missed;         // 1 if dose ended missed
    uint8_t weight_ack;          // 1 if taken confirmed by tray/weight
} RiskCycleRecord_t;

typedef struct
{
    RiskCycleRecord_t hist[RISK_HISTORY_LEN];
    uint8_t head;
    uint8_t count;

    uint8_t current_reminder_count;
    uint8_t current_dispense_failed;
    uint8_t current_weight_ack;

    uint8_t missed_streak;

    float risk_score;
    RiskLevel_t risk_level;

    float w_missed;
    float w_reminder;
    float w_failure;
    float w_ack;

} RiskState_t;

typedef struct
{
    uint16_t buzzer_ms;
    uint16_t repeat_seconds;
    uint8_t max_attempts;
    bool allow_retry_once;
    bool enhanced_assist;
} RiskPolicy_t;



typedef struct
{
    float w_missed;
    float w_reminder;
    float w_failure;
    float w_ack;
    uint32_t magic;
    uint32_t crc;
} RiskWeightsFlash_t;



#define RISK_WEIGHTS_MAGIC 0x5249534BU



bool Risk_SaveWeights(const RiskState_t *s);
bool Risk_LoadWeights(RiskState_t *s);
void Risk_Init(RiskState_t *s);
void Risk_OnReminderAttempt(RiskState_t *s, uint8_t attempts);
void Risk_OnDispenseVerify(RiskState_t *s, bool success);
void Risk_OnWeightAck(RiskState_t *s, bool acked);
void Risk_CloseDoseCycle(RiskState_t *s, bool taken, bool missed, bool weight_ack);
void Risk_UpdateWeights(RiskState_t *s, bool taken, bool missed);

float Risk_ComputeScore(RiskState_t *s);
RiskLevel_t Risk_GetLevel(float score);
RiskPolicy_t Risk_GetPolicy(RiskLevel_t level);
uint8_t Risk_GetPercent(const RiskState_t *s);


void Risk_RefreshCurrentLevel(RiskState_t *s);

#endif
















