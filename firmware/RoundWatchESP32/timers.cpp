/*
 * timers.cpp — stopwatch + countdown timer logic (millis-based).
 * No display or BLE here; the .ino reads these getters to draw the overlay
 * and to publish state over BLE.
 */
#include "timers.h"

// Stopwatch
static bool     s_sw_run   = false;
static uint32_t s_sw_accum = 0;        // accumulated ms while paused
static uint32_t s_sw_start = 0;        // millis() when (re)started

// Countdown
static bool     s_ct_run   = false;
static uint32_t s_ct_dur   = 60000;    // set duration, default 1 min
static uint32_t s_ct_rem   = 60000;    // remaining when paused
static uint32_t s_ct_start = 0;        // millis() when started
static bool     s_ct_alarm = false;

void timers_init() {}

// ── Stopwatch ─────────────────────────────────────────────────────────
void sw_start() { if (!s_sw_run) { s_sw_run = true; s_sw_start = millis(); } }
void sw_stop()  { if (s_sw_run) { s_sw_accum += millis() - s_sw_start; s_sw_run = false; } }
void sw_reset() { s_sw_run = false; s_sw_accum = 0; }
bool sw_is_running() { return s_sw_run; }
uint32_t sw_elapsed_ms() { return s_sw_accum + (s_sw_run ? millis() - s_sw_start : 0); }

// ── Countdown ─────────────────────────────────────────────────────────
void ct_set(uint32_t ms) {
    if (ms < 1000) ms = 1000;
    if (ms > 86400000UL) ms = 86400000UL;     // cap at 24 h
    s_ct_dur = ms;
    s_ct_rem = ms;
    s_ct_run = false;
    s_ct_alarm = false;
}

void ct_start() {
    if (s_ct_run) return;
    if (s_ct_rem == 0) s_ct_rem = s_ct_dur;    // rearm if it had elapsed
    s_ct_alarm = false;
    s_ct_run = true;
    s_ct_start = millis();
}

void ct_stop() {
    if (!s_ct_run) return;
    uint32_t e = millis() - s_ct_start;
    s_ct_rem = (e >= s_ct_rem) ? 0 : s_ct_rem - e;
    s_ct_run = false;
}

void ct_reset() { s_ct_run = false; s_ct_rem = s_ct_dur; s_ct_alarm = false; }
bool ct_is_running() { return s_ct_run; }
uint32_t ct_duration_ms() { return s_ct_dur; }

uint32_t ct_remaining_ms() {
    if (!s_ct_run) return s_ct_rem;
    uint32_t e = millis() - s_ct_start;
    return (e >= s_ct_rem) ? 0 : s_ct_rem - e;
}

bool ct_alarming() { return s_ct_alarm; }
void ct_ack() { s_ct_alarm = false; s_ct_run = false; s_ct_rem = s_ct_dur; }

bool timers_service() {
    if (s_ct_run) {
        uint32_t e = millis() - s_ct_start;
        if (e >= s_ct_rem) {
            s_ct_rem = 0;
            s_ct_run = false;
            if (!s_ct_alarm) { s_ct_alarm = true; return true; }
        }
    }
    return false;
}
