#include "flash_logger.h"

#include <string.h>
#include <stdio.h>
#include "fsl_flash.h"
#include "fsl_debug_console.h"
#include "server_logger.h"
#include "rtc.h"

typedef struct
{
    LogEntry entry;
    uint8_t pad[2];
} FlashLogSlot;

#define FLASH_LOG_REGION_START   (0x001E0000u)
#define FLASH_LOG_REGION_SIZE    (0x00020000u)
#define FLASH_SECTOR_SIZE        (0x1000u)

#define FLASH_WEIGHTS_SIZE       (FLASH_SECTOR_SIZE)
#define FLASH_WEIGHTS_ADDR       (FLASH_LOG_REGION_START + FLASH_LOG_REGION_SIZE - FLASH_WEIGHTS_SIZE)

#define FLASH_LOG_USABLE_SIZE    (FLASH_LOG_REGION_SIZE - FLASH_WEIGHTS_SIZE)
#define FLASH_LOG_RECORD_SIZE    ((uint32_t)sizeof(FlashLogSlot))
#define FLASH_LOG_MAX_RECORDS    (FLASH_LOG_USABLE_SIZE / FLASH_LOG_RECORD_SIZE)

static uint32_t g_flashLogCount = 0U;
static uint32_t g_flashNextIndex = 0U;
static bool g_flashLoggerReady = false;
static flash_config_t g_flashConfig;
static bool g_flashDriverReady = false;


////////////////////////////////////////////////////////////////////////////////////////////////////////////
//-----------------------------ChatGpt almost created the whole file but had to figure--------------------//
//-----------------------------out lot of stuff like how to exactly write to flash memory-----------------//
////////////////////////////////////////////////////////////////////////////////////////////////////////////



const char *Logging_RiskLevelToString(RiskLevel_t level)
{
    switch (level)
    {
        case RISK_HIGH:   return "HIGH";
        case RISK_MEDIUM: return "MEDIUM";
        case RISK_LOW:
        default:          return "LOW";
    }
}

static bool FlashLogger_IsSlotEmpty(const uint8_t *ptr)
{
    uint32_t i;

    if (ptr == NULL)
    {
        return false;
    }

    for (i = 0U; i < sizeof(LogEntry); i++)
    {
        if (ptr[i] != 0xFFU)
        {
            return false;
        }
    }

    return true;
}

static uint16_t FlashLogger_CalcCRC(const LogEntry *e)
{
    const uint8_t *data;
    uint16_t sum = 0U;
    uint32_t i;

    if (e == NULL)
    {
        return 0U;
    }

    data = (const uint8_t *)e;

    for (i = 0U; i < (sizeof(LogEntry) - sizeof(e->crc)); i++)
    {
        sum = (uint16_t)(sum + data[i]);
    }

    return sum;
}

static bool FlashLogger_IsRecordValid(const LogEntry *e)
{
    if (e == NULL)
    {
        return false;
    }

    return (e->crc == FlashLogger_CalcCRC(e));
}





static void FlashLogger_UnpackTimestamp(uint32_t ts,
                                        uint16_t *year,
                                        uint8_t *month,
                                        uint8_t *day,
                                        uint8_t *hour,
                                        uint8_t *minute,
                                        uint8_t *second)
{
    if (year != NULL)
    {
        *year = (uint16_t)(2000U + ((ts >> 26) & 0x3FU));
    }

    if (month != NULL)
    {
        *month = (uint8_t)((ts >> 22) & 0x0FU);
    }

    if (day != NULL)
    {
        *day = (uint8_t)((ts >> 17) & 0x1FU);
    }

    if (hour != NULL)
    {
        *hour = (uint8_t)((ts >> 12) & 0x1FU);
    }

    if (minute != NULL)
    {
        *minute = (uint8_t)((ts >> 6) & 0x3FU);
    }

    if (second != NULL)
    {
        *second = (uint8_t)(ts & 0x3FU);
    }
}


