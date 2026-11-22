/*
 * timer_face.cpp — stopwatch / countdown overlay for the round screen.
 * Two tabs (Stopwatch / Timer), a big time read-out, Start-Stop + Reset, and
 * 1/5/10-minute presets for the timer. Buttons call straight into timers.cpp;
 * the .ino publishes the resulting state over BLE so the app stays in sync.
 */
#include "timer_face.h"
#include "timers.h"

enum { A_MODE_SW = 0, A_MODE_CT, A_SS, A_RESET, A_P1, A_P5, A_P10 };

#define COL_BTN   0x1B2734
#define COL_ACC   0x3D8BFD
#define COL_BAD   0xFF6B6B

static lv_obj_t *s_layer;
static lv_obj_t *s_time;
static lv_obj_t *s_tab_sw, *s_tab_ct;
static lv_obj_t *s_ss, *s_reset;
static lv_obj_t *s_presets[3];
static int       s_mode  = 0;      // 0 stopwatch, 1 countdown
static bool      s_blink = false;

static void btn_cb(lv_event_t *e) {
    int a = (int)(intptr_t)lv_event_get_user_data(e);
    switch (a) {
    case A_MODE_SW: s_mode = 0; break;
    case A_MODE_CT: s_mode = 1; break;
    case A_SS:
        if (s_mode == 0) {
            if (sw_is_running()) sw_stop(); else sw_start();
        } else if (ct_alarming()) {
            ct_ack();
        } else if (ct_is_running()) {
            ct_stop();
        } else {
            ct_start();
        }
        break;
    case A_RESET:
        if (s_mode == 0) sw_reset(); else ct_reset();
        break;
    case A_P1:  ct_set(60000UL);  break;
    case A_P5:  ct_set(300000UL); break;
    case A_P10: ct_set(600000UL); break;
    }
    tf_tick();
}

static lv_obj_t *mk_btn(lv_obj_t *p, int x, int y, int w, int h,
                        const char *txt, int action, const lv_font_t *f) {
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_set_size(b, w, h);
    lv_obj_align(b, LV_ALIGN_CENTER, x, y);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_BTN), 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)action);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_center(l);
    return b;
}

void tf_create(lv_obj_t *screen) {
    s_layer = lv_obj_create(screen);
    lv_obj_remove_style_all(s_layer);
    lv_obj_set_size(s_layer, 240, 240);
    lv_obj_set_pos(s_layer, 0, 0);
    lv_obj_set_style_bg_color(s_layer, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_layer, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_SCROLLABLE);

    s_tab_sw = mk_btn(s_layer, -42, -84, 76, 28, "Stopwatch", A_MODE_SW, &lv_font_montserrat_14);
    s_tab_ct = mk_btn(s_layer,  42, -84, 76, 28, "Timer",     A_MODE_CT, &lv_font_montserrat_14);

    s_time = lv_label_create(s_layer);
    lv_obj_set_style_text_font(s_time, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_time, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_time, "00:00.00");
    lv_obj_align(s_time, LV_ALIGN_CENTER, 0, -14);

    s_ss    = mk_btn(s_layer, -50, 52, 92, 34, "Start", A_SS,    &lv_font_montserrat_16);
    s_reset = mk_btn(s_layer,  50, 52, 92, 34, "Reset", A_RESET, &lv_font_montserrat_16);

    s_presets[0] = mk_btn(s_layer, -48, 94, 44, 26, "1m",  A_P1,  &lv_font_montserrat_14);
    s_presets[1] = mk_btn(s_layer,   0, 94, 44, 26, "5m",  A_P5,  &lv_font_montserrat_14);
    s_presets[2] = mk_btn(s_layer,  48, 94, 44, 26, "10m", A_P10, &lv_font_montserrat_14);

    lv_obj_add_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
}

void tf_set_visible(bool on) {
    if (!s_layer) return;
    if (on) { lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(s_layer); tf_tick(); }
    else    lv_obj_add_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
}

bool tf_visible() {
    return s_layer && !lv_obj_has_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
}

static void fmt_sw(char *out, size_t n, uint32_t ms) {
    unsigned long cs = (ms % 1000) / 10, s = (ms / 1000) % 60,
                  m = (ms / 60000) % 60, h = ms / 3600000UL;
    if (h) snprintf(out, n, "%lu:%02lu:%02lu", h, m, s);
    else   snprintf(out, n, "%02lu:%02lu.%02lu", m, s, cs);
}

static void fmt_ct(char *out, size_t n, uint32_t ms) {
    unsigned long total = (ms + 999) / 1000;    // round up seconds
    unsigned long s = total % 60, m = (total / 60) % 60, h = total / 3600;
    if (h) snprintf(out, n, "%lu:%02lu:%02lu", h, m, s);
    else   snprintf(out, n, "%02lu:%02lu", m, s);
}

void tf_tick() {
    if (!s_layer) return;
    char buf[16];
    bool alarm = ct_alarming();

    if (s_mode == 0) fmt_sw(buf, sizeof buf, sw_elapsed_ms());
    else             fmt_ct(buf, sizeof buf, ct_remaining_ms());

    if (alarm) {
        s_blink = !s_blink;
        lv_label_set_text(s_time, s_blink ? "TIME!" : buf);
        lv_obj_set_style_text_color(s_time, lv_color_hex(s_blink ? COL_BAD : 0xFFFFFF), 0);
    } else {
        lv_label_set_text(s_time, buf);
        lv_obj_set_style_text_color(s_time, lv_color_hex(0xFFFFFF), 0);
    }
    lv_obj_set_style_text_font(s_time,
        strlen(buf) > 8 ? &lv_font_montserrat_20 : &lv_font_montserrat_48, 0);

    bool run = (s_mode == 0) ? sw_is_running() : ct_is_running();
    lv_label_set_text(lv_obj_get_child(s_ss, 0), (run || alarm) ? "Stop" : "Start");

    lv_obj_set_style_bg_color(s_tab_sw, lv_color_hex(s_mode == 0 ? COL_ACC : COL_BTN), 0);
    lv_obj_set_style_bg_color(s_tab_ct, lv_color_hex(s_mode == 1 ? COL_ACC : COL_BTN), 0);

    for (int i = 0; i < 3; i++) {
        if (s_mode == 1) lv_obj_clear_flag(s_presets[i], LV_OBJ_FLAG_HIDDEN);
        else             lv_obj_add_flag(s_presets[i], LV_OBJ_FLAG_HIDDEN);
    }
}
