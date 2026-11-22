#pragma once
#include <Arduino.h>

// Stopwatch + countdown timer. All state is millis()-based so it keeps running
// correctly while the screen sleeps. The watch owns the state; the app mirrors
// it over BLE (chars b1 command / b2 state) and either side can drive it.

void timers_init();

// ── Stopwatch ─────────────────────────────────────────────────────────
void     sw_start();
void     sw_stop();
void     sw_reset();
bool     sw_is_running();
uint32_t sw_elapsed_ms();

// ── Countdown timer ───────────────────────────────────────────────────
void     ct_set(uint32_t ms);      // set duration (also resets remaining)
void     ct_start();
void     ct_stop();
void     ct_reset();
bool     ct_is_running();
uint32_t ct_remaining_ms();
uint32_t ct_duration_ms();
bool     ct_alarming();            // true from when it hits 0 until acknowledged
void     ct_ack();                 // stop the alarm + rearm to the set duration

// Call every loop tick: latches the alarm when the countdown reaches 0.
// Returns true on the single tick the alarm starts firing.
bool     timers_service();