static bool FlashLogger_EraseTestSector(void)
{
    status_t status;

    if (!g_flashDriverReady)
    {
        PRINTF("SYS,FLASH_ERASE_NOT_READY\r\n");
        return false;
    }

    PRINTF("SYS,FLASH_ERASE_START,addr=0x%08lX,size=0x%lX\r\n",
           (unsigned long)FLASH_LOG_REGION_START,
           (unsigned long)FLASH_SECTOR_SIZE);

    status = FLASH_Erase(&g_flashConfig,
                         FLASH_LOG_REGION_START,
                         FLASH_SECTOR_SIZE,
                         kFLASH_ApiEraseKey);

    if (status != kStatus_Success)
    {
        PRINTF("SYS,FLASH_ERASE_FAIL,status=%ld\r\n", (long)status);
        return false;
    }

    PRINTF("SYS,FLASH_ERASE_OK\r\n");
    return true;
}






static bool FlashLogger_EraseSectorContainingIndex(uint32_t index)
{
    status_t status;
    uint32_t recordAddr;
    uint32_t sectorStartAddr;
    uint32_t sectorStartIndex;

    if (!g_flashDriverReady)
    {
        PRINTF("SYS,FLASH_RECOVERY_ERASE_NOT_READY\r\n");
        return false;
    }

    if (index >= FLASH_LOG_MAX_RECORDS)
    {
        PRINTF("SYS,FLASH_RECOVERY_ERASE_BAD_INDEX,index=%lu\r\n",
               (unsigned long)index);
        return false;
    }

    recordAddr = FLASH_LOG_REGION_START + (index * FLASH_LOG_RECORD_SIZE);
    sectorStartAddr = recordAddr & ~(FLASH_SECTOR_SIZE - 1U);

    if (sectorStartAddr < FLASH_LOG_REGION_START)
    {
        PRINTF("SYS,FLASH_RECOVERY_ERASE_ADDR_LOW,addr=0x%08lX\r\n",
               (unsigned long)sectorStartAddr);
        return false;
    }

    if ((sectorStartAddr + FLASH_SECTOR_SIZE) >
        (FLASH_LOG_REGION_START + FLASH_LOG_REGION_SIZE))
    {
        PRINTF("SYS,FLASH_RECOVERY_ERASE_ADDR_HIGH,addr=0x%08lX\r\n",
               (unsigned long)sectorStartAddr);
        return false;
    }

    sectorStartIndex =
        (sectorStartAddr - FLASH_LOG_REGION_START) / FLASH_LOG_RECORD_SIZE;

    PRINTF("SYS,FLASH_RECOVERY_ERASE_START,index=%lu,sector_index=%lu,addr=0x%08lX,size=0x%lX\r\n",
           (unsigned long)index,
           (unsigned long)sectorStartIndex,
           (unsigned long)sectorStartAddr,
           (unsigned long)FLASH_SECTOR_SIZE);

    status = FLASH_Erase(&g_flashConfig,
                         sectorStartAddr,
                         FLASH_SECTOR_SIZE,
                         kFLASH_ApiEraseKey);

    if (status != kStatus_Success)
    {
        PRINTF("SYS,FLASH_RECOVERY_ERASE_FAIL,index=%lu,status=%ld\r\n",
               (unsigned long)index,
               (long)status);
        return false;
    }

    g_flashNextIndex = sectorStartIndex;
    g_flashLogCount = sectorStartIndex;
    g_flashLoggerReady = true;

    PRINTF("SYS,FLASH_RECOVERY_ERASE_OK,next=%lu,count=%lu\r\n",
           (unsigned long)g_flashNextIndex,
           (unsigned long)g_flashLogCount);

    return true;
}



static bool FlashLogger_DriverInit(void)
{
    status_t status;


    status = FLASH_Init(&g_flashConfig);
        if (status != kStatus_Success)
        {
            PRINTF("SYS,FLASH_DRIVER_INIT_FAIL,status=%ld\r\n", (long)status);
            g_flashDriverReady = false;
            return false;
        }

        g_flashDriverReady = true;
        PRINTF("SYS,FLASH_DRIVER_INIT_OK\r\n");
        return true;
}




static uint32_t FlashLogger_WeightsCRC(const RiskWeightsFlash_t *fw)
{
    RiskWeightsFlash_t temp;
    const uint8_t *p;
    uint32_t sum = 0U;
    uint32_t i;

    if (fw == NULL)
    {
        return 0U;
    }

    temp = *fw;
    temp.crc = 0U;

    p = (const uint8_t *)&temp;

    for (i = 0U; i < sizeof(RiskWeightsFlash_t); i++)
    {
        sum += p[i];
    }

    return sum;
}


