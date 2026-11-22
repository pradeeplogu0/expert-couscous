/*
 * RoundWatchESP32 — customisable pendant / watch
 * Seeed Studio XIAO ESP32-S3 (Sense) + Round Display for XIAO
 * (same hardware as ClawdometerRound)
 * Arduino IDE / arduino-cli
 *
 * Two faces, both fully customisable from the iOS "Watch Lab" app
 * (Apple-Watch-style face designer):
 *   Digital  — battery ring + text elements
 *   Pendant  — analog hands + gold ticks + text elements
 * Elements: time, seconds, AM/PM, date, battery %, battery volts, rotation,
 * Bluetooth, steps, battery ring. Each has visible / x / y / size / colour,
 * stored per face in flash and edited live over BLE (characteristic ae).
 *
 * Touch: tap wakes the screen; long-press rotates it 90 degrees.
 * Screen sleep (pendant battery saver): off after N seconds of no touch.
 *
 * ── Required libraries ────────────────────────────────────────────────
 *   "Seeed Arduino Round display" (Seeed Studio), "lvgl" v8.3.x
 *   (BLE and Preferences are part of the ESP32 Arduino core)
 * ── Board: XIAO_ESP32S3, PSRAM: OPI, Partition: Default 8MB / Huge APP
 * ── lv_conf.h: LV_FONT_MONTSERRAT_14 / 16 / 20 / 48 = 1
 */

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <math.h>
#define USE_TFT_ESPI_LIBRARY
#include "lv_xiao_round_screen.h"   // Seeed Round Display: display + LVGL init
#include "imu.h"
#include "ble_service.h"
#include "rtc.h"
#include "nav_face.h"
#include "timers.h"
#include "timer_face.h"

// ── Hardware ─────────────────────────────────────────────────────────
#define BAT_ADC_PIN      D0          // Round Display battery divider (1:2)
#define BAT_ADC_SAMPLES  20
#define PIN_BACKLIGHT    D6
#define BL_PWM_FREQ      5000
#define BL_PWM_RES       8

#define COL_GOLD   0xD4AF37
#define COL_MUTED  0x7A8794
#define COL_OK     0x3DD68C
#define COL_WARN   0xF5C542
#define COL_BAD    0xFF6B6B

// ── Face layout model (must match the app) ───────────────────────────
enum ElId : uint8_t {
    EL_TIME = 0, EL_SECONDS, EL_AMPM, EL_DATE, EL_BATPCT, EL_BATV,
    EL_ROT, EL_BLE, EL_STEPS, EL_RING, EL_COUNT
};
enum { FACE_DIGITAL = 0, FACE_PENDANT = 1, FACE_COUNT = 2 };

struct El { uint8_t vis; int8_t x, y; uint8_t size, color; };

// size 0-3 → font (text) or arc width (ring)
static const lv_font_t *FONTS[4] = {&lv_font_montserrat_14, &lv_font_montserrat_16,
                                    &lv_font_montserrat_20, &lv_font_montserrat_48};
static const uint8_t RING_W[4] = {4, 8, 14, 20};
// colour 0 = auto (battery level / BLE state / white)
static const uint32_t PALETTE[12] = {0, 0xFFFFFF, 0xC0D0E0, 0x7A8794, 0xD4AF37, 0x3DD68C,
                                     0x4DABF7, 0xFF8C5A, 0xF5C542, 0xFF6B6B, 0xB197FC, 0x22D3EE};

static const El DEFAULT_LAYOUT[FACE_COUNT][EL_COUNT] = {
    {   // Digital
        {1,   0,  -4, 3, 1},   // time      XL white
        {1,   0,  32, 1, 3},   // seconds   M  muted
        {1,   0, -44, 0, 4},   // AM/PM     S  gold
        {1,   0, -76, 1, 2},   // date      M  light
        {1, -36,  56, 0, 0},   // battery % S  auto
        {1,  36,  56, 0, 7},   // battery V S  orange
        {1,   0,  76, 0, 8},   // rotation  S  yellow
        {1,   0,  96, 0, 0},   // bluetooth S  auto
        {0,   0, -60, 0, 6},   // steps     hidden
        {1,   0,   0, 2, 0},   // ring      thick, auto
    },
    {   // Pendant
        {0,   0, -66, 1, 1},   // time      hidden
        {0,   0,  66, 0, 3},   // seconds   hidden
        {1,   0,  64, 0, 4},   // AM/PM     gold
        {1,   0,  46, 0, 4},   // date      gold
        {1,   0, -46, 0, 0},   // battery % auto
        {0,   0, -28, 0, 7},   // battery V hidden
        {0,   0,  28, 0, 8},   // rotation  hidden
        {1,   0,  84, 0, 0},   // bluetooth auto
        {0,   0, -28, 0, 6},   // steps     hidden
        {1,   0,   0, 0, 0},   // ring      thin, auto
    },
};

