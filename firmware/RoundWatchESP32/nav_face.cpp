/*
 * Navigation face: big turn arrow, distance to the turn, instruction and ETA.
 * The arrow is two thick rounded polylines (shaft + head) drawn in a
 * 100 x 100 box, so every maneuver stays crisp on the 240 x 240 screen.
 */
#include "nav_face.h"
#include "ble_service.h"

#define BOX_X  70     // arrow box top-left on screen
#define BOX_Y  18
#define LINE_W 11

struct Pt { uint8_t x, y; };
struct Shape { uint8_t ns; Pt shaft[6]; uint8_t nh; Pt head[5]; uint8_t ring; };

// Shapes in the 100 x 100 box (y down). ring: 0 none, 1 big target, 2 roundabout.
static const Shape SHAPES[NAV_MANEUVER_COUNT] = {
    /* STRAIGHT     */ {2, {{50, 95}, {50, 12}}, 3, {{24, 38}, {50, 12}, {76, 38}}, 0},
    /* SLIGHT_LEFT  */ {3, {{64, 95}, {64, 60}, {26, 22}}, 3, {{50, 20}, {24, 20}, {24, 46}}, 0},
    /* LEFT         */ {3, {{70, 95}, {70, 50}, {18, 50}}, 3, {{38, 30}, {16, 50}, {38, 70}}, 0},
    /* SHARP_LEFT   */ {3, {{70, 95}, {70, 30}, {30, 72}}, 3, {{30, 48}, {28, 74}, {54, 74}}, 0},
    /* SLIGHT_RIGHT */ {3, {{36, 95}, {36, 60}, {74, 22}}, 3, {{50, 20}, {76, 20}, {76, 46}}, 0},
    /* RIGHT        */ {3, {{30, 95}, {30, 50}, {82, 50}}, 3, {{62, 30}, {84, 50}, {62, 70}}, 0},
    /* SHARP_RIGHT  */ {3, {{30, 95}, {30, 30}, {70, 72}}, 3, {{70, 48}, {72, 74}, {46, 74}}, 0},
    /* UTURN        */ {6, {{70, 95}, {70, 36}, {62, 20}, {38, 20}, {30, 36}, {30, 70}}, 3,
                          {{14, 54}, {30, 72}, {46, 54}}, 0},
    /* ARRIVE       */ {0, {}, 0, {}, 1},
    /* DEPART       */ {2, {{50, 95}, {50, 12}}, 3, {{24, 38}, {50, 12}, {76, 38}}, 0},
    /* ROUNDABOUT   */ {2, {{50, 96}, {50, 76}}, 5, {{63, 37}, {84, 16}, {64, 16}, {84, 16}, {84, 36}}, 2},
};

static lv_obj_t  *s_layer, *s_shaft, *s_head, *s_ring, *s_dist, *s_text, *s_eta;
static lv_point_t s_shaft_pts[6], s_head_pts[5];
static bool       s_active = false;

static lv_obj_t *mk_line(lv_obj_t *p) {
    lv_obj_t *l = lv_line_create(p);
    lv_obj_set_style_line_width(l, LINE_W, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_set_style_line_color(l, lv_color_hex(0xFFFFFF), 0);
    return l;
}

static lv_obj_t *mk_label(lv_obj_t *p, const lv_font_t *f, uint32_t col, int y) {
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
    return l;
}

void nav_create(lv_obj_t *screen) {
    s_layer = lv_obj_create(screen);
    lv_obj_remove_style_all(s_layer);
    lv_obj_set_size(s_layer, 240, 240);
    lv_obj_set_pos(s_layer, 0, 0);
    lv_obj_set_style_bg_color(s_layer, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_layer, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_SCROLLABLE);

    s_ring = lv_obj_create(s_layer);
    lv_obj_remove_style_all(s_ring);
    lv_obj_set_style_radius(s_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(s_ring, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(s_ring, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);

    s_shaft = mk_line(s_layer);
    s_head  = mk_line(s_layer);

    s_dist = mk_label(s_layer, &lv_font_montserrat_48, 0xFFFFFF, 20);
    s_text = mk_label(s_layer, &lv_font_montserrat_16, 0xC0D0E0, 58);
    lv_obj_set_width(s_text, 184);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_SCROLL_CIRCULAR);
    s_eta  = mk_label(s_layer, &lv_font_montserrat_14, 0x3DD68C, 84);

    lv_obj_add_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
}

void nav_set_visible(bool on) {
    if (on) { lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(s_layer); }
    else    lv_obj_add_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
}

static void set_poly(lv_obj_t *l, lv_point_t *dst, const Pt *src, uint8_t n) {
    if (n < 2) { lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN); return; }
    for (uint8_t i = 0; i < n; i++) { dst[i].x = BOX_X + src[i].x; dst[i].y = BOX_Y + src[i].y; }
    lv_line_set_points(l, dst, n);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN);
}