static bool FlashLogger_ShouldStore(const LogEntry *entry)
{
    DoseStatus st;

    if (entry == NULL)
    {
        return false;
    }

    if ((LogSource)entry->source != LOG_SRC_REMINDER)
    {
        return false;
    }

    /* STORE REAL AI RISK */
    if ((LogType)entry->type == LOG_TYPE_RISK_CYCLE)
    {
        return true;
    }

    /* STORE USER OUTCOME */
    if ((LogType)entry->type == LOG_TYPE_DOSE_EVENT)
    {
        st = (DoseStatus)entry->status;

        if ((st == DOSE_SNOOZED) ||
            (st == DOSE_MISSED)  ||
            (st == DOSE_TAKEN))
        {
            return true;
        }
    }

    return false;
}



static bool FlashLogger_WriteTestRecord(void)
{
    FlashLogSlot slot;
    status_t status;

    if (!g_flashDriverReady)
    {
        PRINTF("SYS,FLASH_WRITE_NOT_READY\r\n");
        return false;
    }

    memset(&slot, 0xFF, sizeof(slot));
    memset(&slot.entry, 0, sizeof(slot.entry));

    slot.entry.seq = 1;
    slot.entry.timestamp = 0;
    slot.entry.type = LOG_TYPE_REMINDER_TRIGGERED;   /* use your real enum */
    slot.entry.status = 0;          /* use your real enum */
    slot.entry.source = LOG_SRC_REMINDER;            /* use your real enum */
    slot.entry.value = 123;
    slot.entry.extra = 0;
    slot.entry.extra2 = 0;
    slot.entry.flags = 0;
    memcpy(slot.entry.med_id, "TEST", 5);

    slot.entry.crc = FlashLogger_CalcCRC(&slot.entry);

    PRINTF("SYS,FLASH_WRITE_START,addr=0x%08lX,size=%lu,seq=%lu\r\n",
           (unsigned long)FLASH_LOG_REGION_START,
           (unsigned long)sizeof(slot),
           (unsigned long)slot.entry.seq);

    status = FLASH_Program(&g_flashConfig,
                           FLASH_LOG_REGION_START,
                           (uint8_t *)&slot,
                           sizeof(slot));

    if (status != kStatus_Success)
    {
        PRINTF("SYS,FLASH_WRITE_FAIL,status=%ld\r\n", (long)status);
        return false;
    }

    PRINTF("SYS,FLASH_WRITE_OK\r\n");
    return true;
}


static void FlashLogger_PrintRecoveredEvent(const LogEntry *entry)
{
    uint16_t year = 0U;
    uint8_t month = 0U;
    uint8_t day = 0U;
    uint8_t hour = 0U;
    uint8_t minute = 0U;
    uint8_t second = 0U;

    if (entry == NULL)
    {
        return;
    }

    FlashLogger_UnpackTimestamp(entry->timestamp,
                                &year,
                                &month,
                                &day,
                                &hour,
                                &minute,
                                &second);

    PRINTF("FLASH_RECOVERED,SEQ=%lu,TS=%04u-%02u-%02u %02u:%02u:%02u,TYPE=%s,MED=%s,SRC=%s,STATUS=%s,REM=%d,VAL=%d,EXTRA=%d,EXTRA2=%d,RISK=%u%% (%s),CRC=%u\r\n",
           (unsigned long)entry->seq,
           (unsigned int)year,
           (unsigned int)month,
           (unsigned int)day,
           (unsigned int)hour,
           (unsigned int)minute,
           (unsigned int)second,
           Logging_LogTypeToString((LogType)entry->type),
           entry->med_id,
           Logging_SourceToString((LogSource)entry->source),
           Logging_StatusToString(entry->status),

           (int)entry->value,          // REM (reminder count)
           (int)entry->value,          // VAL (same or optional)
           (int)entry->extra,          // dispense_failed
           (int)entry->extra2,         // weight_ack

           (unsigned int)entry->flags, // risk %
           Logging_RiskLevelToString(
               Risk_GetLevel((float)entry->flags / 100.0f)
           ),

           (unsigned int)entry->crc);


}

