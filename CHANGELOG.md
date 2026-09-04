# Changelog

All notable changes are documented here. The project follows
[Semantic Versioning](https://semver.org/) while firmware is pre-1.0.

## [Unreleased]

Changes under active evaluation on the `testing` branch.

## [0.16.0] - 2026-09-04

### Added

- Phone-guided mounting wizard with dedicated upright, side-to-side lean, and
  level-calibration stages, progress feedback, retry handling, and screen wake
  lock support where available.
- Embedded-web regression coverage for the complete mounting wizard flow.

### Changed

- First-time mounting setup now waits until the web interface is available
  instead of requiring the rider to follow instructions on the OLED during
  boot. Safe default axes remain active until setup is saved.
- Firmware version advanced to `v0.16.0`.

## [0.15.1] - 2026-09-01

### Added

- Dedicated Data Capture dashboard with live controller, IMU, pitch, and saved
  event status, a large event marker, and one-tap CSV download.
- A distinct `CAPTURE` operating mode that samples the existing 50 Hz event
  buffer while forcing the physical light output off.

### Changed

- Capture mode exits to Standby through both dashboard and physical-button
  controls, and rejects event markers until its two-second buffer is ready.
- Firmware version advanced to `v0.15.1`.

## [0.15.0] - 2026-08-31

### Added

- Fixed-capacity, allocation-free firmware module registry with independently
  ordered startup and loop phases for future subsystem modules.
- Module authoring template and architecture documentation covering ownership,
  lifecycle ordering, safety boundaries, and verification requirements.

### Changed

- Firmware version advanced to `v0.15.0` for the modular architecture work.
- Split the 3,902-line firmware monolith into sensor, output, storage, network,
  dashboard, and operating-mode implementation units with an explicit shared
  runtime and API boundary.
- Reduced `src/main.cpp` to the application composition root and hardware-safe
  startup/loop orchestration.

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

[Unreleased]: https://github.com/cronular/motorcycle-wheelie-lighting-controller/compare/v0.16.0...HEAD
[0.16.0]: https://github.com/cronular/motorcycle-wheelie-lighting-controller/compare/v0.15.0...v0.16.0
[0.15.1]: https://github.com/cronular/motorcycle-wheelie-lighting-controller/compare/v0.15.0...v0.16.0
[0.15.0]: https://github.com/cronular/motorcycle-wheelie-lighting-controller/compare/v0.14.0...v0.15.0
[0.14.0]: https://github.com/cronular/motorcycle-wheelie-lighting-controller/compare/v0.13.0...v0.14.0
[0.13.0]: https://github.com/cronular/motorcycle-wheelie-lighting-controller/compare/v0.11.0...v0.13.0
[0.11.0]: https://github.com/cronular/motorcycle-wheelie-lighting-controller/releases/tag/v0.11.0
