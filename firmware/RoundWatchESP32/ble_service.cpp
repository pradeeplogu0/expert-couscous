/*
 * BLE service — same UUIDs as StepCounterZephyr so the existing
 * Web Bluetooth control panel (web/control/index.html) keeps working,
 * plus extra characteristics used by the iOS "Watch Lab" app.
 *
 * Uses the ESP32 Arduino Bluedroid BLE stack (BLEDevice.h), which ships
 * with the esp32 Arduino core — no extra library needed.
 *
 * Service 4b0ad7a0-4b2c-11ee-be56-0242ac120002, characteristics 4b0ad7XX-…:
 *   a1  steps        READ + NOTIFY          uint32_t LE
 *   a2  reset        WRITE                  1 byte, value must be 0x01
 *   a3  goal         READ + WRITE           uint32_t LE (1000-50000)
 *   a4  time         WRITE                  7 bytes [tm_year-1900, tm_mon(0-11), mday, hour, min, sec, wday]
 *   a5  sensitivity  READ + WRITE           1 byte (1-10)
 *   a6  imu          READ + NOTIFY          4x int16_t LE [ax, ay, az, mag] in mg
 *   a7  battery      READ + NOTIFY          4 bytes [pct, 0, battery_mv u16 LE]   (every 2 s)
 *   a8  clock        READ + NOTIFY          8 bytes [tm_year-1900, tm_mon, mday, h, m, s, wday, valid] (1 Hz)
 *   a9  rotation     READ + WRITE + NOTIFY  1 byte 0-3 (x 90 deg)
 *   aa  brightness   READ + WRITE + NOTIFY  1 byte 5-100 (%)
 *   ab  time format  READ + WRITE + NOTIFY  1 byte 0 = 24 h, 1 = 12 h
 *   ac  face         READ + WRITE + NOTIFY  1 byte 0 = digital, 1 = pendant
 *   ad  sleep        READ + WRITE + NOTIFY  1 byte seconds until screen off, 0 = never
 *   ae  layout       READ + WRITE + NOTIFY  face designer: write 7 bytes [face, element, visible, x, y, size, color]
 *                                          or 0xFF to reset; read = every element of both faces
 *   af  navigation   WRITE (+ no response)  turn-by-turn packet, see ble_service.h
 */
#include "ble_service.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#define SERVICE_UUID   "4b0ad7a0-4b2c-11ee-be56-0242ac120002"
#define STEPS_UUID     "4b0ad7a1-4b2c-11ee-be56-0242ac120002"
#define RESET_UUID     "4b0ad7a2-4b2c-11ee-be56-0242ac120002"
#define GOAL_UUID      "4b0ad7a3-4b2c-11ee-be56-0242ac120002"
#define TIME_UUID      "4b0ad7a4-4b2c-11ee-be56-0242ac120002"
#define SENS_UUID      "4b0ad7a5-4b2c-11ee-be56-0242ac120002"
#define IMU_UUID       "4b0ad7a6-4b2c-11ee-be56-0242ac120002"
#define BATT_UUID      "4b0ad7a7-4b2c-11ee-be56-0242ac120002"
#define CLOCK_UUID     "4b0ad7a8-4b2c-11ee-be56-0242ac120002"

static BLEServer          *s_server   = nullptr;
static BLECharacteristic  *s_steps    = nullptr;
static BLECharacteristic  *s_imu      = nullptr;
static BLECharacteristic  *s_goal     = nullptr;
static BLECharacteristic  *s_sens     = nullptr;
static BLECharacteristic  *s_batt     = nullptr;
static BLECharacteristic  *s_clock    = nullptr;

static volatile bool     s_connected         = false;

// Pending values written by the BLE client
static volatile bool     s_reset_pending     = false;
static volatile bool     s_goal_pending      = false;
static volatile uint32_t s_goal_value        = 0;
static volatile bool     s_sens_pending      = false;
static volatile uint8_t  s_sens_value        = 0;
static volatile bool     s_time_pending      = false;
static volatile uint8_t  s_time_buf[7]       = {};

