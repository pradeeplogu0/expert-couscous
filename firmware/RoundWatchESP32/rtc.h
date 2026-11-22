#pragma once
#include <Arduino.h>

struct RtcTime {
    uint16_t year;   // e.g. 2026
    uint8_t  month;  // 1-12
    uint8_t  day;    // 1-31
    uint8_t  hour;   // 0-23
    uint8_t  minute; // 0-59
    uint8_t  second; // 0-59
    uint8_t  weekday;// 0-6
};

void rtc_init();

// Returns false if the RTC has lost power / time is invalid.
bool rtc_get(RtcTime *out);

// buf = 7-byte BLE time packet [tm_year-1900, tm_mon(0-11), mday, hour, min, sec, wday]
void rtc_set_from_ble(const uint8_t buf[7]);

// Packed day value for rollover detection: (year<<16)|(month<<8)|day
int32_t rtc_day_pack(const RtcTime *t);
