/*
 * IMU — LSM6DS3TR-C step counter
 * Full rhythm-gate pedometer ported from StepCounterZephyr/src/main.c.
 *
 * Runs in a dedicated FreeRTOS task at 100 Hz so LVGL redraws never
 * starve the sampler. All shared variables are volatile 32-bit aligned
 * so 32-bit reads on ESP32 are inherently atomic.
 */
#include "imu.h"
#include <Wire.h>
#include <Preferences.h>
#include <math.h>

// ── LSM6DS3TR-C registers ────────────────────────────────────────────
#define IMU_ADDR       0x6A
#define REG_WHO_AM_I   0x0F
#define REG_CTRL1_XL   0x10  // accel config
#define REG_CTRL3_C    0x12  // BDU + IF_INC
#define REG_STATUS     0x1E
#define REG_OUTX_L_XL  0x28

// 104 Hz, ±4 g (0x48) — same as Zephyr config
#define CTRL1_XL_104HZ_4G  0x48
// BDU=1 (block data update), IF_INC=1 (auto-increment)
#define CTRL3_C_DEFAULT    0x44

// mg per LSB at ±4 g, 16-bit output
#define MG_PER_LSB  0.122f

// ── Pedometer constants (match Zephyr) ──────────────────────────────
#define PED_PERIOD_MS   10
#define LP_ALPHA        0.25f
#define BASE_ALPHA      0.02f
#define PEAK_MIN_MG     35.0f
#define AMP_MIN_MG      70.0f
#define AMP_MAX_MG      5000.0f
#define STEP_MIN_MS     240U
#define BOUNCE_MS       140U
#define STEP_MAX_MS     2200U
#define RHYTHM_TOL      0.50f
#define AMP_TOL_LO      0.22f
#define AMP_TOL_HI      3.5f
#define CONFIRM_STEPS   2U

// ── Shared state ─────────────────────────────────────────────────────
static volatile uint32_t g_steps;
static volatile bool     g_walking;
static volatile int      g_cadence_spm;
static volatile int      g_ax_mg, g_ay_mg, g_az_mg, g_mag_mg;
static volatile uint8_t  g_sensitivity = 5;
static bool              g_present = false;   // LSM6DS3 answered WHO_AM_I

// ── Pedometer state (task-private) ───────────────────────────────────
enum PedPhase { PH_SEEK, PH_PEAK, PH_VALLEY };

static struct {
    float lp1, lp2, base;
    PedPhase phase;
    float peak, valley;
    uint32_t peak_t;

    uint32_t last_t;
    uint32_t avg_dt;
    float avg_amp;
    uint32_t pending;
    bool walking;
    bool fast_strike;
} p;

// ── Persistence ──────────────────────────────────────────────────────
static Preferences prefs;

static void steps_load() {
    prefs.begin("pk", true);
    g_steps     = prefs.getUInt("steps", 0);
    g_sensitivity = (uint8_t)prefs.getUInt("sens", 5);
    prefs.end();
}

static void steps_save_now() {
    prefs.begin("pk", false);
    prefs.putUInt("steps", g_steps);
    prefs.end();
}

static void sens_save() {
    prefs.begin("pk", false);
    prefs.putUInt("sens", g_sensitivity);
    prefs.end();
}

