# Changelog

All notable changes are documented here. The project follows
[Semantic Versioning](https://semver.org/) while firmware is pre-1.0.

## [Unreleased]

Changes under active evaluation on the `testing` branch.

## [0.14.0] - 2026-08-31

### Added

- Persistent rider profile that learns stable gyro and acceleration-residual
  distributions without changing the safety-critical controller state machine;
  the feature is opt-in and disabled by default.
- Advisory-only 50 Hz shadow event model with two seconds of pre-event context,
  compact feature extraction, and a twelve-event NVS history.
- Rider Model Lab in Settings for intentional, nuisance, and missed-event
  feedback plus downloadable feature CSV data.
- Leakage-safe offline logistic model shaping tool with whole-ride validation.
- Explicit adaptive baseline states for IMU, controller, motion, and external
  acceleration holds.

### Changed

- Firmware version advanced to `v0.14.0` for calibration and filtering work.
- Pitch and roll now use time-based complementary filtering that reduces
  accelerometer correction during launches, braking, bumps, and vibration.
- Gyro rate used by warning and adaptive-baseline logic is separately smoothed.
- Adaptive freeze rate now respects a bounded, measured three-sigma noise floor.

### Fixed

- Turning the high-angle warning pattern Off no longer suppresses the ordinary
  wheelie light while the warning state is active.

## [0.13.0] - 2026-08-30

First versioned release of the mounting, secure OTA, telemetry, and Rider HUD
work developed after v0.11.0.

### Added

- First-run OLED mounting wizard that detects upright, roll, and pitch axes from
  a stationary pose followed by gentle side-to-side leaning.
- Persistent orientation storage in device NVS plus a settings-page rerun flow.
- Live roll/lean angle and roll-rate telemetry for future lean displays.
- ECDSA P-256 signed `.wctrl` OTA packages with streamed SHA-256 verification.
- Signed hardware, chip, channel, version, commit, date, and size metadata that
  blocks incompatible or unsigned OTA images before partition activation.
- Rolling `stable` and `testing` OTA releases sourced from `main` and `testing`.
- Per-client write-endpoint rate limiting and write-token rotation after saved
  configuration changes.
- Optional per-device Wi-Fi password generation from the Settings page.
- Firmware build commit, build date, release channel, and target diagnostics.
- Opt-in 5 Hz ride telemetry limited to 90 minutes and the newest three sessions.
- Downloadable HTML ride reports and CSV data with sample-stream SHA-256 proofs.
- Rider HUD with calibrated lean angle, warning/trigger markers, sunlight mode,
  and shared hold-to-arm/disarm controls.
- Stable-only one-time raw OTA bridge for migrating v0.11.0 devices to signed
  firmware without requiring a USB reflash.

### Changed

- Dashboard OTA now accepts signed `.wctrl` packages only; raw `.bin` files
  remain available solely for wired/factory flashing and the documented
  v0.11.0-to-stable migration bridge.
- Rolling and tagged release titles, manifests, and firmware assets retain the
  `v0.13.0` firmware version while channel metadata remains separate.

## [0.11.0] - 2026-08-29

### Added

- Dynamic linear +G telemetry with gravity removal and session peaks.
- High-angle and pitch-rate warning logic with configurable light patterns.
- Wi-Fi recovery controls, captive DNS, mDNS, OTA updates, and rollback.
- Interactive desktop component simulator and repeatable riding scenarios.
- Native device-free regression suite covering production controller logic.
- Automated CI builds and tagged firmware release packaging.
- Reproducible release builds pinned to Arduino-ESP32 3.3.11 / ESP-IDF 5.5.5.

### Safety

- Output defaults off, shuts down on sustained IMU failure, and remains off
  during firmware updates, calibration failures, and standby operation.

[Unreleased]: https://github.com/cronular/motorcycle-wheelie-lighting-controller/compare/v0.13.0...HEAD
[0.13.0]: https://github.com/cronular/motorcycle-wheelie-lighting-controller/compare/v0.11.0...v0.13.0
[0.11.0]: https://github.com/cronular/motorcycle-wheelie-lighting-controller/releases/tag/v0.11.0
