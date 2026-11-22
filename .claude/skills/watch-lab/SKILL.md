---
name: watch-lab
description: >-
  Develop the "Watch Lab" round smartwatch in this repo — the Seeed XIAO
  ESP32-S3 (Sense) + Round Display firmware (firmware/RoundWatchESP32) and its
  Flutter iOS companion app (app/). Use when asked to build/flash the firmware,
  build or sideload the iOS IPA, change the BLE protocol, add a watch face /
  setting / overlay feature, debug the device over serial, or "continue" the
  project to-do list. Read HANDOFF.md first.
---

# Watch Lab — build & extend

Monorepo: `app/` (Flutter companion) + `firmware/RoundWatchESP32/` (ESP32-S3).
**Always read `HANDOFF.md` at the repo root first** — it holds the live to-do
list, project history, and the full BLE protocol.

## Environment (this PC)
- Board on **COM17**, MAC `cc:ba:97:0e:ff:0c`. ⚠️ COM3 is a *different* board (an
  AirPlay receiver) — never flash it. Verify the MAC in esptool output.
- arduino-cli: `C:\Users\prade\AppData\Local\Programs\arduino-cli\arduino-cli.exe`.
  The `package_seeedstudio_index.json` 403 warning is harmless — ignore it.
- Flutter SDK at `C:\src\flutter\bin`; prefix PowerShell calls with
  `$env:Path = "C:\src\flutter\bin;" + $env:Path`.
- Libraries: `C:\Users\prade\Documents\Arduino\libraries` (lvgl 8.3.x, Seeed
  Arduino Round display, TFT_eSPI).

## Firmware: compile → flash → verify
FQBN: `esp32:esp32:XIAO_ESP32S3:PartitionScheme=max_app_8MB,PSRAM=opi,USBMode=hwcdc`

```
arduino-cli compile --fqbn <FQBN> --libraries <LIBS> firmware/RoundWatchESP32
arduino-cli upload  --fqbn <FQBN> --port COM17 firmware/RoundWatchESP32
```

Confirm the boot over USB-CDC with .NET SerialPort (toggle RTS to force a reset),
NOT a blocking monitor:

```powershell
$p = New-Object System.IO.Ports.SerialPort('COM17',115200); $p.ReadTimeout=1000
$p.Open(); Start-Sleep -Milliseconds 150
$p.RtsEnable=$true; Start-Sleep -Milliseconds 120; $p.RtsEnable=$false
$deadline=(Get-Date).AddSeconds(7)
while((Get-Date)-lt $deadline){ try { $p.ReadLine() } catch {} }
$p.Close()
```
Look for `[SYS] init complete` and `[BLE] advertising as 'StepCounter'`.

## App: analyze → test → build IPA
```
cd app
flutter pub get
flutter analyze      # MUST print "No issues found!" (project treats info lints as failing)
flutter test         # includes a phone-width (375px) RenderFlex-overflow guard
```
- **Import `package:material_ui/material_ui.dart`, not `package:flutter/material.dart`**
  (the latter is deprecated in this Flutter build and fails analyze). Self-contained
  pages define their own colour consts; `main.dart` has `class C`.
- IPA builds on GitHub Actions: push `app/**` to `main` → workflow
  `.github/workflows/ios-build.yml` (macOS runner) → artifact **`watch-lab-ipa`**.
  Watch with the flutter-ios-sideload skill's `watch.ps1 -Repo pradeeplogu0/expert-couscous -Sha <sha>`.
  **Pushing to `main` needs the user's explicit OK** (the auto-mode classifier
  blocks default-branch pushes) — ask before pushing.

## BLE protocol
Service `4b0ad7a0-4b2c-11ee-be56-0242ac120002`, chars `4b0ad7XX-…`:
`a1` steps · `a2` reset · `a3` goal · `a4` time · `a5` sens · `a6` imu · `a7` batt
· `a8` clock · `a9` rotation · `aa` brightness · `ab` 12/24h · `ac` face ·
`ad` sleep · `ae` layout · `af` nav · `b1` timer-cmd · `b2` timer-state.

- **One-byte settings (a9–ad)** are table-driven: to add one, append to
  `SettingChar s_set[]` in `ble_service.cpp` with a new UUID, add the enum to
  `ble_service.h`, and extend `SET_KEYS/SET_DEFAULT/SET_LO/SET_HI` + the `g_set`
  initializer in the .ino. The loop auto-creates, persists, and notifies it.
- **Layout (ae):** write `[face, element, visible, x, y, size, color]` (7 B) to
  edit one element, `0xFF` to reset. Element ids in `enum ElId`.
- **Timer (b1 cmd / b2 state):** cmd `0x01/02/03` sw start/stop/reset,
  `0x10/11/12` ct start/stop/reset, `0x13`+u32ms ct-set, `0x14` ct-ack. state 15 B:
  `[0]`sw_run `[1..4]`sw_elapsed_ms `[5]`ct_run `[6..9]`ct_remaining
  `[10..13]`ct_duration `[14]`flags(bit0 alarm).
- In the app, make new characteristics **optional** (`opt(uuid)` → null) so older
  firmware still connects; drive controllers through a `resolveWriter` closure
  (see `NavController`, `TimerController`).

## Firmware conventions
- **ESP32 core 3.3+ runs NimBLE** under the `BLEDevice.h` API: just call
  `characteristic->notify()` — do NOT add a manual `BLE2902`/`0x2902` descriptor
  or gate notify on a CCCD flag; those callbacks never fire and notifications stop.
- Full-screen overlays (nav, stopwatch/timer) follow one pattern: a hidden
  240×240 opaque layer created as a child of the screen, shown with
  `lv_obj_clear_flag(HIDDEN)` + `lv_obj_move_foreground()`. See `nav_face.cpp`,
  `timer_face.cpp`.
- Fonts available via lv_conf: montserrat 14 / 16 / 20 / 48 only.

## Known hardware facts
- ⚠️ **No IMU on this unit** (`WHO_AM_I=0x00`, only RTC 0x51 on I2C) — step
  counter and any accelerometer feature are inert until an LSM6DS3 is wired.
- **Battery** is on D0 via a 1:2 divider; `analogReadMilliVolts × 2`. It reads
  ~80 mV low at full charge (ADC upper-range nonlinearity + divider tolerance), so
  % caps near 92 % against the `4200 mV = 100 %` curve. Fix = calibrate (scale
  `bat_mv` or shift the top of `LIPO_MV`).
- Firmware screen "sleep" only turns the backlight off; CPU + BLE stay on
  (~35–55 mA idle). Real battery life on a small LiPo is a few hours; adding BLE
  modem-sleep / CPU light-sleep is the big runtime win.

## Gotchas
- Windows PowerShell `Compress-Archive` and .NET-Framework `ZipFile` write
  **backslash** zip paths that mangle on macOS/Linux; build zips entry-by-entry
  with forward slashes if delivering cross-platform.
- The sandbox classifier blocks `Remove-Item -Recurse -Force` and direct `main`
  pushes — use `[System.IO.Directory]::Delete()` for cleanup and ask before pushing.
