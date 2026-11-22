# HANDOFF — Watch Lab (resume here)

_Last updated: 2026-11-22. This file is the single source of truth for where the
project stands and what to do next. Read it first before continuing._

## What this is
A customizable watch/pendant on **Seeed XIAO ESP32-S3 (Sense) + Round Display
for XIAO**, configured over BLE by a native iOS app. Monorepo: `app/` (Flutter)
+ `firmware/RoundWatchESP32/` (Arduino).

## Hardware / environment
- Board on **COM17**, MAC `cc:ba:97:0e:ff:0c`. (COM3 is a *different* board — an
  AirPlay receiver; never flash that one.)
- arduino-cli: `C:\Users\prade\AppData\Local\Programs\arduino-cli\arduino-cli.exe`
  (the Seeed index 403 warning is harmless).
  FQBN `esp32:esp32:XIAO_ESP32S3:PartitionScheme=max_app_8MB,PSRAM=opi,USBMode=hwcdc`,
  libs `C:\Users\prade\Documents\Arduino\libraries` (lvgl 8.3.x, Seeed Round display, TFT_eSPI).
- Flutter SDK: `C:\src\flutter\bin`. App repo was `pradeeplogu0/ipad_demo`; now
  lives here under `app/`. iOS build runs on GitHub Actions (`.github/workflows/ios-build.yml`).
- ⚠️ **No IMU on this unit.** Boot log shows `WHO_AM_I=0x00, present=0`; only the
  RTC (I2C 0x51) answers. So the step counter and any accelerometer feature are
  inert until an LSM6DS3 is wired.

## BLE protocol
Service `4b0ad7a0-4b2c-11ee-be56-0242ac120002`, characteristics `4b0ad7XX-…`:
`a1` steps · `a2` reset · `a3` goal · `a4` time · `a5` sens · `a6` imu · `a7` batt
· `a8` clock · `a9` rotation · `aa` brightness · `ab` 12/24h · `ac` face ·
`ad` sleep · `ae` layout · `af` nav · `b1` timer-cmd · `b2` timer-state.

- One-byte settings (a9–ad): table-driven `SettingChar s_set[]` in
  `ble_service.cpp` + `SET_KEYS/SET_DEFAULT/SET_LO/SET_HI` in the .ino.
- Layout (`ae`): write 7 bytes `[face, element, visible, x, y, size, color]` to
  edit one element, or `0xFF` to reset. Element ids: time, seconds, AM/PM, date,
  batt%, battV, rot, BLE, steps, ring.
- Timer (`b1` cmd / `b2` state): cmd `0x01` sw-start `0x02` sw-stop `0x03` sw-reset
  `0x10` ct-start `0x11` ct-stop `0x12` ct-reset `0x13`+u32ms ct-set `0x14` ct-ack.
  state (15 B): `[0]`sw_run `[1..4]`sw_elapsed_ms `[5]`ct_run `[6..9]`ct_remaining
  `[10..13]`ct_duration `[14]`flags(bit0 alarming).

## File map
- Firmware: `RoundWatchESP32.ino` (faces, settings, battery, loop), `ble_service.*`
  (GATT), `imu.*`, `rtc.*`, `nav_face.*` (nav overlay), `timers.*` (stopwatch/timer
  logic), `timer_face.*` (LVGL stopwatch/timer overlay).
- App: `lib/main.dart` (BLE client + home), `face_designer/face_layout/face_painter`
  (face editor), `nav_engine/nav_page` (navigation), `timers/timer_page`
  (stopwatch/timer).

## History / decisions (newest first)
- **Stopwatch + timer** added, mirrored both ways (`b1/b2`; `timers.*`,
  `timer_face.*`, `timers.dart`, `timer_page.dart`). Long-press on the watch face
  currently opens the overlay (presets 1/5/10 min).
- **Voice notes + auto-rotate were REMOVED.** Voice (PDM mic → BLE IMA-ADPCM) was
  dropped because a 60 s clip took minutes over BLE. Auto-rotate was dropped with
  it (and was inert anyway — no IMU). Do not reintroduce without a faster transport.
- Earlier work: face designer (digital/pendant), live battery/clock/settings,
  turn-by-turn nav.

## ▶ TO DO — requested 2026-11-22 (do these next)
1. **Watch-face colours fully customizable from the app** — background colour per
   face, and every colour (pendant hands, ticks, ring, all text). Today bg is
   hardcoded black and the pendant hands/ticks are hardcoded (`COL_GOLD`,
   `0x3A4656`, white) in `create_face()`. Plan: add a background element + hand/tick
   colours to the layout model (extend `ae`), surface in `face_designer.dart`.
2. **Full iOS-native feel** — app is Material now and feels web/Android on
   scroll/drag. Move to Cupertino styling / native scroll physics. (Confirm scope
   with the user: light theming vs. full `CupertinoApp` rewrite.)
3. **Countdown duration via slider (max 120 min)** — replace the fixed 1/5/10 min
   preset buttons in `timer_page.dart` with a slider; firmware already accepts any
   `ct_set(ms)`. Mirror the chosen value to the watch.
4. **Open the stopwatch/timer by SWIPE, not long-press** — in the firmware replace
   the `on_long_press` toggle with an LVGL gesture (LV_EVENT_GESTURE / swipe). The
   screen-rotate gesture is already gone; remove any remaining rotate path.
5. **Fix stopwatch font overlap on the watch** — in `timer_face.cpp` the big time
   label (`s_time`, 48 px at y=-14) overlaps the tabs/buttons/box. Reposition /
   constrain width / auto-shrink so it sits cleanly.
6. **Battery caps at 92%, should reach 100%** — in `RoundWatchESP32.ino`
   `battery_sample()` / `lipo_pct()`: measured mV underreads (ADC + 1:2 divider
   tolerance) so it never reaches the 4200 mV = 100% point. Next step: read
   `g_bat_mv` on a full charge (serial/BLE), then calibrate (scale factor or shift
   the top of the `LIPO_MV` curve, e.g. ~4100–4150 mV = 100%).

## Resume checklist
1. `cd app && flutter pub get` then work; verify with `flutter analyze` + `flutter test`.
2. Firmware: compile, flash to COM17, confirm boot over serial (RTS-toggle reset,
   115200) — look for `[SYS] init complete` and `[BLE] advertising`.
3. Push `app/` changes to `main` → GitHub Actions builds the IPA → sideload.
