// display.c
#include "display.h"
#include "logging.h"
#include <string.h>
#include "fsl_i2c.h"
#include "fsl_clock.h"
#include "fsl_port.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

// =====================
// I2C selection (matches your Config Tools: I2C1 on
//PTc10/PTC11)
// =====================
#ifndef OLED_I2C
#define OLED_I2C I2C1
#endif

#ifndef OLED_I2C_CLK_SRC
#define OLED_I2C_CLK_SRC kCLOCK_BusClk
#endif

#define OLED_BAUDRATE 100000U

// SSD1306
#define SSD1306_ADDR 0x3CU  // 7-bit addr
#define OLED_W 128
#define OLED_H 64
#define OLED_PAGES (OLED_H/8)
#define LOG_VIEW_TIMEOUT_MS 20000U

static uint8_t fb[OLED_W * OLED_PAGES]; // 1024 bytes
static bool s_hold_active = false;
static TickType_t s_hold_until_tick = 0;

static bool s_log_view_active = false;
static uint16_t s_log_view_index = 0;
static TickType_t s_log_view_until_tick = 0;

//static void draw_hms_line(uint8_t page, uint8_t h, uint8_t m, uint8_t s);
// If you init I2C in RTC_Init(), don’t re-init here.
extern bool RTC_Init(void); // optional, only if you want Display_Init() to ensure I2C is up

// ---- I2C helpers ----
static status_t oled_write_cmd(uint8_t cmd)
{
    uint8_t buf[2] = {0x00, cmd}; // 0x00 = command
    i2c_master_transfer_t xfer = {0};
    xfer.slaveAddress = SSD1306_ADDR;
    xfer.direction = kI2C_Write;
    xfer.data = buf;
    xfer.dataSize = sizeof(buf);
    xfer.flags = kI2C_TransferDefaultFlag;
    return I2C_MasterTransferBlocking(OLED_I2C, &xfer);
}


//////////////////////////////////////////////////////////////////////////////////////////////
//---------used copilot and claude for tis code section (line 60 - line 338)----------------//
//////////////////////////////////////////////////////////////////////////////////////////////


static status_t oled_write_data(const uint8_t *data, size_t len)
{
    uint8_t chunk[17]; // 1 control + 16 data
    chunk[0] = 0x40;   // 0x40 = data

    while (len)
    {
        size_t n = (len > 16) ? 16 : len;
        memcpy(&chunk[1], data, n);

        i2c_master_transfer_t xfer = {0};
        xfer.slaveAddress = SSD1306_ADDR;
        xfer.direction = kI2C_Write;
        xfer.data = chunk;
        xfer.dataSize = 1 + n;
        xfer.flags = kI2C_TransferDefaultFlag;

        status_t st = I2C_MasterTransferBlocking(OLED_I2C, &xfer);
        if (st != kStatus_Success) return st;

        data += n;
        len  -= n;
    }
    return kStatus_Success;
}

static void oled_set_cursor(uint8_t page, uint8_t col)
{
    oled_write_cmd(0xB0 | (page & 0x07));
    oled_write_cmd(0x00 | (col & 0x0F));
    oled_write_cmd(0x10 | ((col >> 4) & 0x0F));
}

// ---- Tiny font ----
static const uint8_t font5x7_space[5] = {0,0,0,0,0};
static const uint8_t font5x7_colon[5] = {0x00,0x36,0x36,0x00,0x00};

static const uint8_t font5x7_0[5] = {0x3E,0x51,0x49,0x45,0x3E};
static const uint8_t font5x7_1[5] = {0x00,0x42,0x7F,0x40,0x00};
static const uint8_t font5x7_2[5] = {0x42,0x61,0x51,0x49,0x46};
static const uint8_t font5x7_3[5] = {0x21,0x41,0x45,0x4B,0x31};
static const uint8_t font5x7_4[5] = {0x18,0x14,0x12,0x7F,0x10};
static const uint8_t font5x7_5[5] = {0x27,0x45,0x45,0x45,0x39};
static const uint8_t font5x7_6[5] = {0x3C,0x4A,0x49,0x49,0x30};
static const uint8_t font5x7_7[5] = {0x01,0x71,0x09,0x05,0x03};
static const uint8_t font5x7_8[5] = {0x36,0x49,0x49,0x49,0x36};
static const uint8_t font5x7_9[5] = {0x06,0x49,0x49,0x29,0x1E};

