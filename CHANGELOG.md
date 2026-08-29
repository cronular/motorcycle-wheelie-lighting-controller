# Changelog

All notable changes are documented here. The project follows
[Semantic Versioning](https://semver.org/) while firmware is pre-1.0.

## [Unreleased]

Changes under active evaluation on the `testing` branch.

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

[Unreleased]: https://github.com/cronular/motorcycle-wheelie-lighting-controller/compare/v0.11.0...HEAD
[0.11.0]: https://github.com/cronular/motorcycle-wheelie-lighting-controller/releases/tag/v0.11.0
