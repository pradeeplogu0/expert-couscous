#pragma once
#include <Arduino.h>
#include <lvgl.h>

// Full-screen stopwatch / countdown overlay with on-screen touch buttons.
// Built hidden; long-press on the watch face toggles it (see the .ino).
void tf_create(lv_obj_t *screen);
void tf_set_visible(bool on);
bool tf_visible();
void tf_tick();     // refresh the display from the timers module