static El g_lay[FACE_COUNT][EL_COUNT];

// ── Persisted state ──────────────────────────────────────────────────
static Preferences g_prefs;
static uint32_t    g_goal        = 8000;
static int32_t     g_saved_day   = -1;
static uint8_t     g_set[SET_COUNT] = {0, 100, 0, 0, 0};   // see BleSetting

static const char   *SET_KEYS[SET_COUNT]    = {"rot", "bright", "fmt", "face", "sleep"};
static const uint8_t SET_DEFAULT[SET_COUNT] = {0, 100, 0, 0, 0};
static const uint8_t SET_LO[SET_COUNT]      = {0, 5, 0, 0, 0};
static const uint8_t SET_HI[SET_COUNT]      = {3, 100, 1, 1, 255};
#define LAYOUT_VERSION 1

static void settings_load() {
    g_prefs.begin("pk", true);
    g_goal      = g_prefs.getUInt("goal", 8000);
    g_saved_day = g_prefs.getInt("day",  -1);
    for (int i = 0; i < SET_COUNT; i++) {
        uint8_t v = g_prefs.getUChar(SET_KEYS[i], SET_DEFAULT[i]);
        g_set[i] = (v < SET_LO[i] || v > SET_HI[i]) ? SET_DEFAULT[i] : v;
    }
    bool lay_ok = g_prefs.getUChar("layv", 0) == LAYOUT_VERSION &&
                  g_prefs.getBytesLength("lay") == sizeof g_lay &&
                  g_prefs.getBytes("lay", g_lay, sizeof g_lay) == sizeof g_lay;
    g_prefs.end();
    if (!lay_ok) memcpy(g_lay, DEFAULT_LAYOUT, sizeof g_lay);
    if (g_goal < 1000 || g_goal > 50000) g_goal = 8000;
}

static void prefs_put_u32(const char *k, uint32_t v) {
    g_prefs.begin("pk", false); g_prefs.putUInt(k, v); g_prefs.end();
}

static void day_save(int32_t d) {
    g_saved_day = d;
    g_prefs.begin("pk", false); g_prefs.putInt("day", d); g_prefs.end();
}

static uint32_t g_layout_dirty_ms = 0;   // debounce flash writes while dragging

static void layout_save_now() {
    g_prefs.begin("pk", false);
    g_prefs.putBytes("lay", g_lay, sizeof g_lay);
    g_prefs.putUChar("layv", LAYOUT_VERSION);
    g_prefs.end();
    g_layout_dirty_ms = 0;
}

static void layout_publish() {
    uint8_t buf[FACE_COUNT * EL_COUNT * 7];
    size_t n = 0;
    for (int f = 0; f < FACE_COUNT; f++) {
        for (int i = 0; i < EL_COUNT; i++) {
            const El &e = g_lay[f][i];
            buf[n++] = f; buf[n++] = i; buf[n++] = e.vis;
            buf[n++] = (uint8_t)e.x; buf[n++] = (uint8_t)e.y;
            buf[n++] = e.size; buf[n++] = e.color;
        }
    }
    ble_set_layout(buf, n);
}

// ── Battery ───────────────────────────────────────────────────────────
static int g_bat_pct = -1;
static int g_bat_mv  = 0;

// Typical 1-cell LiPo resting-voltage curve (mV → %), linear between points.
static const int16_t LIPO_MV[]  = {4200, 4100, 4000, 3900, 3800, 3750, 3700, 3650, 3600, 3500, 3300};
static const int8_t  LIPO_PCT[] = { 100,   90,   78,   65,   50,   40,   30,   20,   12,    5,    0};

