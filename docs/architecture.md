# Firmware architecture

Firmware v0.15 uses a small composition root in `src/main.cpp` and six
subsystem modules under `src/modules/`. The goal is to keep hardware ownership,
safety behavior, persistence, and presentation code independently reviewable
without adding dynamic allocation or a runtime plugin system to the XIAO.

## Module map

| Module | Owns | Must not own |
| --- | --- | --- |
| `operating-mode` | button gestures, arm/standby transitions, controller state machine | sensor transport, PWM implementation, HTTP transport |
| `output` | PWM setup, output targets, fades, manual-test timeout | wheelie detection policy, settings persistence |
| `storage` | NVS settings, rider model, bounded ride files | HTTP routing, sensor sampling |
| `dashboard` | OLED pages, embedded web UI, API presentation and ride downloads | access-point lifecycle, output policy |
| `sensor` | MPU transport, mounting wizard, calibration, filtering, angle/G processing | Wi-Fi and OTA transport |
| `network` | AP/DNS/mDNS, write authorization, route registration, signed OTA transport | control decisions and sensor filtering |

`include/firmware_runtime.h` is the compatibility state boundary for behavior
moved from the former monolith. New modules should keep private state inside
their `.cpp` file and expose the smallest possible interface. Portable control
math belongs in an Arduino-free header, where the native regression suite can
exercise it.

## Lifecycle

Every module provides a `FirmwareModule` descriptor with a name, `begin` hook,
`tick` hook, and independent order values. `ModuleRegistry` stores pointers in
a fixed-size array; it performs no heap allocation.

Startup order protects the safety path:

1. `operating-mode` configures the physical button.
2. `storage` validates settings and prepares bounded logging.
3. `dashboard` initializes the shared I2C bus and OLED.
4. `output` configures PWM after `main.cpp` has already driven the trigger low.
5. `sensor` configures the MPU, runs orientation/calibration, and chooses the
   safe initial operating mode.
6. `network` starts last, after local hardware is ready.

Loop order preserves the v0.14 behavior:

1. service network requests and deferred AP restarts;
2. process button gestures;
3. apply manual-output timeout and PWM fades;
4. sample/filter the IMU and run deterministic control logic;
5. update bounded rider-model and ride storage;
6. refresh OLED and serial diagnostics.

The output pin is explicitly configured LOW in `main.cpp` before the registry
starts any module. A failed calibration or unhealthy IMU still forces the
controller to `NORMAL` and the output off.

## Adding a module

1. Copy `include/modules/module_template.h.example` and
   `src/modules/module_template.cpp.example`, then replace `futureFeature` with
   the feature name.
2. Keep the public header small. Prefer snapshots, commands, or portable helper
   functions over exposing writable globals.
3. Register the descriptor in `registerModules()` and increase the registry
   capacity in `src/main.cpp`.
4. Pick begin/tick order values deliberately. Leave gaps so a later module can
   be inserted without renumbering every descriptor.
5. Put deterministic logic in an Arduino-free header and add native tests.
6. Run the native suite, Python suite, embedded-web check, simulator smoke test,
   and XIAO firmware build. Sensor/output changes still require real hardware
   verification before a stable release.

Do not let a new module arm the controller, weaken IMU failure handling, bypass
signed OTA checks, or write the output pin through a second code path.
