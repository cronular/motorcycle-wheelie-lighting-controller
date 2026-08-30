<div align="center">

# Motorcycle Wheelie Lighting Controller

**Motion-aware motorcycle lighting firmware with a built-in dashboard and a device-free simulation lab.**

[![Firmware CI](https://github.com/cronular/motorcycle-wheelie-lighting-controller/actions/workflows/ci.yml/badge.svg)](https://github.com/cronular/motorcycle-wheelie-lighting-controller/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/cronular/motorcycle-wheelie-lighting-controller?display_name=tag)](https://github.com/cronular/motorcycle-wheelie-lighting-controller/releases/latest)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-XIAO_ESP32--S3-orange)](https://platformio.org/)
[![Tests](https://img.shields.io/badge/native_tests-21-brightgreen)](#test-without-hardware)
[![Live simulator](https://img.shields.io/badge/try-live_simulator-48e0d0)](https://cronular.github.io/motorcycle-wheelie-lighting-controller/)

</div>

Firmware for a Seeed XIAO ESP32-S3, MPU6050, SSD1306 OLED, and PWM-driven
lighting output. It detects sustained motorcycle pitch, applies configurable
hysteresis and warnings, and exposes live telemetry through an onboard Wi-Fi
dashboard.

> [!IMPORTANT]
> This project controls auxiliary lighting only. It is not a safety system and
> must not interfere with required vehicle lighting or rider control.

## Highlights

- Absolute and terrain-following adaptive pitch modes.
- Configurable trigger hold, minimum-on time, brightness, fades, and patterns.
- High-angle and pitch-rate warning behavior with safe IMU-failure shutdown.
- SSD1306 status pages and physical-button gesture controls.
- First-run mounting wizard with persistent pitch/roll axis detection.
- Local Wi-Fi dashboard, captive portal, OTA update, and rollback support.
- Desktop simulator and native regression tests—no connected board required.
- Automated GitHub builds with downloadable firmware artifacts and releases.

## Hardware

| Component | Role | Firmware interface |
| --- | --- | --- |
| Seeed XIAO ESP32-S3 | Controller and Wi-Fi dashboard | Main target |
| MPU6050 | Pitch, gyro rate, and acceleration | I²C `0x68` |
| SSD1306 128×64 | Local status display | I²C on D4/D5 |
| Logic-level MOSFET | Auxiliary-light PWM switching | D0, 1 kHz / 8-bit |
| User button | Page, mode, AP, and recovery gestures | D1, active-low |

## Quick start

Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html),
clone the repository, and run:

```powershell
pio run -e seeed_xiao_esp32s3
pio run -e seeed_xiao_esp32s3 -t upload --upload-port COM13
```

Change `COM13` to the board's current serial port.
The project pins pioarduino `55.03.311` so local and GitHub release builds use
Arduino-ESP32 3.3.11 and ESP-IDF 5.5.5 consistently.

## Test without hardware

```powershell
pio test -e native
```

PlatformIO provisions the project-local Windows compiler automatically. The
suite executes the same portable helpers used by the firmware and covers state
transitions, timing, hysteresis, rollover, adaptive tracking, warnings, patterns,
fades, mounting-axis detection, validation, and complete wheelie profiles.

## Interactive simulator

```powershell
python tools/run_simulator.py
```

The lab opens at <http://127.0.0.1:8765/> with virtual XIAO, MPU6050, OLED,
MOSFET, and lighting components. It includes manual sensor injection plus clean
wheelie, short-blip, hill, warning, and IMU-disconnect scenarios. Press `Ctrl+C`
to stop it, or smoke-test it with `python tools/run_simulator.py --check`.

The current `main` version is also hosted as a
[live GitHub Pages simulator](https://cronular.github.io/motorcycle-wheelie-lighting-controller/).

## Development workflow

- `main` is the current releasable firmware.
- `testing` is the integration branch for firmware and simulator changes.
- Pull requests flow from `testing` into `main` after CI passes.
- Version tags publish a verified OTA `firmware.bin`, SHA-256 checksums, and a
  combined factory image when the selected PlatformIO release produces one.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the complete workflow and
[CHANGELOG.md](CHANGELOG.md) for release history.

## Simulation boundary

Desktop simulation provides fast logic feedback, but final release checks must
still cover real MPU noise and mounting, I²C wiring, OLED orientation, USB/Wi-Fi
behavior, and MOSFET/load operation.
