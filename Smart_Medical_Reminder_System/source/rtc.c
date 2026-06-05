// rtc.c
#include "rtc.h"
#include <string.h>
#include "fsl_i2c.h"
#include "fsl_clock.h"
#include "fsl_port.h"
#include "fsl_debug_console.h"


// =====================
// I2C selection (matches your Config Tools: I2C1 on
//PTC10/PTC11)
// =====================
#ifndef DS3231_I2C
#define DS3231_I2C I2C1
#endif

#ifndef DS3231_I2C_CLK_SRC
#define DS3231_I2C_CLK_SRC kCLOCK_BusClk
#endif

#define DS3231_BAUDRATE 100000U

#define DS3231_ADDR        0x68U
#define DS3231_REG_SECONDS 0x00U



///////////////////////////////////////////////////////////////////////////
//-------------------WROTE static helpers with help of chatGpt-----------//
///////////////////////////////////////////////////////////////////////////



static bool s_i2c_inited = false;

static inline uint8_t bcd_to_dec(uint8_t bcd) { return (uint8_t)(((bcd >> 4) * 10U) + (bcd & 0x0FU)); }
static inline uint8_t dec_to_bcd(uint8_t dec) { return (uint8_t)(((dec / 10U) << 4) | (dec % 10U)); }

static void i2c1_init_once(void)
{
    if (s_i2c_inited) return;

    // PTC10/PTC11 are on PORTC
    CLOCK_EnableClock(kCLOCK_PortC);

    i2c_master_config_t cfg;
    I2C_MasterGetDefaultConfig(&cfg);
    cfg.baudRate_Bps = DS3231_BAUDRATE;

    const uint32_t i2cClk = CLOCK_GetFreq(DS3231_I2C_CLK_SRC);
    I2C_MasterInit(DS3231_I2C, &cfg, i2cClk);

    s_i2c_inited = true;
}

static status_t ds3231_write_reg(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[1 + 8];
    if (len > 8) return kStatus_InvalidArgument;

    buf[0] = reg;
    memcpy(&buf[1], data, len);

    i2c_master_transfer_t xfer = {0};
    xfer.slaveAddress   = DS3231_ADDR;
    xfer.direction      = kI2C_Write;
    xfer.data           = buf;
    xfer.dataSize       = 1 + len;
    xfer.flags          = kI2C_TransferDefaultFlag;

    return I2C_MasterTransferBlocking(DS3231_I2C, &xfer);
}

static status_t ds3231_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    i2c_master_transfer_t xfer = {0};
    xfer.slaveAddress   = DS3231_ADDR;
    xfer.direction      = kI2C_Read;
    xfer.subaddress     = reg;
    xfer.subaddressSize = 1;
    xfer.data           = data;
    xfer.dataSize       = len;
    xfer.flags          = kI2C_TransferDefaultFlag;

    return I2C_MasterTransferBlocking(DS3231_I2C, &xfer);
}

bool RTC_AppInit(void)
{
    i2c1_init_once();

    // Sanity read: seconds register
    uint8_t sec = 0;
    return (ds3231_read_reg(DS3231_REG_SECONDS, &sec, 1) == kStatus_Success);

    RtcTime t;

        // __TIME__ format: "HH:MM:SS"
        t.hour   = (__TIME__[0]-'0')*10 + (__TIME__[1]-'0');
        t.minute = (__TIME__[3]-'0')*10 + (__TIME__[4]-'0');
        t.second = (__TIME__[6]-'0')*10 + (__TIME__[7]-'0');

        t.dow = 1; // not important for your project

        RTC_SetTime(&t);

        return true;
}



/////////////////////////////////////
//-----PUBLIC FUNCTIONS------------//
/////////////////////////////////////


bool RTC_GetTime(RtcTime *out)
{
    if (!out) return false;
    i2c1_init_once();

    uint8_t raw[7] = {0}; // sec, min, hour, dow, day, month, year
    if (ds3231_read_reg(DS3231_REG_SECONDS, raw, sizeof(raw)) != kStatus_Success)
    {
        return false;
    }

    out->second = bcd_to_dec(raw[0] & 0x7F);
    out->minute = bcd_to_dec(raw[1] & 0x7F);

    {
        uint8_t hr = raw[2];
        if (hr & 0x40)
        {
            uint8_t h12 = bcd_to_dec(hr & 0x1F);
            bool pm = (hr & 0x20) != 0;
            out->hour = (uint8_t)((h12 % 12) + (pm ? 12 : 0));
        }
        else
        {
            out->hour = bcd_to_dec(hr & 0x3F);
        }
    }

    out->dow   = bcd_to_dec(raw[3] & 0x07);      // 1..7
    out->day   = bcd_to_dec(raw[4] & 0x3F);      // 1..31
    out->month = bcd_to_dec(raw[5] & 0x1F);      // 1..12
    out->year  = (uint16_t)(2000U + bcd_to_dec(raw[6])); // DS3231 stores 00..99

    return true;
}



bool RTC_SetTime(const RtcTime *in)
{
    if (!in) return false;
    i2c1_init_once();

    uint8_t raw[7];
    raw[0] = dec_to_bcd(in->second);
    raw[1] = dec_to_bcd(in->minute);
    raw[2] = dec_to_bcd(in->hour);               // 24h mode
    raw[3] = dec_to_bcd(in->dow);                // 1..7
    raw[4] = dec_to_bcd(in->day);                // 1..31
    raw[5] = dec_to_bcd(in->month);              // 1..12
    raw[6] = dec_to_bcd((uint8_t)(in->year % 100U));

    return (ds3231_write_reg(DS3231_REG_SECONDS, raw, sizeof(raw)) == kStatus_Success);
}

