#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <U8x8lib.h>
#include <esp_arduino_version.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <Update.h>
#include "controller_core.h"
#include "ride_log_format.h"
#include "rider_model.h"
#include "firmware_signing_key.h"
#include "build_metadata.generated.h"

namespace firmware {

// =====================================================
// FINAL-HARDWARE PIN ASSIGNMENTS
// =====================================================

#define I2C_SDA_PIN       D4
#define I2C_SCL_PIN       D5
#define USER_BUTTON_PIN   D1
// D6 is GPIO43/UART TX on the XIAO ESP32-S3 and pulses during ROM boot.
// D0/GPIO1 stays suitable for the MOSFET trigger's external pulldown.
#define OUTPUT1_PWM_PIN   D0

// =====================================================
// FIRMWARE
// =====================================================

constexpr const char* FIRMWARE_VERSION = "v0.15.0";
constexpr const char* TARGET_BOARD_ID = "seeed_xiao_esp32s3";
constexpr const char* TARGET_CHIP_ID = "esp32s3";

#ifndef BUILD_COMMIT
#define BUILD_COMMIT "unknown"
#endif
#ifndef BUILD_DATE
#define BUILD_DATE "unknown"
#endif
#ifndef RELEASE_CHANNEL
#define RELEASE_CHANNEL "testing"
#endif

// =====================================================
// WIFI / WEB
// =====================================================

constexpr const char* WIFI_AP_SSID_PREFIX = "wheelie_controller_";
constexpr const char* DEFAULT_WIFI_AP_PASSWORD = "wheeliectrl";
constexpr const char* MDNS_HOSTNAME = "wheelie";
constexpr uint16_t DNS_PORT = 53;

// Password is stored in NVS. The physical button can restore the default.
inline String wifiApPassword = DEFAULT_WIFI_AP_PASSWORD;
inline String wifiApSsid;

inline IPAddress AP_IP(192, 168, 4, 1);
inline IPAddress AP_GATEWAY(192, 168, 4, 1);
inline IPAddress AP_SUBNET(255, 255, 255, 0);

inline WebServer server(80);
inline DNSServer dnsServer;

inline bool dnsHealthy = false;
inline bool wifiHealthy = false;
inline bool mdnsHealthy = false;
inline bool accessPointEnabled = false;
inline bool webRoutesRegistered = false;
inline String writeToken;
inline String otaChannel = RELEASE_CHANNEL;

constexpr size_t WRITE_RATE_BUCKET_COUNT = 4;
inline WriteRateLimitBucket writeRateBuckets[WRITE_RATE_BUCKET_COUNT];

// A password change needs to let the HTTP response leave before the AP
// disconnects. The main loop performs the restart after this delay.
inline bool accessPointRestartPending = false;
inline unsigned long accessPointRestartAtMs = 0;
inline unsigned long wifiPasswordResetNoticeUntil = 0;

struct FirmwareUploadState {
    bool authorized = false;
    bool rateLimited = false;
    bool failed = false;
    bool updateStarted = false;
    bool hashInitialized = false;
    bool complete = false;
    String error;
    uint8_t header[FIRMWARE_PACKAGE_HEADER_SIZE] = {};
    size_t headerReceived = 0;
    char manifest[FIRMWARE_MANIFEST_MAX_SIZE + 1] = {};
    size_t manifestExpected = 0;
    size_t manifestReceived = 0;
    uint8_t signature[FIRMWARE_SIGNATURE_MAX_SIZE] = {};
    size_t signatureExpected = 0;
    size_t signatureReceived = 0;
    uint32_t firmwareExpected = 0;
    uint32_t firmwareReceived = 0;
    String expectedSha256;
    String packageVersion;
    String packageCommit;
    String packageBuilt;
    mbedtls_sha256_context hashContext;
};

inline FirmwareUploadState firmwareUpload;

// =====================================================
// PERSISTENT SETTINGS
// =====================================================

inline Preferences preferences;

inline ControllerSettings settings;

// =====================================================
// OLED
// Generic 128x64 SSD1306 I2C display, including
// GME12864-12 and the development expansion-board OLED.
//
// IMPORTANT: U8x8 initializes the shared I2C bus.
// DO NOT call Wire.begin() separately.
// =====================================================

inline U8X8_SSD1306_128X64_NONAME_HW_I2C oled(
    U8X8_PIN_NONE,
    I2C_SCL_PIN,
    I2C_SDA_PIN
);

constexpr unsigned long DISPLAY_REFRESH_MS = 100;

// =====================================================
// PWM OUTPUT
// =====================================================

constexpr uint32_t PWM_FREQUENCY = 1000;
constexpr uint8_t PWM_RESOLUTION = 8;
constexpr uint8_t PWM_CHANNEL = 0; // Arduino-ESP32 2.x only

inline uint8_t outputBrightness = 0;      // actual PWM value
inline uint8_t outputTarget = 0;          // requested PWM value
inline uint8_t fadeStartBrightness = 0;
inline unsigned long fadeStartMs = 0;
inline uint32_t activeFadeDurationMs = 0;
inline bool fadeActive = false;

// Manual output test is only available in STANDBY.
constexpr unsigned long MANUAL_TEST_TIMEOUT_MS = 10000;
inline bool manualTestActive = false;
inline unsigned long manualTestStartMs = 0;

// =====================================================
// MPU6050
// =====================================================

constexpr uint8_t MPU_ADDR = 0x68;
constexpr uint8_t PWR_MGMT_1 = 0x6B;
constexpr uint8_t CONFIG_REG = 0x1A;
constexpr uint8_t GYRO_CONFIG = 0x1B;
constexpr uint8_t ACCEL_CONFIG = 0x1C;
constexpr uint8_t ACCEL_XOUT_H = 0x3B;

// v0.10 uses +/-4g instead of +/-2g so the live G-load display
// has useful headroom for bumps, launches, and landings.
constexpr uint8_t ACCEL_RANGE_CONFIG_VALUE = 0x08; // AFS_SEL=1, +/-4g
constexpr float ACCEL_LSB_PER_G = 8192.0f;

// =====================================================
// PITCH FILTER
// =====================================================

constexpr float ATTITUDE_FILTER_TIME_CONSTANT_SEC = 0.49f;
constexpr float GYRO_RATE_FILTER_TIME_CONSTANT_SEC = 0.04f;
constexpr float PITCH_SIGN = -1.0f; // front rising = positive

// =====================================================
// IMU CALIBRATION
// =====================================================

// Calibration is intentionally tolerant of normal engine-idle vibration.
// It averages the acceleration vector, not per-sample pitch angles.
constexpr int CALIBRATION_ACCEPTED_SAMPLES = 1500;
constexpr unsigned long CALIBRATION_TIMEOUT_MS = 10000;
constexpr uint8_t INITIAL_CALIBRATION_DELAY_SECONDS = 3;
constexpr unsigned long CALIBRATION_FLASH_MS = 200;
constexpr float CAL_ACCEL_MAG_MIN_G = 0.65f;
constexpr float CAL_ACCEL_MAG_MAX_G = 1.35f;
constexpr float CAL_MAX_GYRO_AXIS_DPS = 30.0f;

inline float calibrationAccelNoiseRms = 0.0f;
inline float calibrationGyroNoiseRms = 0.0f;
inline bool calibrationHighVibration = false;

// =====================================================
// BUTTON
// =====================================================

constexpr unsigned long BUTTON_DEBOUNCE_MS = 35;
constexpr unsigned long BUTTON_LONG_PRESS_MS = 1500;
constexpr unsigned long BUTTON_WIFI_PASSWORD_RESET_MS = 30000;
constexpr unsigned long BUTTON_MULTI_TAP_MS = 400;

inline bool lastRawButtonState = HIGH;
inline bool debouncedButtonState = HIGH;
inline unsigned long lastButtonChangeTime = 0;
inline unsigned long buttonPressStartTime = 0;
inline bool longPressHandled = false;
inline bool longPressGestureActive = false;

// Short taps are collected into one gesture window:
// 1 tap = next OLED page
// 2 taps = ABSOLUTE <-> ADAPTIVE
// 3 taps = Wi-Fi access point ON/OFF
inline uint8_t shortTapCount = 0;
inline unsigned long lastTapReleaseTime = 0;

// =====================================================
// CONTROLLER TYPES
// =====================================================

struct MPUData {
    float ax;
    float ay;
    float az;
    float gx;
    float gy;
    float gz;
};

enum class OperatingMode {
    ARMED,
    STANDBY
};

enum class DisplayPage {
    STATUS,
    SETTINGS,
    NETWORK,
    DIAGNOSTICS
};

// =====================================================
// GLOBAL CONTROLLER STATE
// =====================================================

inline RotationAxis rotationAxis = RotationAxis::Y;
inline RotationAxis rollAxis = RotationAxis::X;
inline RotationAxis verticalAxis = RotationAxis::Z;
inline bool orientationConfigured = false;
inline float gyroAxisBias = 0.0f;
inline float rollGyroBias = 0.0f;
inline float pitch = 0.0f;
inline float pitchZero = 0.0f;
inline float roll = 0.0f;
inline float rollZero = 0.0f;
inline float levelReferenceX = 0.0f;
inline float levelReferenceY = 0.0f;
inline float levelReferenceZ = 1.0f;

// Absolute pitch is relative to startup/calibration zero.
inline float currentAbsolutePitch = 0.0f;

// Adaptive baseline slowly follows terrain in ADAPTIVE mode.
inline float adaptiveBaseline = 0.0f;

// This is the angle actually sent to the wheelie detector.
inline float currentTriggerPitch = 0.0f;

inline float currentGyroRate = 0.0f;
inline float currentFilteredGyroRate = 0.0f;
inline float currentRollAngle = 0.0f;
inline float currentRollRate = 0.0f;
inline float currentAccelerationTrust = 1.0f;
inline float currentAccelerationMagnitude = 1.0f;
inline float effectiveAdaptiveFreezeRate = 8.0f;

// Live acceleration telemetry. v0.11 reports +G as dynamic linear
// acceleration with the gravity vector removed, so a stationary bike
// settles at approximately +0.00 g instead of ~1 g.
inline float currentAccelX = 0.0f;
inline float currentAccelY = 0.0f;
inline float currentAccelZ = 1.0f;
inline float currentGLoad = 0.0f;

// Calibration normalizes the particular MPU's measured resting gravity
// magnitude (for example 1.12 raw g) back to 1.00 g.
inline float accelScaleCorrection = 1.0f;
inline float calibrationRestMagnitudeRaw = 1.0f;

// Low-pass gravity estimate in sensor coordinates. Subtracting this from
// instantaneous acceleration produces a useful launch/bump +G estimate.
inline float gravityEstimateX = 0.0f;
inline float gravityEstimateY = 0.0f;
inline float gravityEstimateZ = 1.0f;
constexpr float GRAVITY_TRACK_TIME_SEC = 1.50f;
constexpr float GLOAD_SMOOTHING_TIME_SEC = 0.08f;
constexpr float GLOAD_DEADBAND_G = 0.015f;

// Session peaks (not persisted). They reset at calibration or from the
// dashboard's individual reset buttons.
inline float highestAngle = 0.0f;
inline float highestGLoad = 0.0f;

inline bool adaptiveBaselineFrozen = false;
inline AdaptiveFreezeReason adaptiveFreezeReason = AdaptiveFreezeReason::NONE;

inline bool imuHealthy = false;
inline int imuFailureCount = 0;
inline int imuRecoveryCount = 0;
constexpr int IMU_FAILURE_LIMIT = 5;
constexpr int IMU_RECOVERY_LIMIT = 20;
constexpr unsigned long ORIENTATION_UPRIGHT_SAMPLE_MS = 1500;
constexpr unsigned long ORIENTATION_MOTION_SAMPLE_MS = 8000;
constexpr uint8_t ORIENTATION_START_DELAY_SECONDS = 4;

inline unsigned long lastMicros = 0;
inline unsigned long triggerStartTime = 0;
inline unsigned long wheelieStartTime = 0;
inline unsigned long lastDisplayUpdate = 0;
inline unsigned long lastSerialUpdate = 0;

inline ControllerState controllerState = ControllerState::NORMAL;
inline bool highAngleWarningActive = false;
inline unsigned long completedWheelieCount = 0;
inline unsigned long lastWheelieDurationMs = 0;
inline float lastWheeliePeakAngle = 0.0f;
inline float lastWheeliePeakG = 0.0f;
inline float activeWheeliePeakAngle = 0.0f;
inline float activeWheeliePeakG = 0.0f;
inline OperatingMode operatingMode = OperatingMode::STANDBY;
inline DisplayPage displayPage = DisplayPage::STATUS;

inline bool displayDirty = true;

// =====================================================
// BOUNDED RIDE TELEMETRY
// =====================================================

inline bool rideStorageReady = false;
inline bool rideLoggingEnabled = RIDE_LOG_DEFAULT_ENABLED;
inline bool riderModelEnabled = RIDER_MODEL_DEFAULT_ENABLED;
inline bool rideSessionActive = false;
inline bool rideHashInitialized = false;
inline File rideFile;
inline RideLogHeader activeRideHeader;
inline mbedtls_sha256_context rideHashContext;
inline uint8_t activeRideSlot = 0;
inline uint32_t rideSessionStartMs = 0;
inline uint32_t rideNextSampleMs = 0;
inline uint32_t rideWheelieStartCount = 0;
inline uint16_t rideSamplesSinceFlush = 0;

// =====================================================
// RIDER MODEL / HIGH-RATE EVENT FEATURES
// =====================================================

constexpr uint16_t MODEL_SAMPLE_RATE_HZ = 50;
constexpr uint32_t MODEL_SAMPLE_INTERVAL_MS = 1000 / MODEL_SAMPLE_RATE_HZ;
constexpr uint16_t MODEL_PRE_EVENT_SAMPLES = MODEL_SAMPLE_RATE_HZ * 2;
constexpr uint32_t MODEL_POST_EVENT_MS = 2000;
constexpr uint32_t MODEL_PROFILE_SAVE_INTERVAL_MS = 5UL * 60UL * 1000UL;

struct ModelRawSample {
    uint32_t timeMs = 0;
    float pitch = 0.0f;
    float pitchRate = 0.0f;
    float gLoad = 0.0f;
    float roll = 0.0f;
    bool baselineFrozen = false;
};

inline RiderProfile riderProfile;
inline ModelEventHistory modelEventHistory;
inline ModelEventAccumulator modelEventAccumulator;
inline ModelRawSample modelPreEventRing[MODEL_PRE_EVENT_SAMPLES];
inline uint16_t modelPreEventHead = 0;
inline uint16_t modelPreEventCount = 0;
inline uint32_t modelNextSampleMs = 0;
inline uint32_t modelFinishAtMs = 0;
inline uint32_t modelLastProfileSaveMs = 0;
inline ModelEventOutcome modelPendingOutcome = ModelEventOutcome::CANCELLED;
inline ControllerState modelPreviousControllerState = ControllerState::NORMAL;
inline bool modelFinishPending = false;
inline bool riderModelDirty = false;

} // namespace firmware
