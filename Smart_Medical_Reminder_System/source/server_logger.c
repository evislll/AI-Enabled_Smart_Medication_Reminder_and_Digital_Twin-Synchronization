#include "server_logger.h"

#include <stdio.h>
#include <string.h>

#include "fsl_debug_console.h"
#include "net_client.h"

#include "FreeRTOS.h"
#include "task.h"
#include "flash_logger.h"
#include "queue.h"

#define SERVER_LOGGER_JSON_MAX_LEN      256U
#define SERVER_LOGGER_QUEUE_LENGTH      96U
#define SERVER_LOGGER_TASK_STACK        1024U
#define SERVER_LOGGER_TASK_PRIORITY     (tskIDLE_PRIORITY + 1)

typedef struct
{
    char path[64];
    char json[SERVER_LOGGER_JSON_MAX_LEN];
} ServerLogMessage;

static QueueHandle_t g_serverLogQueue = NULL;
static bool g_serverLoggerInitialized = false;

static void SafeCopyResult(char *dst, size_t dst_size, const char *src)
{
    if ((dst == NULL) || (dst_size == 0U))
    {
        return;
    }

    if (src == NULL)
    {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, dst_size - 1U);
    dst[dst_size - 1U] = '\0';
}

static long FloatToX100(float value)
{
    if (value >= 0.0f)
    {
        return (long)(value * 100.0f + 0.5f);
    }
    else
    {
        return (long)(value * 100.0f - 0.5f);
    }
}


static void FlashRecovery_Task(void *pvParameters)
{
    (void)pvParameters;

    /* wait for network + logger task to be stable */
    vTaskDelay(pdMS_TO_TICKS(30000));

    PRINTF("SYS,FLASH_SYNC_START\r\n");
    FlashLogger_DumpAll();
    PRINTF("SYS,FLASH_SYNC_DONE\r\n");

    vTaskDelete(NULL);
}



static void ServerLogger_Task(void *pvParameters)
{
	ServerLogMessage msg;

	    (void)pvParameters;

	    PRINTF("SYS,SERVER_LOGGER_TASK_START\r\n");
	    vTaskDelay(pdMS_TO_TICKS(3000));


///////////////////////////////////////////////////////////////////////////////////
//------------------block from line 91 - 104 was checked on ChatGpt--------------//
//------------------and then corrected properly----------------------------------//
///////////////////////////////////////////////////////////////////////////////////


	    for (;;)

    {
        if (xQueueReceive(g_serverLogQueue, &msg, portMAX_DELAY) == pdPASS)
        {
//            PRINTF("SYS,SERVER_LOGGER_DEQUEUE\r\n");
//            PRINTF("POST_JSON,%s\r\n", msg.json);

            if (NetClient_PostJson(msg.path, msg.json))
            {
//                PRINTF("SYS,SERVER_LOGGER_POST_OK\r\n");
            }
            else
            {
//                PRINTF("SYS,SERVER_LOGGER_POST_FAIL\r\n");
            	vTaskDelay(pdMS_TO_TICKS(200));
            }
        }
    }
}



bool ServerLogger_Enqueue(const char *path, const char *json)
{
    ServerLogMessage msg;

    if ((path == NULL) || (json == NULL))
    {
        PRINTF("SYS,SERVER_LOGGER_ENQUEUE_BAD_ARGS\r\n");
        return false;
    }

    if (!g_serverLoggerInitialized)
    {
        PRINTF("SYS,SERVER_LOGGER_NOT_INIT\r\n");
        return false;
    }

    memset(&msg, 0, sizeof(msg));
    SafeCopyResult(msg.path, sizeof(msg.path), path);
    SafeCopyResult(msg.json, sizeof(msg.json), json);

    if (xQueueSend(g_serverLogQueue, &msg, 0) != pdPASS)
    {
        PRINTF("SYS,SERVER_LOGGER_QUEUE_FULL\r\n");
        return false;
    }

//    PRINTF("SYS,SERVER_LOGGER_ENQUEUE_OK,path=%s\r\n", msg.path);
    return true;
}


/////////////////////////////////////////////////////////////////////////////////
//---------------following function is Created using ChatGpt-------------------//
/////////////////////////////////////////////////////////////////////////////////



bool ServerLogger_Init(void)
{
    BaseType_t task_ok;

    if (g_serverLoggerInitialized)
    {
        return true;
    }

    g_serverLogQueue = xQueueCreate(SERVER_LOGGER_QUEUE_LENGTH, sizeof(ServerLogMessage));
    if (g_serverLogQueue == NULL)
    {
        PRINTF("SYS,SERVER_LOGGER_QUEUE_CREATE_FAIL\r\n");
        return false;
    }

    task_ok = xTaskCreate(ServerLogger_Task,
                          "server_log",
                          SERVER_LOGGER_TASK_STACK,
                          NULL,
                          SERVER_LOGGER_TASK_PRIORITY,
                          NULL);

    if (task_ok != pdPASS)
    {
        PRINTF("SYS,SERVER_LOGGER_TASK_CREATE_FAIL\r\n");
        return false;
    }


    /* ADD THIS BLOCK */
    task_ok = xTaskCreate(FlashRecovery_Task,
                          "flash_recovery",
                          configMINIMAL_STACK_SIZE + 512,
                          NULL,
                          tskIDLE_PRIORITY + 1,
                          NULL);

    if (task_ok != pdPASS)
    {
        PRINTF("SYS,FLASH_RECOVERY_TASK_CREATE_FAIL\r\n");
        return false;
    }


    g_serverLoggerInitialized = true;
    PRINTF("SYS,SERVER_LOGGER_INIT_OK\r\n");
    return true;
}