static int lipo_pct(int mv) {
    if (mv >= LIPO_MV[0]) return 100;
    for (int i = 1; i < (int)(sizeof LIPO_MV / sizeof LIPO_MV[0]); i++) {
        if (mv >= LIPO_MV[i]) {
            return LIPO_PCT[i] + (mv - LIPO_MV[i]) * (LIPO_PCT[i - 1] - LIPO_PCT[i]) /
                                 (LIPO_MV[i - 1] - LIPO_MV[i]);
        }
    }
    return 0;
}

static void battery_sample() {
    int32_t sum = 0;
    for (int i = 0; i < BAT_ADC_SAMPLES; i++) sum += analogReadMilliVolts(BAT_ADC_PIN);
    int bat_mv = (int)(sum / BAT_ADC_SAMPLES) * 2;
    g_bat_mv  = (g_bat_mv == 0) ? bat_mv : (g_bat_mv * 3 + bat_mv + 2) / 4;
    g_bat_pct = lipo_pct(g_bat_mv);
}

static uint32_t bat_color() {
    return g_bat_pct > 30 ? COL_OK : g_bat_pct > 15 ? COL_WARN : COL_BAD;
}

// ── UI objects ────────────────────────────────────────────────────────
static lv_obj_t *g_layer[FACE_COUNT];
static lv_obj_t *g_obj[FACE_COUNT][EL_COUNT];      // labels; arc for EL_RING
static lv_obj_t *g_hand_h, *g_hand_m, *g_hand_s;
static lv_point_t g_pts_h[2], g_pts_m[2], g_pts_s[2];
static lv_point_t g_tick_pts[12][2];

static volatile bool g_timer_toggle_request = false;
static bool     g_asleep  = false;
static uint32_t g_wake_ms = 0;

static void on_long_press(lv_event_t *) {
    if (g_asleep || millis() - g_wake_ms < 800) return;   // first touch only wakes
    g_timer_toggle_request = true;                        // open/close stopwatch
}

static lv_obj_t *make_layer(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, 240, 240);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *make_ring(lv_obj_t *parent, int size, uint32_t track) {
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, size, size);
    lv_obj_center(a);
    lv_arc_set_rotation(a, 135);
    lv_arc_set_bg_angles(a, 0, 270);
    lv_arc_set_range(a, 0, 1000);
    lv_arc_set_value(a, 0);
    lv_obj_set_style_arc_color(a, lv_color_hex(track), LV_PART_MAIN);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    return a;
}

static lv_obj_t *make_hand(lv_obj_t *parent, lv_point_t *pts, int width, uint32_t color) {
    lv_obj_t *l = lv_line_create(parent);
    pts[0].x = pts[1].x = 120;
    pts[0].y = pts[1].y = 120;
    lv_line_set_points(l, pts, 2);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    return l;
}

static void create_face(int f) {
    g_layer[f] = make_layer(lv_scr_act());
    g_obj[f][EL_RING] = make_ring(g_layer[f], f == FACE_PENDANT ? 236 : 220,
                                  f == FACE_PENDANT ? 0x10161E : 0x18222E);

    if (f == FACE_PENDANT) {   // gold ticks at 12/3/6/9, slate otherwise
        for (int i = 0; i < 12; i++) {
            bool major = (i % 3) == 0;
            float a = i * 30.0f * DEG_TO_RAD;
            int r0 = major ? 92 : 102, r1 = 110;
            g_tick_pts[i][0].x = 120 + (lv_coord_t)lroundf(sinf(a) * r0);
            g_tick_pts[i][0].y = 120 - (lv_coord_t)lroundf(cosf(a) * r0);
            g_tick_pts[i][1].x = 120 + (lv_coord_t)lroundf(sinf(a) * r1);
            g_tick_pts[i][1].y = 120 - (lv_coord_t)lroundf(cosf(a) * r1);
            lv_obj_t *t = lv_line_create(g_layer[f]);
            lv_line_set_points(t, g_tick_pts[i], 2);
            lv_obj_set_style_line_width(t, major ? 4 : 2, 0);
            lv_obj_set_style_line_color(t, lv_color_hex(major ? COL_GOLD : 0x3A4656), 0);
            lv_obj_set_style_line_rounded(t, true, 0);
        }
    }

    for (int i = 0; i < EL_COUNT; i++) {
        if (i == EL_RING) continue;
        lv_obj_t *l = lv_label_create(g_layer[f]);
        lv_label_set_text(l, "");
        g_obj[f][i] = l;
    }

    if (f == FACE_PENDANT) {   // hands on top of the text
        g_hand_h = make_hand(g_layer[f], g_pts_h, 7, 0xE8EEF6);
        g_hand_m = make_hand(g_layer[f], g_pts_m, 5, 0xE8EEF6);
        g_hand_s = make_hand(g_layer[f], g_pts_s, 2, COL_GOLD);
        lv_obj_t *dot = lv_obj_create(g_layer[f]);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 12, 12);
        lv_obj_center(dot);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(COL_GOLD), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    }
}

