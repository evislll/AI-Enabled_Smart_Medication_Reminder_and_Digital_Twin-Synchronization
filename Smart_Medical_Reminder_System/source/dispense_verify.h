#ifndef DISPENSE_VERIFY_H_
#define DISPENSE_VERIFY_H_

#include <stdbool.h>
#include <stdint.h>


////////////////////////////////////////////////////////////////////////////////////////////////////
//-------Got this skeleton from ChatGpt by providing it the context and functions i needed--------//
////////////////////////////////////////////////////////////////////////////////////////////////////



#ifdef __cplusplus
extern "C" {
#endif

#define DISPENSE_VERIFY_MED_ID_MAX_LEN     16
#define DISPENSE_VERIFY_RESULT_MAX_LEN     24
#define DISPENSE_VERIFY_NOTE_MAX_LEN       64

typedef enum
{
    DISPENSE_VERIFY_OK = 0,
    DISPENSE_VERIFY_ERR_NULL,
    DISPENSE_VERIFY_ERR_NOT_READY,
    DISPENSE_VERIFY_ERR_PRE_STABLE_TIMEOUT,
    DISPENSE_VERIFY_ERR_POST_STABLE_TIMEOUT,
    DISPENSE_VERIFY_ERR_SERVO_FAIL
} DispenseVerifyStatus;

typedef struct
{
    float threshold_g;              // success if delta_g >= threshold_g
    uint32_t pre_stable_timeout_ms; // max wait for pre stable weight
    uint32_t post_stable_timeout_ms;// max wait for post stable weight
    uint32_t settle_delay_ms;       // wait after servo before post read
    uint32_t stable_recheck_ms;     // small delay between repeated stability checks
} DispenseVerifyConfig;

typedef struct
{
    char med_id[DISPENSE_VERIFY_MED_ID_MAX_LEN];
    char result[DISPENSE_VERIFY_RESULT_MAX_LEN]; // DISPENSE_SUCCESS / DISPENSE_FAIL / DISPENSE_ERROR
    char note[DISPENSE_VERIFY_NOTE_MAX_LEN];

    float pre_weight_g;
    float post_weight_g;
    float delta_g;
    float threshold_g;

    DispenseVerifyStatus status;
    bool success;
} DispenseVerifyResult;

/**
 * Initialize module with default config.
 */
void DispenseVerify_Init(void);

/**
 * Override configuration.
 * Returns false if cfg == NULL.
 */
bool DispenseVerify_SetConfig(const DispenseVerifyConfig *cfg);

/**
 * Read current config.
 */
void DispenseVerify_GetConfig(DispenseVerifyConfig *out_cfg);

/**
 * Run full closed-loop dispense verification:
 * 1) capture pre stable weight
 * 2) trigger servo dispense
 * 3) wait settle delay
 * 4) capture post stable weight
 * 5) compute delta
 * 6) classify success/fail
 *
 * med_id may be NULL; then "UNKNOWN" is used.
 */
bool DispenseVerify_Run(const char *med_id, DispenseVerifyResult *out);

/**
 * Helper to get human-readable status text.
 */
const char *DispenseVerify_StatusToString(DispenseVerifyStatus status);

#ifdef __cplusplus
}
#endif

#endif /* DISPENSE_VERIFY_H_ */