/////////////////////////////////////////////////////////////////////////////////
//---------------following function is Created using ChatGpt-------------------//
/////////////////////////////////////////////////////////////////////////////////



bool ServerLogger_SendDispenseVerification(const DispenseVerifyResult *result)
{
    ServerLogMessage msg;
    char med_id[DISPENSE_VERIFY_MED_ID_MAX_LEN];
    char result_text[DISPENSE_VERIFY_RESULT_MAX_LEN];
    long pre_x100;
    long post_x100;
    long delta_x100;
    long threshold_x100;
    int json_len;

    if (result == NULL)
    {
        PRINTF("SYS,SERVER_LOGGER_NULL_RESULT\r\n");
        return false;
    }

    if (!g_serverLoggerInitialized)
    {
        PRINTF("SYS,SERVER_LOGGER_NOT_INIT\r\n");
        return false;
    }

    SafeCopyResult(med_id, sizeof(med_id), result->med_id);
    SafeCopyResult(result_text, sizeof(result_text), result->result);

    pre_x100 = FloatToX100(result->pre_weight_g);
    post_x100 = FloatToX100(result->post_weight_g);
    delta_x100 = FloatToX100(result->delta_g);
    threshold_x100 = FloatToX100(result->threshold_g);

    memset(&msg, 0, sizeof(msg));
    SafeCopyResult(msg.path, sizeof(msg.path), "/api/dispense_verification");

    json_len = snprintf(
        msg.json,
        sizeof(msg.json),
        "{"
        "\"med_id\":\"%s\","
        "\"result\":\"%s\","
        "\"pre_weight_x100\":%ld,"
        "\"post_weight_x100\":%ld,"
        "\"delta_x100\":%ld,"
        "\"threshold_x100\":%ld"
        "}",
        med_id,
        result_text,
        pre_x100,
        post_x100,
        delta_x100,
        threshold_x100
    );

    if ((json_len <= 0) || ((size_t)json_len >= sizeof(msg.json)))
    {
        PRINTF("SYS,SERVER_LOGGER_JSON_BUILD_FAIL\r\n");
        return false;
    }

    if (xQueueSend(g_serverLogQueue, &msg, 0) != pdPASS)
    {
        PRINTF("SYS,SERVER_LOGGER_QUEUE_FULL\r\n");
        return false;
    }

    PRINTF("SYS,SERVER_LOGGER_ENQUEUE_OK\r\n");
    return true;
}




bool ServerLogger_SendDoseEvent(const char *med_id,
                                const char *status,
                                uint8_t reminder_count,
                                uint16_t delay_minutes)
{
    ServerLogMessage msg;
    char safe_med_id[32];
    char safe_status[16];
    int json_len;

    if (!g_serverLoggerInitialized)
    {
        PRINTF("SYS,SERVER_LOGGER_NOT_INIT\r\n");
        return false;
    }

    memset(&msg, 0, sizeof(msg));
    memset(safe_med_id, 0, sizeof(safe_med_id));
    memset(safe_status, 0, sizeof(safe_status));

    SafeCopyResult(safe_med_id, sizeof(safe_med_id), med_id);
    SafeCopyResult(safe_status, sizeof(safe_status), status);

    if (safe_med_id[0] == '\0')
    {
        strcpy(safe_med_id, "unknown");
    }

    if (safe_status[0] == '\0')
    {
        strcpy(safe_status, "UNKNOWN");
    }

    SafeCopyResult(msg.path, sizeof(msg.path), "/api/events");

    json_len = snprintf(
        msg.json,
        sizeof(msg.json),
        "{"
        "\"timestamp\":\"remote\","
        "\"med_id\":\"%s\","
        "\"status\":\"%s\","
        "\"reminder_count\":%u,"
        "\"delay_minutes\":%u"
        "}",
        safe_med_id,
        safe_status,
        (unsigned int)reminder_count,
        (unsigned int)delay_minutes
    );

    if ((json_len <= 0) || ((size_t)json_len >= sizeof(msg.json)))
    {
        PRINTF("SYS,SERVER_LOGGER_JSON_BUILD_FAIL\r\n");
        return false;
    }

    if (xQueueSend(g_serverLogQueue, &msg, 0) != pdPASS)
    {
        PRINTF("SYS,SERVER_LOGGER_QUEUE_FULL\r\n");
        return false;
    }

    PRINTF("SYS,SERVER_LOGGER_ENQUEUE_OK\r\n");
    return true;
}


bool ServerLogger_PostTwin(const char *json)
{
    if (json == NULL)
    {
        return false;
    }

    return ServerLogger_Enqueue("/api/twin/update", json);
}








