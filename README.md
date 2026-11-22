# expert-couscous — Watch Lab (Round Watch + companion app)

A customizable smartwatch/pendant built on the **Seeed XIAO ESP32-S3 (Sense) +
Round Display for XIAO**, with a native iOS companion app ("Watch Lab") that
configures it over BLE.

This is a monorepo holding both halves of the project:

```
expert-couscous/
├── app/                      # Flutter iOS app ("Watch Lab")
│   └── lib/                  #   BLE client, face designer, nav, stopwatch/timer
├── firmware/
│   └── RoundWatchESP32/      # Arduino (ESP32-S3) watch firmware
└── .github/workflows/
    └── ios-build.yml         # builds an unsigned .ipa on a macOS runner
```

## Features (current)

- Two watch faces — **Digital** and **Pendant** (analog) — each with per-element
  layout (position/size/colour/visibility) designed live from the app.
- Live clock sync, battery %, screen brightness, 12/24 h, screen-sleep timer.
- Turn-by-turn **navigation** overlay driven by the app (Apple Maps).
- **Stopwatch + countdown timer**, controllable from the watch (touch overlay)
  and the app, mirrored both ways over BLE.

## Build & flash

**Firmware** (arduino-cli, board on COM17):

```
arduino-cli compile --fqbn "esp32:esp32:XIAO_ESP32S3:PartitionScheme=max_app_8MB,PSRAM=opi,USBMode=hwcdc" \
  --libraries "C:\Users\prade\Documents\Arduino\libraries" firmware/RoundWatchESP32
arduino-cli upload  --fqbn "esp32:esp32:XIAO_ESP32S3:PartitionScheme=max_app_8MB,PSRAM=opi,USBMode=hwcdc" \
  --port COM17 firmware/RoundWatchESP32
```

**App** — pushing to `main` builds an unsigned `.ipa` via GitHub Actions
(`watch-lab-ipa` artifact), then sideload with Sideloadly. Locally:
`cd app && flutter pub get && flutter analyze && flutter test`.

See **[HANDOFF.md](HANDOFF.md)** for full protocol details, history, and the
current to-do list.
