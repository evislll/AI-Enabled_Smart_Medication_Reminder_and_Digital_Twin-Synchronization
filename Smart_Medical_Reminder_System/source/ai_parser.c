#include "ai_parser.h"
#include "reminder.h"
#include "fsl_debug_console.h"
#include "net_client.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"


bool AI_ParseLine(const char *line)
{
    char buf[64];
    char *token;
    char *med_id;
    char *risk_str;
    char *lead_str;
    char *endptr;
    long risk_val;
    long lead_val;
    bool high_risk;
    uint16_t lead_sec;

    if (line == NULL)
    {
        return false;
    }

    //----------Chat gpt was used to create this block----------// (line 34 - 71)

    strncpy(buf, line, sizeof(buf) - 1U);
    buf[sizeof(buf) - 1U] = '\0';

    token = strtok(buf, ",");
    if ((token == NULL) || (strcmp(token, "AI") != 0))
    {
        return false;
    }

    med_id   = strtok(NULL, ",");
    risk_str = strtok(NULL, ",");
    lead_str = strtok(NULL, ",\r\n");

    if ((med_id == NULL) || (risk_str == NULL) || (lead_str == NULL))
    {
        PRINTF("AI parse failed: invalid format\r\n");
        return false;
    }

    risk_val = strtol(risk_str, &endptr, 10);
    if ((*endptr != '\0') || ((risk_val != 0) && (risk_val != 1)))
    {
        PRINTF("AI parse failed: invalid risk=%s\r\n", risk_str);
        return false;
    }

    lead_val = strtol(lead_str, &endptr, 10);
    if ((*endptr != '\0') || (lead_val < 0) || (lead_val > 3600))
    {
        PRINTF("AI parse failed: invalid lead=%s\r\n", lead_str);
        return false;
    }

    high_risk = (risk_val == 1);
    lead_sec = (uint16_t)lead_val;

    if (!Reminder_SetAiProfile(med_id, high_risk, lead_sec))
    {
        PRINTF("AI parse ignored: unknown med_id=%s\r\n", med_id);
        return false;
    }

    PRINTF("AI applied: med=%s high_risk=%u lead_sec=%u\r\n",
           med_id, (unsigned)high_risk, (unsigned)lead_sec);

    return true;
}


void AI_BootSync(void)
{
    uint32_t attempt;
    const uint32_t max_attempts = 3U;

    PRINTF("SYS,AI_BOOTSYNC_START\r\n");

    for (attempt = 1U; attempt <= max_attempts; attempt++)
    {
        PRINTF("SYS,AI_BOOTSYNC_ATTEMPT,%lu\r\n", (unsigned long)attempt);

        if (AI_BootSyncEthernet())
        {
            PRINTF("SYS,AI_BOOTSYNC_SUCCESS\r\n");
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(1000U));
    }

    PRINTF("SYS,AI_BOOTSYNC_TIMEOUT_DEFAULTS\r\n");
}


bool AI_BootSyncEthernet(void)
{
    char rx_buf[512];
    char line[64];
    char *cursor;
    char *next;
    uint32_t valid_lines = 0U;

    memset(rx_buf, 0, sizeof(rx_buf));
    memset(line, 0, sizeof(line));

    PRINTF("SYS,AI_BOOTSYNC_ETH_START\r\n");

    if (!NetClient_GetText("/api/ai/boot-config", rx_buf, sizeof(rx_buf)))
    {
        PRINTF("SYS,AI_BOOTSYNC_ETH_FETCH_FAIL\r\n");
        return false;
    }

    PRINTF("SYS,AI_BOOTSYNC_ETH_FETCH_OK\r\n");


    //------------------WROTE THIS skeleton BLOCK by ourself first but then enhanced using ChatGpt -----------------// (line 129 - 195)

    cursor = rx_buf;

    while ((cursor != NULL) && (*cursor != '\0'))
    {
        next = strchr(cursor, '\n');

        if (next != NULL)
        {
            size_t len = (size_t)(next - cursor);

            if (len >= sizeof(line))
            {
                len = sizeof(line) - 1U;
                PRINTF("SYS,AI_LINE_TRUNCATED\r\n");
            }

            memcpy(line, cursor, len);
            line[len] = '\0';
            cursor = next + 1;
        }
        else
        {
            strncpy(line, cursor, sizeof(line) - 1U);
            line[sizeof(line) - 1U] = '\0';
            cursor = NULL;
        }

        {
            char *start = line;

            while ((*start == ' ') || (*start == '\t') || (*start == '\r'))
            {
                start++;
            }

            if (*start == '\0')
            {
                memset(line, 0, sizeof(line));
                continue;
            }

            if (strcmp(start, "AI_DONE") == 0)
            {
                PRINTF("SYS,AI_SYNC_DONE\r\n");
                PRINTF("SYS,AI_BOOTSYNC_ETH_APPLIED,%lu\r\n", (unsigned long)valid_lines);
                return (valid_lines > 0U);
            }

            if (AI_ParseLine(start))
            {
                valid_lines++;
                PRINTF("SYS,AI_LINE_OK\r\n");
            }
            else
            {
                PRINTF("SYS,AI_LINE_BAD\r\n");
            }
        }

        memset(line, 0, sizeof(line));
    }

    PRINTF("SYS,AI_BOOTSYNC_ETH_END,%lu\r\n", (unsigned long)valid_lines);
    return (valid_lines > 0U);
}






