static const uint8_t font5x7_A[5] = {0x7E,0x11,0x11,0x11,0x7E};
static const uint8_t font5x7_B[5] = {0x7F,0x49,0x49,0x49,0x36};
static const uint8_t font5x7_C[5] = {0x3E,0x41,0x41,0x41,0x22};
static const uint8_t font5x7_D[5] = {0x7F,0x41,0x41,0x41,0x3E};
static const uint8_t font5x7_E[5] = {0x7F,0x49,0x49,0x49,0x41};
static const uint8_t font5x7_F[5] = {0x7F,0x09,0x09,0x09,0x01};
static const uint8_t font5x7_G[5] = {0x3E,0x41,0x49,0x49,0x7A};
static const uint8_t font5x7_H[5] = {0x7F,0x08,0x08,0x08,0x7F};
static const uint8_t font5x7_I[5] = {0x00,0x41,0x7F,0x41,0x00};
static const uint8_t font5x7_K[5] = {0x7F,0x08,0x14,0x22,0x41};
static const uint8_t font5x7_L[5] = {0x7F,0x40,0x40,0x40,0x40};
static const uint8_t font5x7_M[5] = {0x7F,0x02,0x0C,0x02,0x7F};
static const uint8_t font5x7_N[5] = {0x7F,0x04,0x08,0x10,0x7F};
static const uint8_t font5x7_O[5] = {0x3E,0x41,0x41,0x41,0x3E};
static const uint8_t font5x7_P[5] = {0x7F,0x09,0x09,0x09,0x06};
static const uint8_t font5x7_R[5] = {0x7F,0x09,0x19,0x29,0x46};
static const uint8_t font5x7_S[5] = {0x46,0x49,0x49,0x49,0x31};
static const uint8_t font5x7_T[5] = {0x01,0x01,0x7F,0x01,0x01};
static const uint8_t font5x7_U[5] = {0x3F,0x40,0x40,0x40,0x3F};
static const uint8_t font5x7_V[5] = {0x1F,0x20,0x40,0x20,0x1F};
static const uint8_t font5x7_W[5] = {0x7F,0x20,0x10,0x20,0x7F};
static const uint8_t font5x7_X[5] = {0x63,0x14,0x08,0x14,0x63};
static const uint8_t font5x7_Y[5] = {0x07,0x08,0x70,0x08,0x07};
static const uint8_t font5x7_Z[5] = {0x61,0x51,0x49,0x45,0x43};

static const uint8_t font5x7_e[5]     = {0x38,0x54,0x54,0x54,0x18};
static const uint8_t font5x7_d[5]     = {0x38,0x44,0x44,0x28,0x7F};
static const uint8_t font5x7_slash[5] = {0x20,0x10,0x08,0x04,0x02};

static const uint8_t* font_get(char c)
{
    switch (c)
    {
        case ' ': return font5x7_space;
        case ':': return font5x7_colon;
        case '0': return font5x7_0;
        case '1': return font5x7_1;
        case '2': return font5x7_2;
        case '3': return font5x7_3;
        case '4': return font5x7_4;
        case '5': return font5x7_5;
        case '6': return font5x7_6;
        case '7': return font5x7_7;
        case '8': return font5x7_8;
        case '9': return font5x7_9;

        case 'A': return font5x7_A;
        case 'B': return font5x7_B;
        case 'C': return font5x7_C;
        case 'D': return font5x7_D;
        case 'E': return font5x7_E;
        case 'F': return font5x7_F;
        case 'G': return font5x7_G;
        case 'H': return font5x7_H;
        case 'I': return font5x7_I;
        case 'K': return font5x7_K;
        case 'L': return font5x7_L;
        case 'M': return font5x7_M;
        case 'N': return font5x7_N;
        case 'O': return font5x7_O;
        case 'P': return font5x7_P;
        case 'R': return font5x7_R;
        case 'S': return font5x7_S;
        case 'T': return font5x7_T;
        case 'U': return font5x7_U;
        case 'V': return font5x7_V;
        case 'W': return font5x7_W;
        case 'X': return font5x7_X;
        case 'Y': return font5x7_Y;
        case 'Z': return font5x7_Z;

        case 'e': return font5x7_e;
        case 'd': return font5x7_d;
        case '/': return font5x7_slash;

        default:  return font5x7_space;
    }
}

