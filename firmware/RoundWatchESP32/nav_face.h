#pragma once
#include <Arduino.h>
#include <lvgl.h>

// Turn-by-turn navigation face (shown over the watch face while navigating).
void nav_create(lv_obj_t *screen);

// Apply a navigation packet (see ble_service.h). Returns true if navigation
// is active afterwards. h12 selects 12-hour ETA.
bool nav_apply(const uint8_t *pkt, size_t len, bool h12);

void nav_set_visible(bool on);