static void fmt_dist(char *out, size_t n, uint32_t m) {
    if (m < 1000) snprintf(out, n, "%lu m", (unsigned long)(m < 100 ? m : (m + 5) / 10 * 10));
    else if (m < 10000) snprintf(out, n, "%lu.%lu km", (unsigned long)(m / 1000), (unsigned long)((m % 1000) / 100));
    else snprintf(out, n, "%lu km", (unsigned long)((m + 500) / 1000));
}

bool nav_apply(const uint8_t *p, size_t len, bool h12) {
    if (len < 15 || p[0] == 0) { s_active = false; return false; }
    s_active = true;

    uint8_t  man   = p[1] < NAV_MANEUVER_COUNT ? p[1] : NAV_STRAIGHT;
    uint32_t next  = p[2] | (p[3] << 8) | ((uint32_t)p[4] << 16) | ((uint32_t)p[5] << 24);
    uint32_t left  = p[6] | (p[7] << 8) | ((uint32_t)p[8] << 16) | ((uint32_t)p[9] << 24);
    uint8_t  eh = p[10], em = p[11];
    uint16_t mins  = p[12] | (p[13] << 8);
    size_t   tlen  = p[14];
    if (15 + tlen > len) tlen = len - 15;

    // Arrow (gold when the turn is close)
    const Shape &s = SHAPES[man];
    uint32_t col = (next <= 100 && man != NAV_ARRIVE) ? 0xD4AF37 : 0xFFFFFF;
    lv_obj_set_style_line_color(s_shaft, lv_color_hex(col), 0);
    lv_obj_set_style_line_color(s_head, lv_color_hex(col), 0);
    set_poly(s_shaft, s_shaft_pts, s.shaft, s.ns);
    set_poly(s_head, s_head_pts, s.head, s.nh);
    if (s.ring) {
        int d = s.ring == 1 ? 64 : 40, bw = s.ring == 1 ? 12 : 9;
        lv_obj_set_size(s_ring, d, d);
        lv_obj_set_style_border_width(s_ring, bw, 0);
        lv_obj_set_style_border_color(s_ring, lv_color_hex(s.ring == 1 ? 0x3DD68C : col), 0);
        lv_obj_set_pos(s_ring, BOX_X + 50 - d / 2, BOX_Y + (s.ring == 1 ? 50 : 56) - d / 2);
        lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[128];
    if (man == NAV_ARRIVE && next < 30) snprintf(buf, sizeof buf, "Arrived");
    else fmt_dist(buf, sizeof buf, next);
    lv_label_set_text(s_dist, buf);
    lv_obj_set_style_text_font(s_dist, strlen(buf) > 7 ? &lv_font_montserrat_20 : &lv_font_montserrat_48, 0);

    size_t k = 0;
    for (size_t i = 0; i < tlen && k < sizeof buf - 1; i++) {
        uint8_t c = p[15 + i];
        buf[k++] = (c >= 32 && c < 127) ? (char)c : ' ';
    }
    buf[k] = 0;
    lv_label_set_text(s_text, buf);

    char d2[16];
    fmt_dist(d2, sizeof d2, left);
    unsigned h = eh % 24;
    if (h12) {
        unsigned h1 = h % 12 ? h % 12 : 12;
        snprintf(buf, sizeof buf, "%u:%02u %s  " LV_SYMBOL_RIGHT "  %s", h1, em, h < 12 ? "AM" : "PM", d2);
    } else {
        snprintf(buf, sizeof buf, "%02u:%02u  " LV_SYMBOL_RIGHT "  %s", h, em, d2);
    }
    if (mins < 600 && mins > 0) {
        size_t m = strlen(buf);
        snprintf(buf + m, sizeof buf - m, "\n%u min", mins);
    }
    lv_label_set_text(s_eta, buf);
    return true;
}
