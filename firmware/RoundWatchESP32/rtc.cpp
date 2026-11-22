/*
 * PCF8563 RTC driver — minimal direct I2C implementation.
 * I2C address 0x51, shared Wire bus with CST816S touch.
 * All time registers are BCD-encoded.
 */
#include "rtc.h"
#include <Wire.h>

#define PCF8563_ADDR  0x51
#define REG_CONTROL1  0x00
#define REG_CONTROL2  0x01
#define REG_SECONDS   0x02  // bit7=VL (voltage-low), bits6:0=BCD seconds
#define REG_MINUTES   0x03  // bits6:0=BCD minutes
#define REG_HOURS     0x04  // bits5:0=BCD hours (24h mode)
#define REG_DAYS      0x05  // bits5:0=BCD days
#define REG_WEEKDAYS  0x06  // bits2:0=weekday 0-6
#define REG_MONTHS    0x07  // bit7=century, bits4:0=BCD months
#define REG_YEARS     0x08  // bits7:0=BCD years (00-99, relative to 2000)

static uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static uint8_t dec2bcd(uint8_t d) { return ((d / 10) << 4) | (d % 10); }

static bool rtc_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(PCF8563_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool rtc_read_burst(uint8_t reg, uint8_t *buf, size_t len) {
    Wire.beginTransmission(PCF8563_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom((uint8_t)PCF8563_ADDR, (uint8_t)len);
    for (size_t i = 0; i < len; i++) {
        if (!Wire.available()) return false;
        buf[i] = Wire.read();
    }
    return true;
}

void rtc_init() {
    // Clear STOP bit and TEST1 bit, enable clock
    rtc_write(REG_CONTROL1, 0x00);
    rtc_write(REG_CONTROL2, 0x00);
    Serial.println("[RTC] PCF8563 init");
}

bool rtc_get(RtcTime *out) {
    uint8_t r[7];
    if (!rtc_read_burst(REG_SECONDS, r, 7)) return false;

    bool vl = (r[0] & 0x80) != 0;  // VL = voltage low (time may be invalid)
    out->second  = bcd2dec(r[0] & 0x7F);
    out->minute  = bcd2dec(r[1] & 0x7F);
    out->hour    = bcd2dec(r[2] & 0x3F);
    out->day     = bcd2dec(r[3] & 0x3F);
    out->weekday = r[4] & 0x07;
    out->month   = bcd2dec(r[5] & 0x1F);
    // PCF8563 year is 0-99 relative to 2000; century bit selects 1900/2000
    uint8_t century = (r[5] & 0x80) ? 1 : 0;
    out->year    = 2000 + bcd2dec(r[6]) - (century ? 100 : 0);

    return !vl;
}

void rtc_set_from_ble(const uint8_t buf[7]) {
    // BLE packet: [tm_year (since 1900), tm_mon (0-11), mday, hour, min, sec, wday]
    uint16_t year  = 1900U + buf[0];
    uint8_t  month = buf[1] + 1;  // convert 0-11 -> 1-12
    uint8_t  day   = buf[2];
    uint8_t  hour  = buf[3];
    uint8_t  min   = buf[4];
    uint8_t  sec   = buf[5];
    uint8_t  wday  = buf[6] & 0x07;

    // PCF8563 stores 2-digit year; years 2000-2099 use century=0
    uint8_t yr2 = (uint8_t)(year - 2000);
    uint8_t century = 0;
    if (year < 2000) { yr2 = (uint8_t)(year - 1900); century = 1; }

    rtc_write(REG_SECONDS,  dec2bcd(sec)   & 0x7F);  // clears VL
    rtc_write(REG_MINUTES,  dec2bcd(min));
    rtc_write(REG_HOURS,    dec2bcd(hour)  & 0x3F);
    rtc_write(REG_DAYS,     dec2bcd(day));
    rtc_write(REG_WEEKDAYS, wday);
    rtc_write(REG_MONTHS,   dec2bcd(month) | (century ? 0x80 : 0x00));
    rtc_write(REG_YEARS,    dec2bcd(yr2));

    Serial.printf("[RTC] set %04u-%02u-%02u %02u:%02u:%02u\n",
                  year, month, day, hour, min, sec);
}

int32_t rtc_day_pack(const RtcTime *t) {
    return ((int32_t)t->year << 16) | ((int32_t)t->month << 8) | t->day;
}