bool FlashLogger_Init(void)
{
    const uint8_t *base = (const uint8_t *)FLASH_LOG_REGION_START;
    uint32_t i;

    if (!FlashLogger_DriverInit())
    {
        return false;
    }

    PRINTF("SYS,FLASH_LOGGER_INIT,base=0x%08lX,record_size=%lu,max=%lu,weights=0x%08lX\r\n",
       (unsigned long)FLASH_LOG_REGION_START,
       (unsigned long)FLASH_LOG_RECORD_SIZE,
       (unsigned long)FLASH_LOG_MAX_RECORDS,
       (unsigned long)FLASH_WEIGHTS_ADDR);

    PRINTF("SYS,FLASH_PROBE,first_32_bytes=");
    for (i = 0U; i < 32U; i++)
    {
        PRINTF("%02X", base[i]);
        if (i != 31U)
        {
            PRINTF(" ");
        }
    }
    PRINTF("\r\n");

    g_flashLogCount = 0U;
    g_flashNextIndex = 0U;
    g_flashLoggerReady = false;

    for (i = 0U; i < FLASH_LOG_MAX_RECORDS; i++)
    {
        const FlashLogSlot *slot =
            (const FlashLogSlot *)(base + (i * FLASH_LOG_RECORD_SIZE));
        const LogEntry *entry = &slot->entry;

        if ((i % 128U) == 0U)
        {
            PRINTF("SYS,FLASH_SCAN_PROGRESS,i=%lu\r\n", (unsigned long)i);
        }

        if (FlashLogger_IsSlotEmpty((const uint8_t *)entry))
        {
            g_flashNextIndex = i;
            g_flashLogCount = i;
            g_flashLoggerReady = true;

            PRINTF("SYS,FLASH_LOG_INIT_OK,count=%lu,next=%lu,max=%lu\r\n",
                   (unsigned long)g_flashLogCount,
                   (unsigned long)g_flashNextIndex,
                   (unsigned long)FLASH_LOG_MAX_RECORDS);
            return true;
        }

        if (!FlashLogger_IsRecordValid(entry))
        {
            PRINTF("SYS,FLASH_BAD_RECORD_AT,i=%lu\r\n", (unsigned long)i);

            if (FlashLogger_EraseSectorContainingIndex(i))
            {
                PRINTF("SYS,FLASH_RECOVERY_OK_AT,i=%lu,next=%lu,count=%lu\r\n",
                       (unsigned long)i,
                       (unsigned long)g_flashNextIndex,
                       (unsigned long)g_flashLogCount);
                return true;
            }

            PRINTF("SYS,FLASH_RECOVERY_FAILED_AT,i=%lu\r\n",
                   (unsigned long)i);

            g_flashNextIndex = i;
            g_flashLogCount = i;
            g_flashLoggerReady = true;

            PRINTF("SYS,FLASH_LOG_INIT_PARTIAL,count=%lu,next=%lu\r\n",
                   (unsigned long)g_flashLogCount,
                   (unsigned long)g_flashNextIndex);
            return true;
        }
    }

    g_flashLogCount = FLASH_LOG_MAX_RECORDS;
    g_flashNextIndex = FLASH_LOG_MAX_RECORDS;
    g_flashLoggerReady = true;

    PRINTF("SYS,FLASH_LOG_FULL,count=%lu\r\n",
           (unsigned long)g_flashLogCount);

    return true;
}




bool FlashLogger_Append(const LogEntry *entry)
{
    FlashLogSlot slot;
    status_t status;
    uint32_t writeAddr;

    if ((!g_flashLoggerReady) || (!g_flashDriverReady) || (entry == NULL))
    {
        PRINTF("SYS,FLASH_APPEND_NOT_READY\r\n");
        return false;
    }

    if (!FlashLogger_ShouldStore(entry))
    {
        return true;
    }

    if (g_flashNextIndex >= FLASH_LOG_MAX_RECORDS)
    {
        PRINTF("SYS,FLASH_LOG_NO_SPACE\r\n");
        return false;
    }

    writeAddr = FLASH_LOG_REGION_START + (g_flashNextIndex * FLASH_LOG_RECORD_SIZE);

    memset(&slot, 0xFF, sizeof(slot));
    slot.entry = *entry;
    slot.entry.crc = FlashLogger_CalcCRC(&slot.entry);

    status = FLASH_Program(&g_flashConfig,
                           writeAddr,
                           (uint8_t *)&slot,
                           sizeof(slot));

    if (status != kStatus_Success)
    {
        PRINTF("SYS,FLASH_APPEND_FAIL,index=%lu,status=%ld\r\n",
               (unsigned long)g_flashNextIndex,
               (long)status);
        return false;
    }

    g_flashNextIndex++;
    g_flashLogCount++;

    return true;
}



