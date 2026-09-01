<div align="center">

# Motorcycle Wheelie Lighting Controller

**Motion-aware motorcycle lighting firmware with a built-in dashboard and a device-free simulation lab.**

[![Firmware CI](https://github.com/cronular/motorcycle-wheelie-lighting-controller/actions/workflows/ci.yml/badge.svg)](https://github.com/cronular/motorcycle-wheelie-lighting-controller/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/cronular/motorcycle-wheelie-lighting-controller?display_name=tag)](https://github.com/cronular/motorcycle-wheelie-lighting-controller/releases/latest)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-XIAO_ESP32--S3-orange)](https://platformio.org/)
[![Tests](https://img.shields.io/badge/native_tests-35-brightgreen)](#test-without-hardware)
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
- Lean-aware Rider HUD with threshold markers, warning banner, and sunlight mode.
- Local Wi-Fi dashboard, captive portal, OTA update, and rollback support.
- Signed, board-locked OTA packages with stable and testing channels.
- Opt-in, bounded ride telemetry with downloadable reports and CSV data.
- Advisory rider model with bounded event features and explicit rider labels.
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
python -m unittest discover -s test -p "test_*.py"
```

PlatformIO provisions the project-local Windows compiler automatically. The
suite executes the same portable helpers used by the firmware and covers state
transitions, timing, hysteresis, rollover, adaptive tracking, warnings, patterns,
fades, mounting-axis detection, OTA compatibility, write throttling, validation,
complete wheelie profiles, adaptive sensor confidence, and bounded rider-model
features. Python tests additionally exercise signed firmware packaging and the
offline model-shaping pipeline.

## Ride telemetry and reports

Ride logging is **disabled by default** and can be enabled from Settings. While
enabled, each ARMED-to-STANDBY cycle records a session at 5 Hz for up to 90
minutes. The controller retains only the newest three sessions in LittleFS and
automatically overwrites the oldest. Compact 16-byte samples keep the maximum
three-session footprint to about 1.24 MiB and reserve at least 256 KiB of the
default 1.5 MiB data partition.

The Settings page can download a styled ride report or the underlying CSV. The
`DEVICE-RECORDED · SENSOR DATA · SHA-256` mark means that the report came from
controller telemetry and carries a sample-stream checksum. It is not a
third-party certification or, in this version, a hardware-backed device
signature. Simulator exports are separately marked `SIMULATOR-RECORDED`.

## Rider dashboard

The phone dashboard keeps controller mode, connection state, light output, and
ride-recording status visible at all times. Its Rider HUD combines pitch,
trigger/warning markers, an explicit high-angle warning, and an optional lean
gauge driven by the mounting wizard's calibrated roll axis. The Telemetry view
adds roll rate, recent pitch history, event summaries, and shortcuts to the
latest ride report. Arming requires a deliberate hold, while calibration is a
separate control. Dark, day, and high-contrast sunlight themes are available.

## Rider model lab

v0.14 adds an optional, advisory-only rider model alongside the deterministic
controller. It is disabled by default and must be enabled in Settings.
It learns the stable gyro and acceleration-residual distributions for this
motorcycle, derives a bounded noise-aware baseline freeze recommendation, and records up
to twelve compact 50 Hz event summaries. Two seconds of pre-event samples feed
feature extraction; raw high-rate streams are not retained, so the feature
history remains small and ride telemetry keeps its existing three-session cap.

The Settings page shows model confidence and recent shadow scores. A rider can
label an event as intentional or nuisance, or mark a missed event. The score is
diagnostic only: it cannot arm the controller, trigger the light, relax IMU
health checks, or modify warning limits.

Download feature CSV files from several rides and shape a transparent logistic
model with:

```powershell
python tools/shape_rider_model.py exports\ride-1.csv exports\ride-2.csv -o rider-model.json
```

The tool ignores unlabeled events, requires both positive and nuisance labels,
balances the two classes, and validates by holding out complete ride sessions
instead of randomly mixing adjacent events. Treat results from fewer than 20
labeled events across multiple rides as preliminary. Generated JSON remains an
offline research artifact until its versioned coefficients are deliberately
reviewed, tested, and added to firmware.

## Secure OTA updates

Dashboard OTA accepts signed `.wctrl` packages—not raw `.bin` files. Before an
update partition is activated, the device verifies:

- the ECDSA P-256 manifest signature;
- the streamed firmware SHA-256 and signed byte count;
- `seeed_xiao_esp32s3` / `esp32s3` hardware compatibility;
- the device's selected `stable` or `testing` channel.

`main` publishes the rolling `ota-stable` release and `testing` publishes
`ota-testing`. Release titles and downloadable firmware filenames retain the
embedded version number, while the manifest separately records the stable or
testing channel. Tagged `v*` releases must come from `main`, must match the
embedded firmware version, and publish a signed OTA package plus a detached
signature for the release checksum file. Raw factory images are retained for
wired recovery. The Settings page selects the accepted OTA channel and shows
build commit, build date, compiled release channel, and hardware target.

### Migrating from v0.11.0

v0.11.0 predates signed `.wctrl` packages, but its Settings page can perform a
one-time web OTA using the versioned `v0.11-migration.bin` asset from the
`ota-stable` or matching tagged release. Upload that raw migration image while
running v0.11.0, wait for the controller to reboot, then use signed `.wctrl`
packages for every later update. The migration image is the same stable app
image carried inside the signed package and is covered by the release's signed
`SHA256SUMS.txt`. A USB reflash is only needed if the old web updater is
unavailable or fails.

The trusted public key and key-rotation procedure are documented in
[security/README.md](security/README.md). The private key must only be stored in
the `FIRMWARE_SIGNING_KEY_PEM` Actions secret or an encrypted offline backup.

## Interactive simulator

```powershell
python tools/run_simulator.py
```

The lab opens at <http://127.0.0.1:8765/> with virtual XIAO, MPU6050, OLED,
MOSFET, and lighting components. It includes manual pitch, lean, gyro, and
acceleration injection plus clean wheelie, short-blip, hill, warning, and
IMU-disconnect scenarios. A simulated
rider phone runs the exact dashboard and settings pages embedded in the firmware,
with live telemetry and controls bridged to the virtual motorcycle. Press
`Ctrl+C` to stop it, or smoke-test it with
`python tools/run_simulator.py --check`.

The current `main` version is also hosted as a
[live GitHub Pages simulator](https://cronular.github.io/motorcycle-wheelie-lighting-controller/).

## Development workflow

- `main` is the current releasable firmware.
- `testing` is the integration branch for firmware and simulator changes.
- Pull requests flow from `testing` into `main` after CI passes.
- Branch pushes refresh signed `ota-stable` or `ota-testing` channel packages.
- Version tags publish signed `.wctrl` OTA packages, signed SHA-256 checksums,
  and a combined factory image when the PlatformIO build produces one.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the complete workflow and
[CHANGELOG.md](CHANGELOG.md) for release history. The firmware module layout,
lifecycle ordering, and extension scaffold are documented in
[docs/architecture.md](docs/architecture.md).

## Simulation boundary

Desktop simulation provides fast logic feedback, but final release checks must
still cover real MPU noise and mounting, I²C wiring, OLED orientation, USB/Wi-Fi
behavior, and MOSFET/load operation.