static void oled_clear_fb(void) { memset(fb, 0, sizeof(fb)); }

static void oled_flush(void)
{
    for (uint8_t page = 0; page < OLED_PAGES; page++)
    {
        oled_set_cursor(page, 2);
        oled_write_data(&fb[page * OLED_W], OLED_W);
    }
}

static void oled_draw_text(uint8_t page, uint8_t col, const char *s)
{
    while (*s && col < (OLED_W - 6))
    {
        const uint8_t *g = font_get(*s++);
        for (int i = 0; i < 5; i++) fb[page * OLED_W + col++] = g[i];
        fb[page * OLED_W + col++] = 0x00;
    }
}

static const char* status_to_short(DoseStatus s)
{
    switch (s)
    {
        case DOSE_TAKEN:   return "TAKEN";
        case DOSE_MISSED:  return "MISSED";
        case DOSE_SNOOZED: return "SNOOZED";
        case DOSE_PENDING: return "PENDING";
        default:           return "UNK";
    }
}

static const char* log_type_to_short(LogType t)
{
    switch (t)
    {
        case LOG_TYPE_DOSE_EVENT:         return "DOSE";
        case LOG_TYPE_REMINDER_TRIGGERED: return "REM";
        case LOG_TYPE_REMINDER_ACK:       return "ACK";
        case LOG_TYPE_REMINDER_SNOOZED:   return "SNZ";
        case LOG_TYPE_PREALERT_TRIGGERED: return "PRE";
        case LOG_TYPE_DISPENSE_START:     return "DSP_ST";
        case LOG_TYPE_DISPENSE_SUCCESS:   return "DSP_OK";
        case LOG_TYPE_DISPENSE_FAIL:      return "DSP_FL";
        case LOG_TYPE_RETRY_ATTEMPT:      return "RETRY";
        case LOG_TYPE_WEIGHT_CHANGE:      return "WGT";
        case LOG_TYPE_SYSTEM:             return "SYS";
        default:                          return "UNK";
    }
}

static void draw_hms_line(uint8_t page, uint8_t h, uint8_t m, uint8_t s)
{
    char buf[12];
    buf[0] = '0' + (h / 10);
    buf[1] = '0' + (h % 10);
    buf[2] = ':';
    buf[3] = '0' + (m / 10);
    buf[4] = '0' + (m % 10);
    buf[5] = ':';
    buf[6] = '0' + (s / 10);
    buf[7] = '0' + (s % 10);
    buf[8] = 0;
    oled_draw_text(page, 0, buf);
}