// ── I2C helpers ──────────────────────────────────────────────────────
static bool imu_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(IMU_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool imu_read_burst(uint8_t reg, uint8_t *buf, size_t len) {
    Wire.beginTransmission(IMU_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom((uint8_t)IMU_ADDR, (uint8_t)len);
    for (size_t i = 0; i < len; i++) {
        if (!Wire.available()) return false;
        buf[i] = Wire.read();
    }
    return true;
}

static inline int16_t le16(const uint8_t *d) {
    return (int16_t)((uint16_t)d[1] << 8 | d[0]);
}

// ── Pedometer ────────────────────────────────────────────────────────
static void ped_stop_walking() {
    if (p.walking) steps_save_now();
    p.walking = false;
    g_walking = false;
    g_cadence_spm = 0;
}

static void ped_reset() {
    ped_stop_walking();
    p.last_t = 0;
    p.avg_dt = 0;
    p.pending = 0;
    p.fast_strike = false;
}

static void ped_restart(uint32_t t, float amp) {
    ped_stop_walking();
    p.last_t = t;
    p.avg_dt = 0;
    p.avg_amp = amp;
    p.pending = 1;
    p.fast_strike = false;
}

static void step_candidate(uint32_t t, float amp) {
    float amp_min = 145.0f - (float)g_sensitivity * 7.5f;

    if (amp < amp_min) return;
    if (amp > AMP_MAX_MG) { ped_reset(); return; }

    if (p.last_t == 0) { ped_restart(t, amp); return; }

    uint32_t dt = t - p.last_t;

    if (dt > STEP_MAX_MS) { ped_restart(t, amp); return; }

    if (dt < STEP_MIN_MS) {
        if (dt >= BOUNCE_MS || (p.avg_dt != 0 && dt * 2U < p.avg_dt + 80U)) return;
        if (p.walking && !p.fast_strike) { p.fast_strike = true; return; }
        ped_reset();
        return;
    }

    if (amp < p.avg_amp * AMP_TOL_LO || amp > p.avg_amp * AMP_TOL_HI) {
        ped_restart(t, amp);
        return;
    }

    uint32_t add = 1;

    if (p.avg_dt != 0) {
        float r = (float)dt / (float)p.avg_dt;
        if (p.walking && r > 1.6f && r < 2.4f) {
            add = 2;
            dt /= 2;
        } else if (r < 1.0f - RHYTHM_TOL || r > 1.0f + RHYTHM_TOL) {
            ped_restart(t, amp);
            return;
        }
        p.avg_dt = (p.avg_dt * 3U + dt) / 4U;
    } else {
        p.avg_dt = dt;
    }

    p.avg_amp = p.avg_amp * 0.8f + amp * 0.2f;
    p.last_t = t;
    p.fast_strike = false;
    g_cadence_spm = (int)(60000U / p.avg_dt);

    if (p.walking) {
        g_steps += add;
        return;
    }

    p.pending += add;
    if (p.pending >= CONFIRM_STEPS) {
        p.walking = true;
        g_walking = true;
        g_steps += p.pending;
        p.pending = 0;
    }
}

static void ped_process(float mag, uint32_t now_ms) {
    p.lp1  += (mag - p.lp1)  * LP_ALPHA;
    p.lp2  += (p.lp1 - p.lp2) * LP_ALPHA;
    p.base += (mag - p.base) * BASE_ALPHA;

    float sig = p.lp2 - p.base;
    float peak_min = 85.0f - (float)g_sensitivity * 5.0f;
    float thr = peak_min;
    if (p.last_t != 0 && p.avg_amp * 0.2f > thr) thr = p.avg_amp * 0.2f;

    switch (p.phase) {
    case PH_SEEK:
        if (sig > thr) { p.phase = PH_PEAK; p.peak = sig; p.peak_t = now_ms; }
        break;
    case PH_PEAK:
        if (sig > p.peak) { p.peak = sig; p.peak_t = now_ms; }
        else if (sig < 0.0f) { p.phase = PH_VALLEY; p.valley = sig; }
        break;
    case PH_VALLEY:
        if (sig < p.valley) { p.valley = sig; }
        else if (sig > 0.0f) { step_candidate(p.peak_t, p.peak - p.valley); p.phase = PH_SEEK; }
        break;
    }

    if (p.last_t != 0 && (now_ms - p.last_t) > STEP_MAX_MS) ped_reset();
}

// ── FreeRTOS task ─────────────────────────────────────────────────────
static void imu_task(void *) {
    bool primed = false;

    for (;;) {
        TickType_t wake = xTaskGetTickCount();

        uint8_t raw[6];
        if (imu_read_burst(REG_OUTX_L_XL, raw, 6)) {
            float ax = le16(&raw[0]) * MG_PER_LSB;
            float ay = le16(&raw[2]) * MG_PER_LSB;
            float az = le16(&raw[4]) * MG_PER_LSB;
            float mag = sqrtf(ax*ax + ay*ay + az*az);

            g_ax_mg  = (int)ax;
            g_ay_mg  = (int)ay;
            g_az_mg  = (int)az;
            g_mag_mg = (int)mag;

            if (!primed) { p.lp1 = p.lp2 = p.base = mag; primed = true; }
            ped_process(mag, millis());
        }

        vTaskDelayUntil(&wake, pdMS_TO_TICKS(PED_PERIOD_MS));
    }
}

// ── Public API ───────────────────────────────────────────────────────
void imu_init() {
    steps_load();

    // Scan I2C bus to find what devices are present
    Serial.println("[I2C] scanning...");
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) Serial.printf("[I2C] found 0x%02X\n", addr);
    }

    imu_write(REG_CTRL3_C, CTRL3_C_DEFAULT);
    imu_write(REG_CTRL1_XL, CTRL1_XL_104HZ_4G);

    // Verify WHO_AM_I
    uint8_t who = 0;
    imu_read_burst(REG_WHO_AM_I, &who, 1);
    g_present = (who == 0x6A || who == 0x69);
    Serial.printf("[IMU] WHO_AM_I=0x%02X (expect 0x6A), present=%d, steps=%u, sens=%u\n",
                  who, g_present, g_steps, g_sensitivity);
    if (!g_present) {
        Serial.println("[IMU] no IMU found - step counting and tilt disabled");
        return;
    }

    xTaskCreatePinnedToCore(imu_task, "imu", 4096, nullptr, 5, nullptr, 0);
}

bool     imu_present()         { return g_present; }
uint32_t imu_get_steps()       { return g_steps; }
void     imu_reset_steps()     { g_steps = 0; steps_save_now(); }
bool     imu_is_walking()      { return g_walking; }
int      imu_get_cadence_spm() { return g_cadence_spm; }
int      imu_get_ax_mg()       { return g_ax_mg; }
int      imu_get_ay_mg()       { return g_ay_mg; }
int      imu_get_az_mg()       { return g_az_mg; }
int      imu_get_mag_mg()      { return g_mag_mg; }

void imu_set_sensitivity(uint8_t s) {
    if (s < 1 || s > 10) return;
    g_sensitivity = s;
    sens_save();
}
uint8_t imu_get_sensitivity() { return g_sensitivity; }