// Colour for an element: fixed palette entry or "auto".
static uint32_t el_color(int id, uint8_t color) {
    if (color != 0 && color < 12) return PALETTE[color];
    switch (id) {
    case EL_BATPCT: case EL_RING: return g_bat_pct < 0 ? COL_MUTED : bat_color();
    case EL_BLE:    return ble_is_connected() ? 0x2ED3A0 : COL_MUTED;
    case EL_AMPM:   return COL_GOLD;
    default:        return 0xFFFFFF;
    }
}

// Position / size / visibility of one element (colour is applied each tick).
static void apply_element(int f, int id) {
    const El &e = g_lay[f][id];
    lv_obj_t *o = g_obj[f][id];
    if (e.vis) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else       lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    if (id == EL_RING) {
        uint8_t w = RING_W[e.size & 3];
        lv_obj_set_style_arc_width(o, w, LV_PART_MAIN);
        lv_obj_set_style_arc_width(o, w, LV_PART_INDICATOR);
        return;
    }
    lv_obj_set_style_text_font(o, FONTS[e.size & 3], 0);
    lv_obj_align(o, LV_ALIGN_CENTER, e.x, e.y);
}

static void apply_layout() {
    for (int f = 0; f < FACE_COUNT; f++)
        for (int i = 0; i < EL_COUNT; i++) apply_element(f, i);
}

static void create_ui() {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_long_press, LV_EVENT_LONG_PRESSED, NULL);
    create_face(FACE_DIGITAL);
    create_face(FACE_PENDANT);
    apply_layout();
    nav_create(scr);
    tf_create(scr);
}

static void show_face() {
    int f = g_set[SET_FACE] == 1 ? FACE_PENDANT : FACE_DIGITAL;
    lv_obj_clear_flag(g_layer[f], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_layer[1 - f], LV_OBJ_FLAG_HIDDEN);
}