// ── One-byte settings ─────────────────────────────────────────────────
struct SettingChar {
    const char        *uuid;
    uint8_t            lo, hi;
    BLECharacteristic *chr;
    volatile bool      pending;
    volatile uint8_t   value;
};

static SettingChar s_set[SET_COUNT] = {
    {"4b0ad7a9-4b2c-11ee-be56-0242ac120002", 0,   3, nullptr, false, 0},  // rotation
    {"4b0ad7aa-4b2c-11ee-be56-0242ac120002", 5, 100, nullptr, false, 0},  // brightness
    {"4b0ad7ab-4b2c-11ee-be56-0242ac120002", 0,   1, nullptr, false, 0},  // time format
    {"4b0ad7ac-4b2c-11ee-be56-0242ac120002", 0,   1, nullptr, false, 0},  // face
    {"4b0ad7ad-4b2c-11ee-be56-0242ac120002", 0, 255, nullptr, false, 0},  // sleep
};

// ── Callbacks ─────────────────────────────────────────────────────────
class ServerCB : public BLEServerCallbacks {
    void onConnect(BLEServer *) override {
        s_connected = true;
        Serial.println("[BLE] connected");
    }
    void onDisconnect(BLEServer *) override {
        s_connected    = false;
        Serial.println("[BLE] disconnected — restarting adv");
        BLEDevice::startAdvertising();
    }
};

class ResetCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        if (c->getLength() == 1 && c->getData()[0] == 0x01) s_reset_pending = true;
    }
};

class GoalCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        if (c->getLength() == 4) {
            uint32_t g;
            memcpy(&g, c->getData(), 4);
            if (g >= 1000 && g <= 50000) { s_goal_value = g; s_goal_pending = true; }
        }
    }
};

class TimeCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        if (c->getLength() == 7) {
            const uint8_t *d = c->getData();
            for (int i = 0; i < 7; i++) s_time_buf[i] = d[i];
            s_time_pending = true;
        }
    }
};

class SensCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        if (c->getLength() == 1) {
            uint8_t s = c->getData()[0];
            if (s >= 1 && s <= 10) { s_sens_value = s; s_sens_pending = true; }
        }
    }
};

class SettingCB : public BLECharacteristicCallbacks {
public:
    explicit SettingCB(SettingChar *s) : m_s(s) {}
    void onWrite(BLECharacteristic *c) override {
        if (c->getLength() != 1) return;
        uint8_t v = c->getData()[0];
        if (v < m_s->lo || v > m_s->hi) return;
        m_s->value = v;
        m_s->pending = true;
    }
private:
    SettingChar *m_s;
};

// ── Navigation (af) ───────────────────────────────────────────────────
// Turn-by-turn packet from the phone, see ble_service.h for the layout.
#define NAV_UUID "4b0ad7af-4b2c-11ee-be56-0242ac120002"
static volatile uint8_t s_nav_buf[NAV_MAX_LEN];
static volatile size_t  s_nav_len = 0;
static volatile bool    s_nav_pending = false;

class NavCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        size_t n = c->getLength();
        if (n < 1 || n > NAV_MAX_LEN) return;
        const uint8_t *d = c->getData();
        for (size_t i = 0; i < n; i++) s_nav_buf[i] = d[i];
        s_nav_len = n;
        s_nav_pending = true;              // latest packet wins
    }
};

size_t ble_pop_nav(uint8_t *out) {
    if (!s_nav_pending) return 0;
    size_t n = s_nav_len;
    for (size_t i = 0; i < n; i++) out[i] = s_nav_buf[i];
    s_nav_pending = false;
    return n;
}

// ── Face layout (ae) ──────────────────────────────────────────────────
// Write 7 bytes [face, element, visible, x, y, size, color] to change one
// element, or 1 byte 0xFF to reset both faces. A small queue keeps quick
// drags from overwriting each other before loop() consumes them.
#define LAYOUT_UUID "4b0ad7ae-4b2c-11ee-be56-0242ac120002"
static BLECharacteristic *s_layout = nullptr;
static volatile uint8_t   s_lq[16][7];
static volatile uint8_t   s_lq_head = 0, s_lq_tail = 0;
static volatile bool      s_layout_reset = false;

class LayoutCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        const uint8_t *d = c->getData();
        size_t n = c->getLength();
        if (n == 1 && d[0] == 0xFF) { s_layout_reset = true; return; }
        if (n != 7) return;
        uint8_t next = (s_lq_head + 1) & 15;
        if (next == s_lq_tail) return;                 // queue full: drop
        for (int i = 0; i < 7; i++) s_lq[s_lq_head][i] = d[i];
        s_lq_head = next;
    }
};

void ble_set_layout(const uint8_t *buf, size_t len) {
    if (!s_layout) return;
    s_layout->setValue((uint8_t *)buf, len);
    if (s_connected) s_layout->notify();
}

bool ble_pop_layout_element(uint8_t out[7]) {
    if (s_lq_tail == s_lq_head) return false;
    for (int i = 0; i < 7; i++) out[i] = s_lq[s_lq_tail][i];
    s_lq_tail = (s_lq_tail + 1) & 15;
    return true;
}

bool ble_pop_layout_reset() {
    if (!s_layout_reset) return false;
    s_layout_reset = false;
    return true;
}

// ── Stopwatch / timer (b1 cmd, b2 state) ──────────────────────────────
#define TMR_CMD_UUID   "4b0ad7b1-4b2c-11ee-be56-0242ac120002"
#define TMR_STATE_UUID "4b0ad7b2-4b2c-11ee-be56-0242ac120002"
static BLECharacteristic *s_tmr_state = nullptr;
static volatile uint8_t   s_tmr_cmd[8];
static volatile size_t    s_tmr_cmd_len = 0;
static volatile bool      s_tmr_cmd_pending = false;

class TmrCmdCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        size_t n = c->getLength();
        if (n < 1 || n > sizeof s_tmr_cmd) return;
        const uint8_t *d = c->getData();
        for (size_t i = 0; i < n; i++) s_tmr_cmd[i] = d[i];
        s_tmr_cmd_len = n;
        s_tmr_cmd_pending = true;              // latest command wins
    }
};

size_t ble_pop_timer_cmd(uint8_t *out) {
    if (!s_tmr_cmd_pending) return 0;
    size_t n = s_tmr_cmd_len;
    for (size_t i = 0; i < n; i++) out[i] = s_tmr_cmd[i];
    s_tmr_cmd_pending = false;
    return n;
}

void ble_timer_notify_state(const uint8_t *buf, size_t len) {
    if (!s_tmr_state) return;
    s_tmr_state->setValue((uint8_t *)buf, len);
    if (s_connected) s_tmr_state->notify();
}

// ESP32 Arduino core 3.3+ runs BLE on NimBLE: the stack creates the CCCD
// (0x2902) itself for NOTIFY characteristics and tracks who subscribed, and
// notify() only sends to subscribed clients. So no manual descriptor or
// "subscribed" flags here — the old Bluedroid-style descriptor callbacks are
// never called under NimBLE, which silently blocked every notification.
static BLECharacteristic *make_char(BLEService *svc, const char *uuid, uint32_t props) {
    return svc->createCharacteristic(uuid, props);
}