void RecoveredLog_SendToServer(const LogEntry *e)
{
	RtcTime now;
    char json[256];
    uint16_t year = 0U;
    uint8_t month = 0U, day = 0U, hour = 0U, minute = 0U, second = 0U;

    if (e == NULL)
    {
        return;
    }

    FlashLogger_UnpackTimestamp(e->timestamp, &year, &month, &day, &hour, &minute, &second);


    snprintf(json, sizeof(json),
        "{"
        "\"timestamp\":\"%04u-%02u-%02u %02u:%02u:%02u\","
        "\"seq\":%lu,"
        "\"event_type\":\"%s\","
        "\"med_id\":\"%s\","
        "\"src\":\"%s\","
        "\"status\":\"%s\","
        "\"val\":%d,"
        "\"extra\":%d,"
        "\"extra2\":%d,"
        "\"flags\":%u,"
        "\"crc\":%u"
        "}",
        (unsigned int)year,
        (unsigned int)month,
        (unsigned int)day,
        (unsigned int)hour,
        (unsigned int)minute,
        (unsigned int)second,
        (unsigned long)e->seq,
        Logging_LogTypeToString((LogType)e->type),
        e->med_id,
        Logging_SourceToString((LogSource)e->source),
        Logging_StatusToString(e->status),
        (int)e->value,
        (int)e->extra,
        (int)e->extra2,
        (unsigned int)e->flags,
        (unsigned int)e->crc
    );



    if (!ServerLogger_Enqueue("/api/recovered_logs", json))
    {
        PRINTF("SYS,RECOVERED_LOG_DEFER,seq=%lu\r\n",
               (unsigned long)e->seq);

        vTaskDelay(pdMS_TO_TICKS(500));   // small backoff
    }
    else
    {
        PRINTF("SYS,RECOVERED_LOG_ENQUEUE_OK,seq=%lu\r\n",
               (unsigned long)e->seq);
    }


}

bool FlashLogger_ReadAt(uint32_t index, LogEntry *out)
{
    const uint8_t *base = (const uint8_t *)FLASH_LOG_REGION_START;
    const LogEntry *entry;

    if ((!g_flashLoggerReady) || (out == NULL))
    {
        return false;
    }

    if (index >= g_flashLogCount)
    {
        return false;
    }


    {
        const FlashLogSlot *slot =
            (const FlashLogSlot *)(base + (index * FLASH_LOG_RECORD_SIZE));
        entry = &slot->entry;
    }

    if (!FlashLogger_IsRecordValid(entry))
    {
        return false;
    }

    *out = *entry;
    return true;
}

uint32_t FlashLogger_GetCount(void)
{
    return g_flashLogCount;
}

void FlashLogger_DumpAll(void)
{
    uint32_t i;

    if (!g_flashLoggerReady)
    {
        PRINTF("SYS,FLASH_DUMP_NOT_READY\r\n");
        return;
    }

    PRINTF("SYS,FLASH_DUMP_BEGIN,count=%lu\r\n",
           (unsigned long)g_flashLogCount);

    for (i = 0U; i < g_flashLogCount; i++)
    {
        LogEntry entry;

        if (FlashLogger_ReadAt(i, &entry))
        {
        	FlashLogger_PrintRecoveredEvent(&entry);
        	RecoveredLog_SendToServer(&entry);
        	vTaskDelay(pdMS_TO_TICKS(150));
        }
        else
        {
            PRINTF("SYS,FLASH_DUMP_READ_FAIL,index=%lu\r\n",
                   (unsigned long)i);
        }
    }

    PRINTF("SYS,FLASH_DUMP_END\r\n");
}