static void draw_log_entry_screen(uint16_t index)
{
    LogEntry e;
    char line[22];
    uint16_t count = (uint16_t)Logging_Count();
    uint16_t year;
    uint8_t month, day, hour, minute, second;

    oled_clear_fb();

    if ((count == 0U) || !Logging_GetEvent(index, &e))
    {
        oled_draw_text(0, 0, "STORED LOG VIEW");
        oled_draw_text(2, 0, "NO LOGS");
        oled_flush();
        return;
    }

    Logging_GetDateTimeFromEntry(&e, &year, &month, &day, &hour, &minute, &second);

    oled_draw_text(0, 0, "STORED LOG VIEW");

    // Line 1: index/count
    snprintf(line, sizeof(line), "%u/%u", (unsigned)(index + 1U), (unsigned)count);
    oled_draw_text(1, 0, line);

    // Line 2: type
    strncpy(line, log_type_to_short((LogType)e.type), sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    oled_draw_text(2, 0, line);

    // Line 3: med_id
    strncpy(line, e.med_id[0] ? e.med_id : "NA", sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    oled_draw_text(3, 0, line);

    // Line 4: status
    strncpy(line, status_to_short((DoseStatus)e.status), sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    oled_draw_text(4, 0, line);

    // Line 6: time
    draw_hms_line(6, hour, minute, second);

    oled_flush();
}


bool Display_Init(void)
{
    // Ensure port clocks for the I2C1 pins are enabled (PTE0/PTE1 live on PORTC)
    CLOCK_EnableClock(kCLOCK_PortC);



    // SSD1306 init
    oled_write_cmd(0xAE);
//    oled_write_cmd(0x20); oled_write_cmd(0x00);
    oled_write_cmd(0x20); oled_write_cmd(0x02);
    oled_write_cmd(0xB0);
    oled_write_cmd(0xC8);
    oled_write_cmd(0x00);
    oled_write_cmd(0x10);
    oled_write_cmd(0x40);
    oled_write_cmd(0x81); oled_write_cmd(0x7F);
    oled_write_cmd(0xA1);
    oled_write_cmd(0xA6);
    oled_write_cmd(0xA8); oled_write_cmd(0x3F);
    oled_write_cmd(0xA4);
    oled_write_cmd(0xD3); oled_write_cmd(0x00);
    oled_write_cmd(0xD5); oled_write_cmd(0x80);
    oled_write_cmd(0xD9); oled_write_cmd(0xF1);
    oled_write_cmd(0xDA); oled_write_cmd(0x12);
    oled_write_cmd(0xDB); oled_write_cmd(0x40);
    oled_write_cmd(0x8D); oled_write_cmd(0x14);
    oled_write_cmd(0xAF);

    oled_clear_fb();
    oled_flush();
    return true;
}

void Display_ShowBoot(void)
{
    oled_clear_fb();
    oled_draw_text(1, 0, "MEDICATION");
    oled_draw_text(2, 0, "REMINDER");
    oled_draw_text(4, 0, "BOOTING...");
    oled_flush();
}

static void draw_time_line(uint8_t page, const RtcTime *t)
{
    char buf[16];
    buf[0] = '0' + (t->hour / 10);
    buf[1] = '0' + (t->hour % 10);
    buf[2] = ':';
    buf[3] = '0' + (t->minute / 10);
    buf[4] = '0' + (t->minute % 10);
    buf[5] = ':';
    buf[6] = '0' + (t->second / 10);
    buf[7] = '0' + (t->second % 10);
    buf[8] = 0;

    oled_draw_text(page, 0, "TIME ");
    oled_draw_text(page, 36, buf);
}

void Display_ShowTime(const RtcTime* t, const DoseTime* nextDose)
{
    (void)nextDose;

    oled_clear_fb();
    oled_draw_text(0, 0, "NEXT DOSE");
    if (t) draw_time_line(2, t);
    oled_draw_text(5, 0, "READY");
    oled_flush();
}

void Display_ShowReminderScreen(uint8_t h, uint8_t m)
{
    oled_clear_fb();
    oled_draw_text(1, 0, "TAKE MED");
    char buf[8];
    buf[0] = '0' + (h / 10);
    buf[1] = '0' + (h % 10);
    buf[2] = ':';
    buf[3] = '0' + (m / 10);
    buf[4] = '0' + (m % 10);
    buf[5] = 0;
    oled_draw_text(3, 0, "AT ");
    oled_draw_text(3, 18, buf);
    oled_flush();
}

void Display_ShowReminderAttempt(uint8_t attempt)
{
    char buf[16];
    oled_clear_fb();
    oled_draw_text(1, 0, "REMINDER");
    buf[0]='A'; buf[1]='T'; buf[2]='T'; buf[3]='E'; buf[4]='M'; buf[5]='P'; buf[6]='T';
    buf[7]=' '; buf[8]='0'+(attempt%10); buf[9]=0;
    oled_draw_text(3, 0, buf);
    oled_flush();
}

void Display_ShowTakenScreen(void)
{
    oled_clear_fb();
    oled_draw_text(2, 0, "TAKEN");
    oled_flush();
}

void Display_ShowMissedScreen(void)
{
    oled_clear_fb();
    oled_draw_text(2, 0, "MISSED");
    oled_flush();
}

void Display_ShowSnoozedScreen(void)
{
    oled_clear_fb();
    oled_draw_text(2, 0, "SNOOZED");
    oled_flush();
}


///////////////////////////////////////////////////////////////////////////////////////////
//--------------------------Created using ChatGpt line (431 -583)------------------------//
///////////////////////////////////////////////////////////////////////////////////////////


void Display_ShowLogAndRisk(void)
{
    LogEntry e;
    uint16_t year;
    uint8_t month, day, hour, minute, second;

    oled_clear_fb();
    oled_draw_text(0, 0, "LAST EVENT");

    if (Logging_GetEvent(0, &e))
    {
        char line[22];

        Logging_GetDateTimeFromEntry(&e, &year, &month, &day, &hour, &minute, &second);

        strncpy(line, e.med_id[0] ? e.med_id : "NA", sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        oled_draw_text(2, 0, line);

        strncpy(line, status_to_short((DoseStatus)e.status), sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        oled_draw_text(3, 0, line);

        draw_hms_line(5, hour, minute, second);

        (void)Logging_MissRateForHour(Logging_GetHourFromEntry(&e));
        oled_draw_text(7, 0, "RISK TBD");
    }
    else
    {
        oled_draw_text(2, 0, "NO LOGS");
    }

    oled_flush();
}

void Display_ShowLatestEvent(void)
{
    LogEntry e;
    char line[22];
    uint16_t year;
    uint8_t month, day, hour, minute, second;

    oled_clear_fb();
    oled_draw_text(0, 0, "LAST STORED LOG");

    if (!Logging_GetEvent(0, &e))
    {
        oled_draw_text(2, 0, "NO LOGS");
        oled_flush();
        return;
    }

    Logging_GetDateTimeFromEntry(&e, &year, &month, &day, &hour, &minute, &second);

    strncpy(line, log_type_to_short((LogType)e.type), sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    oled_draw_text(2, 0, line);

    strncpy(line, e.med_id[0] ? e.med_id : "NA", sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    oled_draw_text(2, 0, line);

    strncpy(line, status_to_short((DoseStatus)e.status), sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    oled_draw_text(4, 0, line);

    draw_hms_line(6, hour, minute, second);

    oled_flush();
}

bool Display_IsHoldActive(void)
{
    if (Display_IsLogViewerActive())
    {
        return true;
    }

    if (!s_hold_active)
    {
        return false;
    }

    if (xTaskGetTickCount() >= s_hold_until_tick)
    {
        s_hold_active = false;
        return false;
    }

    return true;
}

void Display_HoldLatestEvent(uint32_t hold_ms)
{
    Display_ShowLatestEvent();

    s_hold_active = true;
    s_hold_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(hold_ms);
}

void Display_ShowRecentLogs(uint8_t max_logs)
{
    if (max_logs == 0U)
    {
        max_logs = 1U;
    }
    if (max_logs > 3U)
    {
        max_logs = 3U;
    }

    oled_clear_fb();
    oled_draw_text(0, 0, "RECENT LOGS");

    for (uint8_t i = 0; i < max_logs; i++)
    {
        LogEntry e;
        char line[22];
        uint8_t page = (uint8_t)(2U + i);

        if (!Logging_GetEvent(i, &e))
        {
            break;
        }

        line[0] = status_to_short((DoseStatus)e.status)[0];
        line[1] = ' ';
        line[2] = '\0';

        strncat(line, e.med_id[0] ? e.med_id : "NA", sizeof(line) - strlen(line) - 1);
        oled_draw_text(page, 0, line);
    }

    oled_flush();
}

bool Display_IsLogViewerActive(void)
{
    if (!s_log_view_active)
    {
        return false;
    }

    if (xTaskGetTickCount() >= s_log_view_until_tick)
    {
        s_log_view_active = false;
        s_log_view_index = 0;
        return false;
    }

    return true;
}

void Display_ShowNextStoredLog(void)
{
    uint16_t count = (uint16_t)Logging_Count();

    if (count == 0U)
    {
        s_log_view_active = true;
        s_log_view_index = 0;
        s_log_view_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(LOG_VIEW_TIMEOUT_MS);
        draw_log_entry_screen(0);
        return;
    }

    if (!s_log_view_active)
    {
        s_log_view_active = true;
        s_log_view_index = 0;
    }
    else
    {
        s_log_view_index++;
        if (s_log_view_index >= count)
        {
            s_log_view_index = 0;
        }
    }

    s_log_view_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(LOG_VIEW_TIMEOUT_MS);
    draw_log_entry_screen(s_log_view_index);
}

void Display_ExitLogViewer(void)
{
    s_log_view_active = false;
    s_log_view_index = 0;
}