// ── Face update ───────────────────────────────────────────────────────
static const char *WD[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
static const char *MO[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                           "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

static bool is12h() { return g_set[SET_TIME_FORMAT] == 1; }

static void set_hand(lv_obj_t *l, lv_point_t *pts, float deg, int len, int tail) {
    float a = deg * DEG_TO_RAD, s = sinf(a), c = cosf(a);
    pts[0].x = 120 - (lv_coord_t)lroundf(s * tail);
    pts[0].y = 120 + (lv_coord_t)lroundf(c * tail);
    pts[1].x = 120 + (lv_coord_t)lroundf(s * len);
    pts[1].y = 120 - (lv_coord_t)lroundf(c * len);
    lv_line_set_points(l, pts, 2);
}

static void update_face(const RtcTime *t, uint32_t steps) {
    int f = g_set[SET_FACE] == 1 ? FACE_PENDANT : FACE_DIGITAL;
    lv_obj_t **o = g_obj[f];

    if (t) {
        if (is12h()) {
            unsigned h = t->hour % 12; if (h == 0) h = 12;
            lv_label_set_text_fmt(o[EL_TIME], "%u:%02u", h, t->minute);
            lv_label_set_text(o[EL_AMPM], t->hour < 12 ? "AM" : "PM");
        } else {
            lv_label_set_text_fmt(o[EL_TIME], "%02u:%02u", t->hour, t->minute);
            lv_label_set_text(o[EL_AMPM], "");
        }
        lv_label_set_text_fmt(o[EL_SECONDS], "%02u", t->second);
        lv_label_set_text_fmt(o[EL_DATE], "%s %u %s", WD[t->weekday % 7], t->day,
                              MO[(t->month + 11) % 12]);
    } else {
        lv_label_set_text(o[EL_TIME], "--:--");
        lv_label_set_text(o[EL_SECONDS], "");
        lv_label_set_text(o[EL_AMPM], "");
        lv_label_set_text(o[EL_DATE], "sync time");
    }

    if (g_bat_pct >= 0) {
        lv_label_set_text_fmt(o[EL_BATPCT], LV_SYMBOL_BATTERY_FULL " %d%%", g_bat_pct);
        lv_label_set_text_fmt(o[EL_BATV], "%d.%02d V", g_bat_mv / 1000, (g_bat_mv % 1000) / 10);
        lv_arc_set_value(o[EL_RING], (int16_t)(g_bat_pct * 10));
    }

    if (imu_present()) {
        float ax = imu_get_ax_mg(), ay = imu_get_ay_mg(), az = imu_get_az_mg();
        int pitch = (int)lroundf(atan2f(-ax, sqrtf(ay * ay + az * az)) * 57.2958f);
        int roll  = (int)lroundf(atan2f(ay, az) * 57.2958f);
        lv_label_set_text_fmt(o[EL_ROT], LV_SYMBOL_REFRESH " %d  P %d R %d",
                              g_set[SET_ROTATION] * 90, pitch, roll);
    } else {
        lv_label_set_text_fmt(o[EL_ROT], LV_SYMBOL_REFRESH " %d" "\xC2\xB0",
                              g_set[SET_ROTATION] * 90);
    }

    lv_label_set_text(o[EL_BLE], ble_is_connected() ? LV_SYMBOL_BLUETOOTH " ON" : LV_SYMBOL_BLUETOOTH);
    lv_label_set_text_fmt(o[EL_STEPS], "%lu steps", (unsigned long)steps);

    for (int i = 0; i < EL_COUNT; i++) {
        uint32_t c = el_color(i, g_lay[f][i].color);
        if (i == EL_RING) lv_obj_set_style_arc_color(o[i], lv_color_hex(c), LV_PART_INDICATOR);
        else              lv_obj_set_style_text_color(o[i], lv_color_hex(c), 0);
    }

    if (f == FACE_PENDANT && t) {
        float sec = t->second, min = t->minute + sec / 60.0f;
        float hr  = (t->hour % 12) + min / 60.0f;
        set_hand(g_hand_h, g_pts_h, hr * 30.0f, 54, 10);
        set_hand(g_hand_m, g_pts_m, min * 6.0f, 82, 12);
        set_hand(g_hand_s, g_pts_s, sec * 6.0f, 94, 18);
    }
}

static void refresh_face() {
    RtcTime t;
    bool ok = rtc_get(&t);
    update_face(ok ? &t : nullptr, imu_get_steps());
}

// ── Settings: apply + persist + push to the phone ────────────────────
static void apply_backlight() {
    ledcWrite(PIN_BACKLIGHT, g_asleep ? 0 : map(g_set[SET_BRIGHTNESS], 5, 100, 12, 255));
}

static void apply_setting(BleSetting id, uint8_t v, bool save) {
    g_set[id] = v;
    switch (id) {
    case SET_ROTATION:
        screen_rotation = v & 3;               // also used by the touch mapping
        tft.setRotation(v & 3);
        lv_obj_invalidate(lv_scr_act());
        break;
    case SET_BRIGHTNESS: apply_backlight(); break;
    case SET_FACE:       show_face(); break;
    default: break;
    }
    if (save) {
        g_prefs.begin("pk", false); g_prefs.putUChar(SET_KEYS[id], v); g_prefs.end();
    }
    ble_set_setting(id, v);
    lv_disp_trig_activity(NULL);               // any change wakes the screen
    refresh_face();
}

// One element edit from the app: [face, id, visible, x, y, size, color]
static void apply_layout_edit(const uint8_t m[7]) {
    uint8_t f = m[0], id = m[1];
    if (f >= FACE_COUNT || id >= EL_COUNT || m[5] > 3 || m[6] > 11) return;
    El &e = g_lay[f][id];
    e.vis = m[2] ? 1 : 0;
    e.x = constrain((int8_t)m[3], -115, 115);
    e.y = constrain((int8_t)m[4], -115, 115);
    e.size = m[5];
    e.color = m[6];
    apply_element(f, id);
    g_layout_dirty_ms = millis();
    lv_disp_trig_activity(NULL);
}

// ── Stopwatch / timer helpers ─────────────────────────────────────────
static void apply_timer_cmd(const uint8_t *c, size_t n) {
    switch (c[0]) {
    case 0x01: sw_start(); break;
    case 0x02: sw_stop();  break;
    case 0x03: sw_reset(); break;
    case 0x10: ct_start(); break;
    case 0x11: ct_stop();  break;
    case 0x12: ct_reset(); break;
    case 0x13:
        if (n >= 5) ct_set(c[1] | (c[2] << 8) | ((uint32_t)c[3] << 16) | ((uint32_t)c[4] << 24));
        break;
    case 0x14: ct_ack(); break;
    }
    lv_disp_trig_activity(NULL);           // a phone command wakes the screen
}

static void build_timer_state(uint8_t *s) {
    uint32_t swe = sw_elapsed_ms(), ctr = ct_remaining_ms(), ctd = ct_duration_ms();
    s[0] = sw_is_running() ? 1 : 0;
    memcpy(&s[1], &swe, 4);
    s[5] = ct_is_running() ? 1 : 0;
    memcpy(&s[6], &ctr, 4);
    memcpy(&s[10], &ctd, 4);
    s[14] = ct_alarming() ? 1 : 0;
}

// ── setup / loop ──────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(200);

    Wire.begin();
    analogReadResolution(12);
    settings_load();

    screen_rotation = g_set[SET_ROTATION];
    lv_init();
    lv_xiao_disp_init();
    lv_xiao_touch_init();

    ledcAttach(PIN_BACKLIGHT, BL_PWM_FREQ, BL_PWM_RES);
    apply_backlight();
    Serial.println("[SYS] display ready");

    create_ui();
    show_face();
    lv_timer_handler();

    rtc_init();
    imu_init();
    timers_init();
    ble_init(g_goal, imu_get_sensitivity(), g_set);
    layout_publish();

    battery_sample();
    ble_notify_battery((uint8_t)g_bat_pct, (uint16_t)g_bat_mv);
    refresh_face();

    Serial.printf("[SYS] init complete  face=%u fmt=%s rot=%u bright=%u sleep=%us\n",
                  g_set[SET_FACE], is12h() ? "12h" : "24h", g_set[SET_ROTATION] * 90,
                  g_set[SET_BRIGHTNESS], g_set[SET_SLEEP]);
}

void loop() {
    uint32_t now = millis();
    lv_timer_handler();

    // ── Screen sleep / wake ───────────────────────────────────────
    uint32_t idle = lv_disp_get_inactive_time(NULL);
    if (!g_asleep && g_set[SET_SLEEP] > 0 && idle > g_set[SET_SLEEP] * 1000UL) {
        g_asleep = true;
        apply_backlight();
    } else if (g_asleep && idle < 300) {
        g_asleep = false;
        g_wake_ms = now;
        refresh_face();
        apply_backlight();
    }

    // ── Touch: long-press opens / closes the stopwatch overlay ────
    if (g_timer_toggle_request) {
        g_timer_toggle_request = false;
        tf_set_visible(!tf_visible());
    }

    // ── Stopwatch / countdown timer ───────────────────────────────
    uint8_t tcmd[8];
    size_t  tlen = ble_pop_timer_cmd(tcmd);
    if (tlen) apply_timer_cmd(tcmd, tlen);
    if (timers_service()) {                 // countdown just hit zero
        tf_set_visible(true);               // surface the alarm
        lv_disp_trig_activity(NULL);
    }
    if (tf_visible()) tf_tick();
    {
        static uint32_t tmr_notify_ms = 0;
        static uint8_t  tmr_last[15];
        uint8_t st[15];
        build_timer_state(st);
        bool active = sw_is_running() || ct_is_running() || ct_alarming();
        if (memcmp(st, tmr_last, 15) != 0 || (active && now - tmr_notify_ms > 250)) {
            memcpy(tmr_last, st, 15);
            tmr_notify_ms = now;
            ble_timer_notify_state(st, 15);
        }
    }

    // ── Consume BLE writes ────────────────────────────────────────
    uint32_t new_goal;
    if (ble_pop_pending_goal(&new_goal)) {
        g_goal = new_goal;
        prefs_put_u32("goal", g_goal);
        ble_set_goal_value(g_goal);
    }

    uint8_t v;
    if (ble_pop_pending_sensitivity(&v)) {
        imu_set_sensitivity(v);
        ble_set_sens_value(imu_get_sensitivity());
    }

    uint8_t time_buf[7];
    if (ble_pop_pending_time(time_buf)) {
        rtc_set_from_ble(time_buf);
        refresh_face();
    }

    if (ble_pop_reset_request()) imu_reset_steps();

    for (int i = 0; i < SET_COUNT; i++) {
        if (ble_pop_setting((BleSetting)i, &v)) apply_setting((BleSetting)i, v, true);
    }

    // ── Face designer edits ───────────────────────────────────────
    uint8_t edit[7];
    bool edited = false;
    while (ble_pop_layout_element(edit)) { apply_layout_edit(edit); edited = true; }
    if (ble_pop_layout_reset()) {
        memcpy(g_lay, DEFAULT_LAYOUT, sizeof g_lay);
        apply_layout();
        layout_save_now();
        edited = true;
    }
    if (edited) { layout_publish(); refresh_face(); }
    if (g_layout_dirty_ms && now - g_layout_dirty_ms > 1500) layout_save_now();

    // ── Turn-by-turn navigation from the phone ────────────────────
    static bool     nav_on = false;
    static uint32_t nav_last_ms = 0;
    static uint8_t  nav_man = 0xFF;
    static uint8_t  nav_pkt[NAV_MAX_LEN];
    size_t nav_len = ble_pop_nav(nav_pkt);
    if (nav_len) {
        bool active = nav_apply(nav_pkt, nav_len, is12h());
        nav_last_ms = now;
        if (active != nav_on || (active && nav_pkt[1] != nav_man)) {
            lv_disp_trig_activity(NULL);         // new maneuver wakes the screen
        }
        nav_man = active ? nav_pkt[1] : 0xFF;
        if (active != nav_on) {
            nav_on = active;
            nav_set_visible(active);
            if (!active) show_face();
            Serial.printf("[NAV] %s\n", active ? "started" : "ended");
        }
    } else if (nav_on && now - nav_last_ms > 20000) {   // phone went quiet
        nav_on = false;
        nav_set_visible(false);
        show_face();
        Serial.println("[NAV] timed out");
    }

    // ── 1 Hz: face, steps, clock ──────────────────────────────────
    static uint32_t ui_last = 0;
    if (now - ui_last >= 1000) {
        ui_last = now;

        RtcTime t;
        bool rtc_ok = rtc_get(&t);
        uint32_t steps = imu_get_steps();

        if (rtc_ok) {   // date rollover — reset steps at midnight
            int32_t today = rtc_day_pack(&t);
            if (g_saved_day == -1) {
                day_save(today);
            } else if (today != g_saved_day) {
                imu_reset_steps();
                steps = 0;
                day_save(today);
            }
        }

        if (!g_asleep) update_face(rtc_ok ? &t : nullptr, steps);
        ble_notify_steps(steps);

        uint8_t clk[8] = {0};
        if (rtc_ok) {
            clk[0] = (uint8_t)(t.year - 1900); clk[1] = (uint8_t)(t.month - 1);
            clk[2] = t.day; clk[3] = t.hour; clk[4] = t.minute; clk[5] = t.second;
            clk[6] = t.weekday; clk[7] = 1;
        }
        ble_notify_clock(clk);
    }

    // ── Battery every 2 s (live in the app) ───────────────────────
    static uint32_t bat_last = 0;
    if (now - bat_last >= 2000) {
        bat_last = now;
        battery_sample();
        ble_notify_battery((uint8_t)g_bat_pct, (uint16_t)g_bat_mv);
    }

    // ── ~10 Hz IMU BLE notify (only when a sensor is fitted) ──────
    static uint32_t imu_last = 0;
    if (imu_present() && now - imu_last >= 100) {
        imu_last = now;
        ble_notify_imu((int16_t)imu_get_ax_mg(), (int16_t)imu_get_ay_mg(),
                       (int16_t)imu_get_az_mg(), (int16_t)imu_get_mag_mg());
    }

    delay(5);
}