void FlashLogger_ReadProbe(void)
{
    const uint8_t *p = (const uint8_t *)FLASH_LOGGER_BASE_ADDR;

    PRINTF("FLASH PROBE @ 0x%08X\r\n", FLASH_LOGGER_BASE_ADDR);

    for (int i = 0; i < 32; i++)
    {
        PRINTF("%02X ", p[i]);
        if (((i + 1) % 16) == 0)
        {
            PRINTF("\r\n");
        }
    }
}



bool FlashLogger_EraseAll(void)
{
    status_t status;

    if (!g_flashDriverReady)
    {
        if (!FlashLogger_DriverInit())
        {
            PRINTF("SYS,FLASH_ERASE_ALL_DRIVER_FAIL\r\n");
            return false;
        }
    }

    PRINTF("SYS,FLASH_ERASE_ALL_START,addr=0x%08lX,size=0x%08lX\r\n",
           (unsigned long)FLASH_LOG_REGION_START,
           (unsigned long)FLASH_LOG_REGION_SIZE);

    status = FLASH_Erase(&g_flashConfig,
                         FLASH_LOG_REGION_START,
                         FLASH_LOG_REGION_SIZE,
                         kFLASH_ApiEraseKey);

    if (status != kStatus_Success)
    {
        PRINTF("SYS,FLASH_ERASE_ALL_FAIL,status=%ld\r\n", (long)status);
        return false;
    }

    g_flashLogCount = 0U;
    g_flashNextIndex = 0U;
    g_flashLoggerReady = true;

    PRINTF("SYS,FLASH_ERASE_ALL_OK\r\n");
    return true;
}





bool FlashLogger_SaveRiskWeights(const RiskWeightsFlash_t *fw)
{
    RiskWeightsFlash_t temp;
    status_t status;

    if ((!g_flashDriverReady) || (fw == NULL))
    {
        PRINTF("SYS,WEIGHTS_SAVE_NOT_READY\r\n");
        return false;
    }

    temp = *fw;
    temp.magic = RISK_WEIGHTS_MAGIC;
    temp.crc = 0U;
    temp.crc = FlashLogger_WeightsCRC(&temp);

    PRINTF("SYS,WEIGHTS_SAVE_START,addr=0x%08lX,size=%lu\r\n",
           (unsigned long)FLASH_WEIGHTS_ADDR,
           (unsigned long)sizeof(RiskWeightsFlash_t));

    status = FLASH_Erase(&g_flashConfig,
                         FLASH_WEIGHTS_ADDR,
                         FLASH_WEIGHTS_SIZE,
                         kFLASH_ApiEraseKey);

    if (status != kStatus_Success)
    {
        PRINTF("SYS,WEIGHTS_ERASE_FAIL,status=%ld\r\n", (long)status);
        return false;
    }

    status = FLASH_Program(&g_flashConfig,
                           FLASH_WEIGHTS_ADDR,
                           (uint8_t *)&temp,
                           sizeof(RiskWeightsFlash_t));

    if (status != kStatus_Success)
    {
        PRINTF("SYS,WEIGHTS_WRITE_FAIL,status=%ld\r\n", (long)status);
        return false;
    }

    PRINTF("SYS,WEIGHTS_SAVE_OK\r\n");
    return true;
}

bool FlashLogger_LoadRiskWeights(RiskWeightsFlash_t *fw)
{
    RiskWeightsFlash_t temp;
    uint32_t expected_crc;

    if ((!g_flashDriverReady) || (fw == NULL))
    {
        return false;
    }

    if (fw == NULL)
    {
        return false;
    }

    memcpy(&temp, (const void *)FLASH_WEIGHTS_ADDR, sizeof(RiskWeightsFlash_t));

    if (temp.magic == 0xFFFFFFFFu)
    {
        PRINTF("SYS,WEIGHTS_BLANK\r\n");
        return false;
    }

    if (temp.magic != RISK_WEIGHTS_MAGIC)
    {
        PRINTF("SYS,WEIGHTS_INVALID_MAGIC\r\n");
        return false;
    }

    expected_crc = temp.crc;
    temp.crc = 0U;

    if (expected_crc != FlashLogger_WeightsCRC(&temp))
    {
        PRINTF("SYS,WEIGHTS_CRC_FAIL\r\n");
        return false;
    }

    *fw = temp;
    fw->crc = expected_crc;

    PRINTF("SYS,WEIGHTS_LOAD_OK\r\n");
    return true;
}






