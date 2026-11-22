#pragma once
#include <Arduino.h>

// One-byte settings exposed as READ + WRITE + NOTIFY characteristics.
// Changes made on the watch (touch) are pushed to the phone and vice versa.
enum BleSetting : uint8_t {
    SET_ROTATION = 0,  // a9  0-3   screen rotation x 90 deg
    SET_BRIGHTNESS,    // aa  5-100 backlight %
    SET_TIME_FORMAT,   // ab  0-1   0 = 24 h, 1 = 12 h (AM/PM)
    SET_FACE,          // ac  0-1   0 = digital, 1 = pendant (analog)
    SET_SLEEP,         // ad  0-255 screen sleep after N seconds, 0 = always on
    SET_COUNT
};

// Start the GATT server + advertising with the values loaded from flash so
// the phone reads the real settings right after connecting.
void ble_init(uint32_t goal, uint8_t sens, const uint8_t settings[SET_COUNT]);
bool ble_is_connected();

// Update readable values; notify the client if it subscribed.
void ble_notify_steps(uint32_t steps);
void ble_notify_imu(int16_t ax_mg, int16_t ay_mg, int16_t az_mg, int16_t mag_mg);
void ble_notify_battery(uint8_t pct, uint16_t battery_mv);
void ble_notify_clock(const uint8_t pkt[8]);   // [yr-1900, mon0, mday, h, m, s, wday, valid]

void ble_set_goal_value(uint32_t goal);
void ble_set_sens_value(uint8_t sens);
void ble_set_setting(BleSetting s, uint8_t value);   // READ value + notify

// Pending writes from the client — consumed once each loop tick.
bool ble_pop_pending_goal(uint32_t *out);
bool ble_pop_pending_sensitivity(uint8_t *out);
bool ble_pop_pending_time(uint8_t buf[7]);  // 7-byte BLE time packet
bool ble_pop_reset_request();
bool ble_pop_setting(BleSetting s, uint8_t *out);

// Face layout (characteristic ae, READ + WRITE + NOTIFY).
// Full value = for face 0..1, element 0..N-1: [face, id, visible, x, y, size, color].
void ble_set_layout(const uint8_t *buf, size_t len);
bool ble_pop_layout_element(uint8_t out[7]);   // one element edit from the app
bool ble_pop_layout_reset();                   // app asked for default layouts

// Navigation (characteristic af, WRITE / WRITE NO RESPONSE), little endian:
//   [0] active 0/1   [1] maneuver (NavManeuver)
//   [2..5] metres to next maneuver   [6..9] metres to destination
//   [10] ETA hour  [11] ETA minute   [12..13] minutes to destination
//   [14] text length N   [15..] ASCII instruction (N <= 120)
// active = 0 ends navigation. No packet for 20 s also ends it.
enum NavManeuver : uint8_t {
    NAV_STRAIGHT = 0, NAV_SLIGHT_LEFT, NAV_LEFT, NAV_SHARP_LEFT,
    NAV_SLIGHT_RIGHT, NAV_RIGHT, NAV_SHARP_RIGHT, NAV_UTURN,
    NAV_ARRIVE, NAV_DEPART, NAV_ROUNDABOUT, NAV_MANEUVER_COUNT
};
#define NAV_MAX_LEN 160
size_t ble_pop_nav(uint8_t *out);   // returns packet length, 0 if none

// Stopwatch / countdown timer (characteristic b1 = command, b2 = state).
//   cmd (W): [op] or [op, u32 ms little-endian].
//     0x01 sw-start  0x02 sw-stop  0x03 sw-reset
//     0x10 ct-start  0x11 ct-stop  0x12 ct-reset  0x13+u32 ct-set  0x14 ct-ack
//   state (R N) 15 bytes:
//     [0] sw_running  [1..4] sw_elapsed_ms  [5] ct_running
//     [6..9] ct_remaining_ms  [10..13] ct_duration_ms  [14] flags (bit0 alarming)
size_t ble_pop_timer_cmd(uint8_t *out);     // returns length, 0 if none pending
void   ble_timer_notify_state(const uint8_t *buf, size_t len);