// ── Init ──────────────────────────────────────────────────────────────
void ble_init(uint32_t goal, uint8_t sens, const uint8_t settings[SET_COUNT]) {
    BLEDevice::init("StepCounter");
    BLEDevice::setMTU(247);          // full face layout fits in one notify
    s_server = BLEDevice::createServer();
    s_server->setCallbacks(new ServerCB());

    // 17 characteristics, 11 with a CCCD → ~46 handles; leave headroom.
    BLEService *svc = s_server->createService(BLEUUID(SERVICE_UUID), 72);

    const uint32_t R  = BLECharacteristic::PROPERTY_READ;
    const uint32_t W  = BLECharacteristic::PROPERTY_WRITE;
    const uint32_t N  = BLECharacteristic::PROPERTY_NOTIFY;
    uint8_t zero[8] = {};

    s_steps = make_char(svc, STEPS_UUID, R | N);
    s_steps->setValue(zero, 4);

    make_char(svc, RESET_UUID, W)->setCallbacks(new ResetCB());

    s_goal = make_char(svc, GOAL_UUID, R | W);
    s_goal->setCallbacks(new GoalCB());
    ble_set_goal_value(goal);

    make_char(svc, TIME_UUID, W)->setCallbacks(new TimeCB());

    s_sens = make_char(svc, SENS_UUID, R | W);
    s_sens->setCallbacks(new SensCB());
    ble_set_sens_value(sens);

    s_imu = make_char(svc, IMU_UUID, R | N);
    s_imu->setValue(zero, 8);

    s_batt = make_char(svc, BATT_UUID, R | N);
    s_batt->setValue(zero, 4);

    s_clock = make_char(svc, CLOCK_UUID, R | N);
    s_clock->setValue(zero, 8);

    for (int i = 0; i < SET_COUNT; i++) {
        SettingChar &s = s_set[i];
        s.chr = make_char(svc, s.uuid, R | W | N);
        s.chr->setCallbacks(new SettingCB(&s));
        uint8_t v = settings[i];
        s.chr->setValue(&v, 1);
    }

    s_layout = make_char(svc, LAYOUT_UUID, R | W | N);
    s_layout->setCallbacks(new LayoutCB());

    make_char(svc, NAV_UUID, W | BLECharacteristic::PROPERTY_WRITE_NR)
        ->setCallbacks(new NavCB());

    make_char(svc, TMR_CMD_UUID, W)->setCallbacks(new TmrCmdCB());
    s_tmr_state = make_char(svc, TMR_STATE_UUID, R | N);

    svc->start();

    BLEAdvertising *adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(SERVICE_UUID);
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);
    adv->setMaxPreferred(0x12);
    BLEDevice::startAdvertising();

    Serial.println("[BLE] advertising as 'StepCounter'");
}

// ── Notify (value is always kept current so READ works too) ───────────
void ble_notify_steps(uint32_t steps) {
    s_steps->setValue((uint8_t*)&steps, 4);
    if (s_connected) s_steps->notify();
}

void ble_notify_imu(int16_t ax, int16_t ay, int16_t az, int16_t mag) {
    int16_t buf[4] = {ax, ay, az, mag};
    s_imu->setValue((uint8_t*)buf, 8);
    if (s_connected) s_imu->notify();
}

void ble_notify_battery(uint8_t pct, uint16_t battery_mv) {
    uint8_t buf[4] = {pct, 0, (uint8_t)(battery_mv & 0xFF), (uint8_t)(battery_mv >> 8)};
    s_batt->setValue(buf, 4);
    if (s_connected) s_batt->notify();
}

void ble_notify_clock(const uint8_t pkt[8]) {
    s_clock->setValue((uint8_t*)pkt, 8);
    if (s_connected) s_clock->notify();
}

void ble_set_goal_value(uint32_t goal) { s_goal->setValue((uint8_t*)&goal, 4); }
void ble_set_sens_value(uint8_t sens)  { s_sens->setValue(&sens, 1); }

void ble_set_setting(BleSetting id, uint8_t value) {
    SettingChar &s = s_set[id];
    s.chr->setValue(&value, 1);
    if (s_connected) s.chr->notify();
}

// ── Pop pending ───────────────────────────────────────────────────────
bool ble_is_connected() { return s_connected; }

bool ble_pop_pending_goal(uint32_t *out) {
    if (!s_goal_pending) return false;
    *out = s_goal_value;
    s_goal_pending = false;
    return true;
}

bool ble_pop_pending_sensitivity(uint8_t *out) {
    if (!s_sens_pending) return false;
    *out = s_sens_value;
    s_sens_pending = false;
    return true;
}

bool ble_pop_pending_time(uint8_t buf[7]) {
    if (!s_time_pending) return false;
    for (int i = 0; i < 7; i++) buf[i] = s_time_buf[i];
    s_time_pending = false;
    return true;
}

bool ble_pop_reset_request() {
    if (!s_reset_pending) return false;
    s_reset_pending = false;
    return true;
}

bool ble_pop_setting(BleSetting id, uint8_t *out) {
    SettingChar &s = s_set[id];
    if (!s.pending) return false;
    *out = s.value;
    s.pending = false;
    return true;
}
