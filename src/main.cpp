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
#include "firmware_signing_key.h"
#include "build_metadata.generated.h"

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

constexpr const char* FIRMWARE_VERSION = "v0.13.0-testing";
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
String wifiApPassword = DEFAULT_WIFI_AP_PASSWORD;
String wifiApSsid;

IPAddress AP_IP(192, 168, 4, 1);
IPAddress AP_GATEWAY(192, 168, 4, 1);
IPAddress AP_SUBNET(255, 255, 255, 0);

WebServer server(80);
DNSServer dnsServer;

bool dnsHealthy = false;
bool wifiHealthy = false;
bool mdnsHealthy = false;
bool accessPointEnabled = false;
bool webRoutesRegistered = false;
String writeToken;
String otaChannel = RELEASE_CHANNEL;

constexpr size_t WRITE_RATE_BUCKET_COUNT = 4;
WriteRateLimitBucket writeRateBuckets[WRITE_RATE_BUCKET_COUNT];

// A password change needs to let the HTTP response leave before the AP
// disconnects. The main loop performs the restart after this delay.
bool accessPointRestartPending = false;
unsigned long accessPointRestartAtMs = 0;
unsigned long wifiPasswordResetNoticeUntil = 0;

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

FirmwareUploadState firmwareUpload;

// =====================================================
// PERSISTENT SETTINGS
// =====================================================

Preferences preferences;

ControllerSettings settings;

// =====================================================
// OLED
// Generic 128x64 SSD1306 I2C display, including
// GME12864-12 and the development expansion-board OLED.
//
// IMPORTANT: U8x8 initializes the shared I2C bus.
// DO NOT call Wire.begin() separately.
// =====================================================

U8X8_SSD1306_128X64_NONAME_HW_I2C oled(
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

uint8_t outputBrightness = 0;      // actual PWM value
uint8_t outputTarget = 0;          // requested PWM value
uint8_t fadeStartBrightness = 0;
unsigned long fadeStartMs = 0;
uint32_t activeFadeDurationMs = 0;
bool fadeActive = false;

// Manual output test is only available in STANDBY.
constexpr unsigned long MANUAL_TEST_TIMEOUT_MS = 10000;
bool manualTestActive = false;
unsigned long manualTestStartMs = 0;

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

constexpr float FILTER_ALPHA = 0.98f;
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

float calibrationAccelNoiseRms = 0.0f;
float calibrationGyroNoiseRms = 0.0f;
bool calibrationHighVibration = false;

// =====================================================
// BUTTON
// =====================================================

constexpr unsigned long BUTTON_DEBOUNCE_MS = 35;
constexpr unsigned long BUTTON_LONG_PRESS_MS = 1500;
constexpr unsigned long BUTTON_WIFI_PASSWORD_RESET_MS = 30000;
constexpr unsigned long BUTTON_MULTI_TAP_MS = 400;

bool lastRawButtonState = HIGH;
bool debouncedButtonState = HIGH;
unsigned long lastButtonChangeTime = 0;
unsigned long buttonPressStartTime = 0;
bool longPressHandled = false;
bool longPressGestureActive = false;

// Short taps are collected into one gesture window:
// 1 tap = next OLED page
// 2 taps = ABSOLUTE <-> ADAPTIVE
// 3 taps = Wi-Fi access point ON/OFF
uint8_t shortTapCount = 0;
unsigned long lastTapReleaseTime = 0;

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

RotationAxis rotationAxis = RotationAxis::Y;
RotationAxis rollAxis = RotationAxis::X;
RotationAxis verticalAxis = RotationAxis::Z;
bool orientationConfigured = false;
float gyroAxisBias = 0.0f;
float rollGyroBias = 0.0f;
float pitch = 0.0f;
float pitchZero = 0.0f;
float roll = 0.0f;
float rollZero = 0.0f;
float levelReferenceX = 0.0f;
float levelReferenceY = 0.0f;
float levelReferenceZ = 1.0f;

// Absolute pitch is relative to startup/calibration zero.
float currentAbsolutePitch = 0.0f;

// Adaptive baseline slowly follows terrain in ADAPTIVE mode.
float adaptiveBaseline = 0.0f;

// This is the angle actually sent to the wheelie detector.
float currentTriggerPitch = 0.0f;

float currentGyroRate = 0.0f;
float currentRollAngle = 0.0f;
float currentRollRate = 0.0f;

// Live acceleration telemetry. v0.11 reports +G as dynamic linear
// acceleration with the gravity vector removed, so a stationary bike
// settles at approximately +0.00 g instead of ~1 g.
float currentAccelX = 0.0f;
float currentAccelY = 0.0f;
float currentAccelZ = 1.0f;
float currentGLoad = 0.0f;

// Calibration normalizes the particular MPU's measured resting gravity
// magnitude (for example 1.12 raw g) back to 1.00 g.
float accelScaleCorrection = 1.0f;
float calibrationRestMagnitudeRaw = 1.0f;

// Low-pass gravity estimate in sensor coordinates. Subtracting this from
// instantaneous acceleration produces a useful launch/bump +G estimate.
float gravityEstimateX = 0.0f;
float gravityEstimateY = 0.0f;
float gravityEstimateZ = 1.0f;
constexpr float GRAVITY_TRACK_TIME_SEC = 1.50f;
constexpr float GLOAD_SMOOTHING_TIME_SEC = 0.08f;
constexpr float GLOAD_DEADBAND_G = 0.015f;

// Session peaks (not persisted). They reset at calibration or from the
// dashboard's individual reset buttons.
float highestAngle = 0.0f;
float highestGLoad = 0.0f;

bool adaptiveBaselineFrozen = false;

bool imuHealthy = false;
int imuFailureCount = 0;
int imuRecoveryCount = 0;
constexpr int IMU_FAILURE_LIMIT = 5;
constexpr int IMU_RECOVERY_LIMIT = 20;
constexpr unsigned long ORIENTATION_UPRIGHT_SAMPLE_MS = 1500;
constexpr unsigned long ORIENTATION_MOTION_SAMPLE_MS = 8000;
constexpr uint8_t ORIENTATION_START_DELAY_SECONDS = 4;

unsigned long lastMicros = 0;
unsigned long triggerStartTime = 0;
unsigned long wheelieStartTime = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastSerialUpdate = 0;

ControllerState controllerState = ControllerState::NORMAL;
bool highAngleWarningActive = false;
unsigned long completedWheelieCount = 0;
unsigned long lastWheelieDurationMs = 0;
float lastWheeliePeakAngle = 0.0f;
float lastWheeliePeakG = 0.0f;
float activeWheeliePeakAngle = 0.0f;
float activeWheeliePeakG = 0.0f;
OperatingMode operatingMode = OperatingMode::STANDBY;
DisplayPage displayPage = DisplayPage::STATUS;

bool displayDirty = true;

// =====================================================
// BOUNDED RIDE TELEMETRY
// =====================================================

bool rideStorageReady = false;
bool rideLoggingEnabled = RIDE_LOG_DEFAULT_ENABLED;
bool rideSessionActive = false;
bool rideHashInitialized = false;
File rideFile;
RideLogHeader activeRideHeader;
mbedtls_sha256_context rideHashContext;
uint8_t activeRideSlot = 0;
uint32_t rideSessionStartMs = 0;
uint32_t rideNextSampleMs = 0;
uint32_t rideWheelieStartCount = 0;
uint16_t rideSamplesSinceFlush = 0;

// =====================================================
// FORWARD DECLARATIONS
// =====================================================

void forceOutputOff();
void setOutputTarget(uint8_t target, bool immediate = false);
void setOperatingMode(OperatingMode mode);
bool calibrateMPU();
bool runOrientationWizard();
String makeWriteToken();
void startAccessPoint();
void stopAccessPoint();
void toggleAccessPointFromButton();
void scheduleAccessPointRestart(unsigned long delayMs = 750);
void resetWiFiPasswordToDefault();
void flashCalibrationComplete();
void initializeRideLogging();
void startRideSession();
void finishRideSession(bool capacityReached = false);
void updateRideLogging();

// =====================================================
// STRING HELPERS
// =====================================================

const char* getStateName() {
    switch (controllerState) {
        case ControllerState::NORMAL: return "NORMAL";
        case ControllerState::TRIGGER_PENDING: return "PENDING";
        case ControllerState::WHEELIE: return "WHEELIE";
    }
    return "UNKNOWN";
}

const char* getModeName() {
    return operatingMode == OperatingMode::ARMED ? "ARMED" : "STANDBY";
}

const char* getAngleModeName() {
    return settings.angleMode == AngleMode::ADAPTIVE ? "ADAPTIVE" : "ABSOLUTE";
}

// =====================================================
// SETTINGS / NVS
// =====================================================

void validateSettings() {
    validateControllerSettings(settings);
}

const char* getRotationAxisName() {
    switch (rotationAxis) {
        case RotationAxis::X: return "X";
        case RotationAxis::Y: return "Y";
        case RotationAxis::Z: return "Z";
    }
    return "Y";
}

const char* getAxisName(RotationAxis axis) {
    switch (axis) {
        case RotationAxis::X: return "X";
        case RotationAxis::Y: return "Y";
        case RotationAxis::Z: return "Z";
    }
    return "?";
}

const char* getAxisJsonName(RotationAxis axis) {
    switch (axis) {
        case RotationAxis::X: return "x";
        case RotationAxis::Y: return "y";
        case RotationAxis::Z: return "z";
    }
    return "unknown";
}

void loadSettings() {
    preferences.begin("wheelie", true);

    settings.triggerAngle = preferences.getFloat("trig", 20.0f);
    settings.resetAngle = preferences.getFloat("reset", 10.0f);
    settings.triggerHoldMs = preferences.getUInt("hold", 150);
    settings.minimumOnTimeMs = preferences.getUInt("minon", 1000);
    settings.brightness = preferences.getUChar("bright", 255);
    settings.fadeMs = preferences.getUInt("fade", 200);
    settings.bootArmed = preferences.getBool("bootarm", false);

    settings.angleMode = static_cast<AngleMode>(
        preferences.getUChar("angmode", static_cast<uint8_t>(AngleMode::ABSOLUTE))
    );
    settings.adaptiveTimeConstantSec = preferences.getFloat("adptau", 4.0f);
    settings.adaptiveFreezeRateDegSec = preferences.getFloat("adpfrz", 8.0f);
    settings.warningAngle = preferences.getFloat("warnang", 45.0f);
    settings.warningResetAngle = preferences.getFloat("warnrst", 40.0f);
    settings.warningPitchRateDegSec = preferences.getFloat("warnrate", 45.0f);
    settings.wheeliePattern = static_cast<LightPattern>(preferences.getUChar("whlpat", 1));
    settings.warningPattern = static_cast<LightPattern>(preferences.getUChar("wrnpat", 4));
    settings.warningBrightness = preferences.getUChar("wrnbright", 255);
    rotationAxis = static_cast<RotationAxis>(preferences.getUChar("rotaxis", 1));
    rollAxis = static_cast<RotationAxis>(preferences.getUChar("rollaxis", 0));
    verticalAxis = static_cast<RotationAxis>(preferences.getUChar("vertaxis", 2));
    orientationConfigured = preferences.getBool("orientok", false);
    wifiApPassword = preferences.getString("wifipass", DEFAULT_WIFI_AP_PASSWORD);
    wifiApSsid = preferences.getString("apssid", "");
    otaChannel = preferences.getString("otachannel", RELEASE_CHANNEL);
    rideLoggingEnabled = preferences.getBool("ridelog", RIDE_LOG_DEFAULT_ENABLED);

    preferences.end();

    // Generate the AP identity only once. NVS survives ordinary restarts and
    // OTA firmware updates, so paired devices continue to recognize the AP.
    if (wifiApSsid.length() == 0) {
        char generatedSsid[33];
        snprintf(generatedSsid, sizeof(generatedSsid), "%s%04lu",
                 WIFI_AP_SSID_PREFIX,
                 (unsigned long)(esp_random() % 10000));
        wifiApSsid = generatedSsid;

        preferences.begin("wheelie", false);
        preferences.putString("apssid", wifiApSsid);
        preferences.end();
        Serial.printf("Generated persistent Wi-Fi SSID: %s\n", wifiApSsid.c_str());
    }

    if (wifiApPassword.length() < 8 || wifiApPassword.length() > 63) {
        wifiApPassword = DEFAULT_WIFI_AP_PASSWORD;
    }
    if (otaChannel != "stable" && otaChannel != "testing") {
        otaChannel = RELEASE_CHANNEL;
    }
    if (!isRotationAxisValid(rotationAxis)) {
        rotationAxis = RotationAxis::Y;
    }
    if (!isRotationAxisValid(rollAxis) || !isRotationAxisValid(verticalAxis) ||
        rollAxis == rotationAxis || rollAxis == verticalAxis || rotationAxis == verticalAxis) {
        rollAxis = RotationAxis::X;
        rotationAxis = RotationAxis::Y;
        verticalAxis = RotationAxis::Z;
        orientationConfigured = false;
    }
    validateSettings();
}

void saveSettings() {
    preferences.begin("wheelie", false);

    preferences.putFloat("trig", settings.triggerAngle);
    preferences.putFloat("reset", settings.resetAngle);
    preferences.putUInt("hold", settings.triggerHoldMs);
    preferences.putUInt("minon", settings.minimumOnTimeMs);
    preferences.putUChar("bright", settings.brightness);
    preferences.putUInt("fade", settings.fadeMs);
    preferences.putBool("bootarm", settings.bootArmed);

    preferences.putUChar("angmode", static_cast<uint8_t>(settings.angleMode));
    preferences.putFloat("adptau", settings.adaptiveTimeConstantSec);
    preferences.putFloat("adpfrz", settings.adaptiveFreezeRateDegSec);
    preferences.putFloat("warnang", settings.warningAngle);
    preferences.putFloat("warnrst", settings.warningResetAngle);
    preferences.putFloat("warnrate", settings.warningPitchRateDegSec);
    preferences.putUChar("whlpat", static_cast<uint8_t>(settings.wheeliePattern));
    preferences.putUChar("wrnpat", static_cast<uint8_t>(settings.warningPattern));
    preferences.putUChar("wrnbright", settings.warningBrightness);
    preferences.putUChar("rotaxis", static_cast<uint8_t>(rotationAxis));
    preferences.putUChar("rollaxis", static_cast<uint8_t>(rollAxis));
    preferences.putUChar("vertaxis", static_cast<uint8_t>(verticalAxis));
    preferences.putBool("orientok", orientationConfigured);
    preferences.putString("wifipass", wifiApPassword);
    preferences.putString("otachannel", otaChannel);
    preferences.putBool("ridelog", rideLoggingEnabled);

    preferences.end();
    Serial.println("Settings saved to NVS");
}

// =====================================================
// MPU6050 LOW-LEVEL FUNCTIONS
// =====================================================

void writeMPURegister(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(reg);
    Wire.write(value);
    Wire.endTransmission();
}

bool readMPU(MPUData &data) {
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(ACCEL_XOUT_H);

    if (Wire.endTransmission(false) != 0) {
        return false;
    }

    Wire.requestFrom(MPU_ADDR, (size_t)14, true);

    if (Wire.available() != 14) {
        return false;
    }

    int16_t rawAx = (Wire.read() << 8) | Wire.read();
    int16_t rawAy = (Wire.read() << 8) | Wire.read();
    int16_t rawAz = (Wire.read() << 8) | Wire.read();

    Wire.read();
    Wire.read(); // temperature unused

    int16_t rawGx = (Wire.read() << 8) | Wire.read();
    int16_t rawGy = (Wire.read() << 8) | Wire.read();
    int16_t rawGz = (Wire.read() << 8) | Wire.read();

    data.ax = rawAx / ACCEL_LSB_PER_G;
    data.ay = rawAy / ACCEL_LSB_PER_G;
    data.az = rawAz / ACCEL_LSB_PER_G;

    data.gx = rawGx / 131.0f;
    data.gy = rawGy / 131.0f;
    data.gz = rawGz / 131.0f;

    return true;
}

float getGyroRateForAxis(const MPUData &data, RotationAxis axis) {
    switch (axis) {
        case RotationAxis::X: return data.gx;
        case RotationAxis::Y: return data.gy;
        case RotationAxis::Z: return data.gz;
    }
    return data.gy;
}

float getSelectedGyroRate(const MPUData &data) {
    return getGyroRateForAxis(data, rotationAxis);
}

float calculateAccelAngleRelative(RotationAxis axis, const MPUData &data) {
    return calculateRelativeRotationDegrees(
        axis,
        levelReferenceX, levelReferenceY, levelReferenceZ,
        data.ax, data.ay, data.az);
}

// =====================================================
// PWM OUTPUT / FADES
// =====================================================

void writePWM(uint8_t brightness) {
    outputBrightness = brightness;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(OUTPUT1_PWM_PIN, brightness);
#else
    ledcWrite(PWM_CHANNEL, brightness);
#endif
}

void initializePWM() {
    pinMode(OUTPUT1_PWM_PIN, OUTPUT);
    digitalWrite(OUTPUT1_PWM_PIN, LOW);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    if (!ledcAttach(OUTPUT1_PWM_PIN, PWM_FREQUENCY, PWM_RESOLUTION)) {
        Serial.println("ERROR: PWM initialization failed");
        return;
    }
    ledcWrite(OUTPUT1_PWM_PIN, 0);
#else
    ledcSetup(PWM_CHANNEL, PWM_FREQUENCY, PWM_RESOLUTION);
    ledcAttachPin(OUTPUT1_PWM_PIN, PWM_CHANNEL);
    ledcWrite(PWM_CHANNEL, 0);
#endif

    outputBrightness = 0;
    outputTarget = 0;
    fadeActive = false;
}

void forceOutputOff() {
    manualTestActive = false;
    fadeActive = false;
    outputTarget = 0;
    writePWM(0);
}

void flashCalibrationComplete() {
    // Bypass the configured fade so both confirmation flashes are distinct.
    for (uint8_t flash = 0; flash < 2; ++flash) {
        writePWM(255);
        delay(CALIBRATION_FLASH_MS);
        writePWM(0);
        delay(CALIBRATION_FLASH_MS);
    }
    outputTarget = 0;
    fadeActive = false;
}

void setOutputTarget(uint8_t target, bool immediate) {
    if (target == outputTarget && !immediate) {
        return;
    }

    outputTarget = target;

    if (immediate || settings.fadeMs == 0) {
        fadeActive = false;
        writePWM(target);
        return;
    }

    fadeStartBrightness = outputBrightness;
    fadeStartMs = millis();
    activeFadeDurationMs = settings.fadeMs;
    fadeActive = true;
}

void updateOutputFade() {
    if (!fadeActive) {
        return;
    }

    unsigned long elapsed = millis() - fadeStartMs;

    if (elapsed >= activeFadeDurationMs || activeFadeDurationMs == 0) {
        fadeActive = false;
        writePWM(outputTarget);
        return;
    }

    writePWM(calculateFadeBrightness(
        fadeStartBrightness, outputTarget, elapsed, activeFadeDurationMs));
}

void updateManualOutputTest() {
    if (!manualTestActive) {
        return;
    }

    if (
        operatingMode != OperatingMode::STANDBY ||
        (millis() - manualTestStartMs) >= MANUAL_TEST_TIMEOUT_MS
    ) {
        manualTestActive = false;
        setOutputTarget(0);
        displayDirty = true;
    }
}

// =====================================================
// OLED HELPERS
// =====================================================

void oledPrintRow(uint8_t row, const char* text) {
    char line[17];
    memset(line, ' ', 16);
    line[16] = '\0';

    size_t len = strlen(text);
    if (len > 16) len = 16;
    memcpy(line, text, len);

    oled.drawString(0, row, line);
}

void drawBootScreen() {
    oled.clearDisplay();
    oledPrintRow(0, "WHEELIE CTRL");
    oledPrintRow(2, "BOOTING...");
    oledPrintRow(4, FIRMWARE_VERSION);
}

void drawCalibrationScreen(const char* line5 = "Please wait...") {
    oled.clearDisplay();
    oledPrintRow(0, "IMU CALIBRATION");
    oledPrintRow(2, "Keep bike still");
    oledPrintRow(3, "Idle vibration OK");
    oledPrintRow(5, line5);
}

void drawStandbyScreen() {
    char line[17];

    oledPrintRow(0, "    STANDBY");
    oledPrintRow(1, "----------------");

    if (manualTestActive) {
        int percent = (outputBrightness * 100) / 255;
        snprintf(line, sizeof(line), "TEST OUT:%d%%", percent);
        oledPrintRow(2, line);
    } else {
        oledPrintRow(2, "OUTPUT: OFF");
    }

    snprintf(line, sizeof(line), "PITCH:%+.1f", currentTriggerPitch);
    oledPrintRow(4, line);
    oledPrintRow(6, "Hold 1.5s ARM");
    oledPrintRow(7, "30s WiFi reset");
}

void drawStatusPage() {
    char line[17];

    oledPrintRow(0, "WHEELIE CTRL 1/4");
    oledPrintRow(1, "MODE: ARMED");

    snprintf(line, sizeof(line), "PITCH:%+.1f deg", currentTriggerPitch);
    oledPrintRow(2, line);

    snprintf(line, sizeof(line), "STATE:%s", getStateName());
    oledPrintRow(3, line);

    int outputPercent = (outputBrightness * 100) / 255;
    snprintf(line, sizeof(line), "OUTPUT:%d%%", outputPercent);
    oledPrintRow(4, line);

    snprintf(line, sizeof(line), "ANGLE:%s", settings.angleMode == AngleMode::ADAPTIVE ? "ADAPT" : "ABS");
    oledPrintRow(5, line);

    oledPrintRow(6, "1xPg 2xAng 3xAP");
    oledPrintRow(7, "Hold=Arm 30sPwd");
}

void drawSettingsPage() {
    char line[17];

    oledPrintRow(0, "SETTINGS     2/4");
    oledPrintRow(1, "----------------");

    snprintf(line, sizeof(line), "TRIG:%.1f deg", settings.triggerAngle);
    oledPrintRow(2, line);

    snprintf(line, sizeof(line), "RESET:%.1f deg", settings.resetAngle);
    oledPrintRow(3, line);

    snprintf(line, sizeof(line), "MODE:%s", settings.angleMode == AngleMode::ADAPTIVE ? "ADAPT" : "ABS");
    oledPrintRow(4, line);

    if (settings.angleMode == AngleMode::ADAPTIVE) {
        snprintf(line, sizeof(line), "TAU:%.1fs", settings.adaptiveTimeConstantSec);
        oledPrintRow(5, line);

        snprintf(line, sizeof(line), "FREEZE:%.1f/s", settings.adaptiveFreezeRateDegSec);
        oledPrintRow(6, line);
    } else {
        snprintf(line, sizeof(line), "HOLD:%lums", (unsigned long)settings.triggerHoldMs);
        oledPrintRow(5, line);

        snprintf(line, sizeof(line), "MIN:%lums", (unsigned long)settings.minimumOnTimeMs);
        oledPrintRow(6, line);
    }

    oledPrintRow(7, "2x btn = mode");
}

void drawNetworkPage() {
    char line[17];

    oledPrintRow(0, "NETWORK      3/4");
    oledPrintRow(1, "----------------");

    if (!accessPointEnabled) {
        oledPrintRow(2, "AP: OFF");
        oledPrintRow(3, "WiFi disabled");
        oledPrintRow(4, "");
        oledPrintRow(5, "3x button");
        oledPrintRow(6, "turns AP ON");
        oledPrintRow(7, "");
        return;
    }

    String ssidSuffix = wifiApSsid.substring(wifiApSsid.length() - 4);
    snprintf(line, sizeof(line), "SSID: ...%s", ssidSuffix.c_str());
    oledPrintRow(2, line);
    oledPrintRow(3, "wheelie.local");
    oledPrintRow(4, "192.168.4.1");

    snprintf(line, sizeof(line), "CLIENTS:%u", WiFi.softAPgetStationNum());
    oledPrintRow(5, line);

    snprintf(line, sizeof(line), "mDNS:%s DNS:%s", mdnsHealthy ? "OK" : "--", dnsHealthy ? "OK" : "--");
    oledPrintRow(6, line);
    oledPrintRow(7, "3xAP 30sPwdRst");
}

void drawDiagnosticsPage() {
    char line[17];

    oledPrintRow(0, "DIAGNOSTICS  4/4");

    snprintf(line, sizeof(line), "RAW:%+.1f", currentAbsolutePitch);
    oledPrintRow(1, line);

    snprintf(line, sizeof(line), "BASE:%+.1f", adaptiveBaseline);
    oledPrintRow(2, line);

    snprintf(line, sizeof(line), "TRIG:%+.1f", currentTriggerPitch);
    oledPrintRow(3, line);

    snprintf(line, sizeof(line), "GYRO:%+.1f/s", currentGyroRate);
    oledPrintRow(4, line);

    snprintf(line, sizeof(line), "+G:%.2fg", currentGLoad);
    oledPrintRow(5, line);

    snprintf(line, sizeof(line), "BASE:%s", adaptiveBaselineFrozen ? "FROZEN" : "TRACKING");
    oledPrintRow(6, line);

    snprintf(line, sizeof(line), "IMU:%s", imuHealthy ? "OK" : "FAULT");
    oledPrintRow(7, line);
}

void drawWiFiPasswordResetScreen() {
    oledPrintRow(0, " WIFI PASSWORD");
    oledPrintRow(1, "----------------");
    oledPrintRow(2, "RESET TO DEFAULT");
    oledPrintRow(4, "wheeliectrl");
    oledPrintRow(6, "AP is ON");
    oledPrintRow(7, "Reconnect device");
}

void updateDisplay() {
    unsigned long now = millis();

    if (!displayDirty && (now - lastDisplayUpdate) < DISPLAY_REFRESH_MS) {
        return;
    }

    lastDisplayUpdate = now;
    displayDirty = false;

    if ((long)(wifiPasswordResetNoticeUntil - now) > 0) {
        drawWiFiPasswordResetScreen();
        return;
    }

    if (operatingMode == OperatingMode::STANDBY) {
        drawStandbyScreen();
        return;
    }

    switch (displayPage) {
        case DisplayPage::STATUS: drawStatusPage(); break;
        case DisplayPage::SETTINGS: drawSettingsPage(); break;
        case DisplayPage::NETWORK: drawNetworkPage(); break;
        case DisplayPage::DIAGNOSTICS: drawDiagnosticsPage(); break;
    }
}

// =====================================================
// VIBRATION-TOLERANT CALIBRATION
// =====================================================

bool runOrientationWizard() {
    Serial.println();
    Serial.println("======================================");
    Serial.println("MOUNTING / ORIENTATION WIZARD");
    Serial.println("Hold upright, then lean side to side.");
    Serial.println("======================================");

    forceOutputOff();
    manualTestActive = false;

    for (uint8_t seconds = ORIENTATION_START_DELAY_SECONDS; seconds > 0; --seconds) {
        char line[17];
        oled.clearDisplay();
        oledPrintRow(0, "MOUNTING SETUP");
        oledPrintRow(2, "Hold bike upright");
        oledPrintRow(4, "Do not lift it");
        snprintf(line, sizeof(line), "Reading in %u...", seconds);
        oledPrintRow(6, line);
        delay(1000);
    }

    double sumAx = 0.0;
    double sumAy = 0.0;
    double sumAz = 0.0;
    double sumGx = 0.0;
    double sumGy = 0.0;
    double sumGz = 0.0;
    unsigned long uprightSamples = 0;
    unsigned long started = millis();
    MPUData data;

    oled.clearDisplay();
    oledPrintRow(0, "MOUNTING SETUP");
    oledPrintRow(2, "Reading upright");
    oledPrintRow(4, "Hold steady...");

    while ((millis() - started) < ORIENTATION_UPRIGHT_SAMPLE_MS) {
        if (readMPU(data)) {
            const float magnitude = sqrtf(
                data.ax * data.ax + data.ay * data.ay + data.az * data.az);
            if (magnitude >= CAL_ACCEL_MAG_MIN_G && magnitude <= CAL_ACCEL_MAG_MAX_G) {
                sumAx += data.ax;
                sumAy += data.ay;
                sumAz += data.az;
                sumGx += data.gx;
                sumGy += data.gy;
                sumGz += data.gz;
                uprightSamples++;
            }
        }
        delay(2);
    }

    if (uprightSamples < 100) {
        Serial.println("Orientation wizard failed: unable to read a stable upright pose");
        oled.clearDisplay();
        oledPrintRow(0, "SETUP FAILED");
        oledPrintRow(2, "IMU not stable");
        oledPrintRow(4, "Try in Settings");
        delay(1800);
        displayDirty = true;
        return false;
    }

    const float avgAx = (float)(sumAx / uprightSamples);
    const float avgAy = (float)(sumAy / uprightSamples);
    const float avgAz = (float)(sumAz / uprightSamples);
    const float biasGx = (float)(sumGx / uprightSamples);
    const float biasGy = (float)(sumGy / uprightSamples);
    const float biasGz = (float)(sumGz / uprightSamples);

    double motionX = 0.0;
    double motionY = 0.0;
    double motionZ = 0.0;
    unsigned long motionSamples = 0;
    started = millis();
    unsigned long lastShownSecond = UINT32_MAX;

    oled.clearDisplay();
    oledPrintRow(0, "LEAN SIDE-SIDE");
    oledPrintRow(2, "Smoothly several");
    oledPrintRow(3, "times while parked");
    oledPrintRow(5, "Keep wheels down");

    while ((millis() - started) < ORIENTATION_MOTION_SAMPLE_MS) {
        if (readMPU(data)) {
            motionX += fabsf(data.gx - biasGx);
            motionY += fabsf(data.gy - biasGy);
            motionZ += fabsf(data.gz - biasGz);
            motionSamples++;
        }

        const unsigned long elapsedMs = millis() - started;
        const unsigned long remainingMs = elapsedMs < ORIENTATION_MOTION_SAMPLE_MS
            ? ORIENTATION_MOTION_SAMPLE_MS - elapsedMs : 0;
        const unsigned long remainingSecond = (remainingMs + 999UL) / 1000UL;
        if (remainingSecond != lastShownSecond) {
            char line[17];
            snprintf(line, sizeof(line), "%lu sec remaining", remainingSecond);
            oledPrintRow(7, line);
            lastShownSecond = remainingSecond;
        }
        delay(2);
    }

    if (motionSamples == 0) {
        Serial.println("Orientation wizard failed: no motion samples");
        oled.clearDisplay();
        oledPrintRow(0, "SETUP FAILED");
        oledPrintRow(2, "IMU read fault");
        oledPrintRow(4, "Try in Settings");
        delay(1800);
        displayDirty = true;
        return false;
    }

    const OrientationResult detected = detectOrientationAxes(
        avgAx, avgAy, avgAz,
        (float)(motionX / motionSamples),
        (float)(motionY / motionSamples),
        (float)(motionZ / motionSamples));

    if (!detected.valid) {
        Serial.printf(
            "Orientation wizard failed: motion=%.2f dps confidence=%.2f\n",
            detected.rollMotionDegSec, detected.confidence);
        oled.clearDisplay();
        oledPrintRow(0, "SETUP FAILED");
        oledPrintRow(2, "Lean more clearly");
        oledPrintRow(4, "Try in Settings");
        delay(2200);
        displayDirty = true;
        return false;
    }

    verticalAxis = detected.verticalAxis;
    rollAxis = detected.rollAxis;
    rotationAxis = detected.pitchAxis;
    orientationConfigured = true;
    saveSettings();
    writeToken = makeWriteToken();

    Serial.printf(
        "Orientation saved: vertical=%s roll=%s pitch=%s motion=%.2f confidence=%.2f\n",
        getAxisName(verticalAxis), getAxisName(rollAxis), getAxisName(rotationAxis),
        detected.rollMotionDegSec, detected.confidence);

    char axes[17];
    snprintf(axes, sizeof(axes), "R:%s  P:%s  V:%s",
             getAxisName(rollAxis), getAxisName(rotationAxis), getAxisName(verticalAxis));
    oled.clearDisplay();
    oledPrintRow(0, "SETUP SAVED");
    oledPrintRow(2, axes);
    oledPrintRow(4, "Return upright");
    oledPrintRow(6, "Calibration next");
    delay(2500);
    displayDirty = true;
    return true;
}

bool calibrateMPU() {
    Serial.println();
    Serial.println("======================================");
    Serial.println("IMU CALIBRATION");
    Serial.println("Bike must remain stationary.");
    Serial.println("Normal engine-idle vibration is OK.");
    Serial.println("======================================");

    forceOutputOff();
    drawCalibrationScreen();

    unsigned long started = millis();
    int accepted = 0;
    int rejected = 0;

    double sumAx = 0.0;
    double sumAy = 0.0;
    double sumAz = 0.0;
    double sumGy = 0.0;
    double sumRollGy = 0.0;

    double sumAccelMag = 0.0;
    double sumAccelSq = 0.0;
    double sumGySq = 0.0;

    MPUData data;

    while (
        accepted < CALIBRATION_ACCEPTED_SAMPLES &&
        (millis() - started) < CALIBRATION_TIMEOUT_MS
    ) {
        if (!readMPU(data)) {
            rejected++;
            delay(2);
            continue;
        }

        float accelMag = sqrtf(
            data.ax * data.ax +
            data.ay * data.ay +
            data.az * data.az
        );
        float selectedGyroRate = getSelectedGyroRate(data);
        float selectedRollRate = getGyroRateForAxis(data, rollAxis);

        bool usable =
            accelMag >= CAL_ACCEL_MAG_MIN_G &&
            accelMag <= CAL_ACCEL_MAG_MAX_G &&
            fabsf(selectedGyroRate) <= CAL_MAX_GYRO_AXIS_DPS &&
            fabsf(selectedRollRate) <= CAL_MAX_GYRO_AXIS_DPS;

        if (!usable) {
            rejected++;
            delay(2);
            continue;
        }

        sumAx += data.ax;
        sumAy += data.ay;
        sumAz += data.az;
        sumGy += selectedGyroRate;
        sumRollGy += selectedRollRate;

        // Track magnitude statistics separately from scale error. A clone
        // that reads 1.12 g at rest should not be mistaken for vibration.
        sumAccelMag += accelMag;
        sumAccelSq += accelMag * accelMag;
        sumGySq += selectedGyroRate * selectedGyroRate;

        accepted++;

        if ((accepted % 250) == 0) {
            char line[17];
            snprintf(line, sizeof(line), "%d / %d", accepted, CALIBRATION_ACCEPTED_SAMPLES);
            oledPrintRow(6, line);
        }

        delay(2);
    }

    if (accepted < CALIBRATION_ACCEPTED_SAMPLES) {
        Serial.printf("Calibration FAILED: accepted=%d rejected=%d\n", accepted, rejected);
        imuHealthy = false;
        drawCalibrationScreen("CAL FAILED");
        delay(1200);
        displayDirty = true;
        return false;
    }

    float avgAx = (float)(sumAx / accepted);
    float avgAy = (float)(sumAy / accepted);
    float avgAz = (float)(sumAz / accepted);

    gyroAxisBias = (float)(sumGy / accepted);
    rollGyroBias = (float)(sumRollGy / accepted);
    levelReferenceX = avgAx;
    levelReferenceY = avgAy;
    levelReferenceZ = avgAz;
    pitchZero = 0.0f;
    pitch = 0.0f;
    rollZero = 0.0f;
    roll = 0.0f;

    calibrationRestMagnitudeRaw = (float)(sumAccelMag / accepted);
    if (calibrationRestMagnitudeRaw > 0.5f && calibrationRestMagnitudeRaw < 1.5f) {
        accelScaleCorrection = 1.0f / calibrationRestMagnitudeRaw;
    } else {
        accelScaleCorrection = 1.0f;
    }

    float accelMeanSquare = (float)(sumAccelSq / accepted);
    float accelVariance = accelMeanSquare -
        (calibrationRestMagnitudeRaw * calibrationRestMagnitudeRaw);
    if (accelVariance < 0.0f) accelVariance = 0.0f;
    calibrationAccelNoiseRms = sqrtf(accelVariance);

    // Initialize the gravity estimator from the calibrated resting vector,
    // normalized so stationary acceleration is exactly one gravity vector.
    gravityEstimateX = avgAx * accelScaleCorrection;
    gravityEstimateY = avgAy * accelScaleCorrection;
    gravityEstimateZ = avgAz * accelScaleCorrection;
    currentAccelX = gravityEstimateX;
    currentAccelY = gravityEstimateY;
    currentAccelZ = gravityEstimateZ;
    currentGLoad = 0.0f;
    highestAngle = 0.0f;
    highestGLoad = 0.0f;

    // Gyro vibration/noise is the RMS variation around the measured bias,
    // not the bias itself.
    float gyroMeanSquare = (float)(sumGySq / accepted);
    float gyroVariance = gyroMeanSquare - (gyroAxisBias * gyroAxisBias);
    if (gyroVariance < 0.0f) gyroVariance = 0.0f;
    calibrationGyroNoiseRms = sqrtf(gyroVariance);

    calibrationHighVibration =
        calibrationAccelNoiseRms > 0.08f ||
        calibrationGyroNoiseRms > 4.0f;

    currentAbsolutePitch = 0.0f;
    adaptiveBaseline = 0.0f;
    currentTriggerPitch = 0.0f;
    currentGyroRate = 0.0f;
    currentRollAngle = 0.0f;
    currentRollRate = 0.0f;
    adaptiveBaselineFrozen = false;

    imuHealthy = true;
    imuFailureCount = 0;
    imuRecoveryCount = IMU_RECOVERY_LIMIT;

    Serial.printf("Calibration complete. Accepted=%d Rejected=%d\n", accepted, rejected);
    Serial.printf("Gyro %s bias: %.3f deg/s\n", getRotationAxisName(), gyroAxisBias);
    Serial.printf("Roll gyro %s bias: %.3f deg/s\n", getAxisName(rollAxis), rollGyroBias);
    Serial.printf("Physical zero: %.2f deg\n", pitchZero);
    Serial.printf("Resting accel magnitude: %.4f raw g (scale x%.4f)\n", calibrationRestMagnitudeRaw, accelScaleCorrection);
    Serial.printf("Accel vibration RMS: %.4f g\n", calibrationAccelNoiseRms);
    Serial.printf("Gyro vibration RMS: %.3f deg/s\n", calibrationGyroNoiseRms);

    if (calibrationHighVibration) {
        Serial.println("WARNING: calibration vibration level was high");
    }

    flashCalibrationComplete();
    oled.clearDisplay();
    displayDirty = true;
    return true;
}

// =====================================================
// ANGLE PROCESSING - v0.9
// =====================================================

void resetAdaptiveBaselineToCurrent() {
    if (settings.angleMode == AngleMode::ADAPTIVE) {
        adaptiveBaseline = currentAbsolutePitch;
        currentTriggerPitch = 0.0f;
    } else {
        adaptiveBaseline = 0.0f;
        currentTriggerPitch = currentAbsolutePitch;
    }

    adaptiveBaselineFrozen = false;
}

void updateAngleProcessing(float dt) {
    // Adaptive mode follows terrain only during ordinary, stable riding.
    // The portable helper is also exercised by the desktop test suite.
    const bool controllerAllowsTracking =
        operatingMode == OperatingMode::ARMED &&
        controllerState == ControllerState::NORMAL;
    currentTriggerPitch = processTriggerPitch(
        currentAbsolutePitch, currentGyroRate, dt, imuHealthy,
        controllerAllowsTracking, settings, adaptiveBaseline,
        adaptiveBaselineFrozen);
    if (currentTriggerPitch > highestAngle) highestAngle = currentTriggerPitch;
}

// =====================================================
// RIDE TELEMETRY STORAGE
// =====================================================

String ridePathForSlot(uint8_t slot) {
    return "/ride" + String(slot) + ".wlr";
}

int16_t rideCentiValue(float value) {
    value = max(-327.68f, min(327.67f, value));
    return static_cast<int16_t>(lroundf(value * 100.0f));
}

int16_t rideMilliGValue(float value) {
    value = max(-32.768f, min(32.767f, value));
    return static_cast<int16_t>(lroundf(value * 1000.0f));
}

void copyRideText(char* destination, size_t size, const char* value) {
    if (size == 0) return;
    strncpy(destination, value ? value : "", size - 1);
    destination[size - 1] = '\0';
}

bool readRideHeader(uint8_t slot, RideLogHeader& header) {
    if (!rideStorageReady || slot >= RIDE_LOG_MAX_SESSIONS) return false;
    File file = LittleFS.open(ridePathForSlot(slot), "r");
    if (!file) return false;
    const bool readOk = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) == sizeof(header);
    file.close();
    return readOk && isRideLogHeaderValid(header);
}

void writeActiveRideHeader() {
    if (!rideSessionActive || !rideFile) return;
    activeRideHeader.durationMs = millis() - rideSessionStartMs;
    activeRideHeader.wheelieCount = completedWheelieCount - rideWheelieStartCount;
    const size_t endPosition = rideFile.size();
    if (rideFile.seek(0)) {
        rideFile.write(reinterpret_cast<const uint8_t*>(&activeRideHeader), sizeof(activeRideHeader));
        rideFile.flush();
        rideFile.seek(endPosition);
    }
}

void recoverRideFile(uint8_t slot) {
    const String path = ridePathForSlot(slot);
    File file = LittleFS.open(path, "r+");
    if (!file) return;

    RideLogHeader header;
    if (file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header) ||
        !isRideLogHeaderValid(header)) {
        file.close();
        Serial.printf("Ignoring invalid ride log %s\n", path.c_str());
        return;
    }

    const size_t availableSamples = file.size() > sizeof(header) ?
        (file.size() - sizeof(header)) / sizeof(RideTelemetrySample) : 0;
    if ((header.flags & RIDE_HEADER_COMPLETE) && header.sampleCount <= availableSamples) {
        file.close();
        return;
    }

    header.sampleCount = 0;
    header.durationMs = 0;
    header.wheelieCount = 0;
    header.peakPitchCentiDeg = 0;
    header.peakRollCentiDeg = 0;
    header.peakGyroCentiDegSec = 0;
    header.peakGMillig = 0;

    mbedtls_sha256_context recoveryHash;
    mbedtls_sha256_init(&recoveryHash);
    mbedtls_sha256_starts(&recoveryHash, 0);
    file.seek(sizeof(header));
    RideTelemetrySample sample;
    bool priorWheelie = false;
    while (header.sampleCount < availableSamples &&
           file.read(reinterpret_cast<uint8_t*>(&sample), sizeof(sample)) == sizeof(sample)) {
        mbedtls_sha256_update(&recoveryHash,
            reinterpret_cast<const uint8_t*>(&sample), sizeof(sample));
        header.sampleCount++;
        header.durationMs = sample.elapsedMs;
        header.peakPitchCentiDeg = max(header.peakPitchCentiDeg, sample.pitchCentiDeg);
        const int16_t absoluteRoll = static_cast<int16_t>(abs(static_cast<int>(sample.rollCentiDeg)));
        const int16_t absoluteGyro = static_cast<int16_t>(abs(static_cast<int>(sample.gyroCentiDegSec)));
        const uint16_t positiveG = static_cast<uint16_t>(max(0, static_cast<int>(sample.gLoadMillig)));
        header.peakRollCentiDeg = max(header.peakRollCentiDeg, absoluteRoll);
        header.peakGyroCentiDegSec = max(header.peakGyroCentiDegSec, absoluteGyro);
        header.peakGMillig = max(header.peakGMillig, positiveG);
        const bool wheelie = sample.flags & RIDE_SAMPLE_WHEELIE;
        if (wheelie && !priorWheelie) header.wheelieCount++;
        priorWheelie = wheelie;
    }
    mbedtls_sha256_finish(&recoveryHash, header.sampleSha256);
    mbedtls_sha256_free(&recoveryHash);
    header.flags |= RIDE_HEADER_COMPLETE | RIDE_HEADER_RECOVERED;

    file.seek(0);
    file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
    file.flush();
    file.close();
    Serial.printf("Recovered ride session %lu with %lu samples\n",
                  (unsigned long)header.sessionId, (unsigned long)header.sampleCount);
}

void initializeRideLogging() {
    rideStorageReady = LittleFS.begin(true);
    if (!rideStorageReady) {
        Serial.println("Ride telemetry storage unavailable");
        return;
    }
    for (uint8_t slot = 0; slot < RIDE_LOG_MAX_SESSIONS; ++slot) {
        recoverRideFile(slot);
    }
    Serial.printf("Ride telemetry ready: %lu / %lu bytes used, %u sessions max\n",
                  (unsigned long)LittleFS.usedBytes(), (unsigned long)LittleFS.totalBytes(),
                  RIDE_LOG_MAX_SESSIONS);
}

void startRideSession() {
    if (!rideLoggingEnabled || !rideStorageReady || rideSessionActive) return;

    preferences.begin("wheelie", false);
    uint32_t sessionId = preferences.getUInt("rideid", 0) + 1;
    if (sessionId == 0) sessionId = 1;
    preferences.putUInt("rideid", sessionId);
    preferences.end();

    activeRideSlot = static_cast<uint8_t>((sessionId - 1) % RIDE_LOG_MAX_SESSIONS);
    const String path = ridePathForSlot(activeRideSlot);
    if (LittleFS.exists(path)) LittleFS.remove(path);
    rideFile = LittleFS.open(path, "w+");
    if (!rideFile) {
        Serial.println("Unable to create ride telemetry session");
        return;
    }

    activeRideHeader = RideLogHeader{};
    activeRideHeader.headerSize = sizeof(RideLogHeader);
    activeRideHeader.sampleSize = sizeof(RideTelemetrySample);
    activeRideHeader.sessionId = sessionId;
    activeRideHeader.startUptimeMs = millis();
    activeRideHeader.rotationAxis = static_cast<uint8_t>(rotationAxis);
    activeRideHeader.rollAxis = static_cast<uint8_t>(rollAxis);
    activeRideHeader.verticalAxis = static_cast<uint8_t>(verticalAxis);
    copyRideText(activeRideHeader.firmware, sizeof(activeRideHeader.firmware), FIRMWARE_VERSION);
    copyRideText(activeRideHeader.commit, sizeof(activeRideHeader.commit), BUILD_COMMIT);
    copyRideText(activeRideHeader.buildDate, sizeof(activeRideHeader.buildDate), BUILD_DATE);
    copyRideText(activeRideHeader.channel, sizeof(activeRideHeader.channel), RELEASE_CHANNEL);
    copyRideText(activeRideHeader.board, sizeof(activeRideHeader.board), TARGET_BOARD_ID);
    if (rideFile.write(reinterpret_cast<const uint8_t*>(&activeRideHeader),
                       sizeof(activeRideHeader)) != sizeof(activeRideHeader)) {
        rideFile.close();
        LittleFS.remove(path);
        Serial.println("Unable to write ride telemetry header");
        return;
    }
    rideFile.flush();

    mbedtls_sha256_init(&rideHashContext);
    if (mbedtls_sha256_starts(&rideHashContext, 0) != 0) {
        mbedtls_sha256_free(&rideHashContext);
        rideFile.close();
        LittleFS.remove(path);
        Serial.println("Unable to initialize ride checksum");
        return;
    }
    rideHashInitialized = true;
    rideSessionActive = true;
    rideSessionStartMs = millis();
    rideNextSampleMs = rideSessionStartMs;
    rideWheelieStartCount = completedWheelieCount;
    rideSamplesSinceFlush = 0;
    Serial.printf("Ride session %lu started in slot %u\n",
                  (unsigned long)sessionId, activeRideSlot);
}

void finishRideSession(bool capacityReached) {
    if (!rideSessionActive) return;
    activeRideHeader.durationMs = millis() - rideSessionStartMs;
    activeRideHeader.wheelieCount = completedWheelieCount - rideWheelieStartCount;
    activeRideHeader.flags |= RIDE_HEADER_COMPLETE;
    if (capacityReached) activeRideHeader.flags |= RIDE_HEADER_CAPACITY_REACHED;
    if (rideHashInitialized) {
        mbedtls_sha256_finish(&rideHashContext, activeRideHeader.sampleSha256);
        mbedtls_sha256_free(&rideHashContext);
        rideHashInitialized = false;
    }
    writeActiveRideHeader();
    rideFile.close();
    rideSessionActive = false;
    Serial.printf("Ride session %lu saved: %lu samples, %lu ms%s\n",
                  (unsigned long)activeRideHeader.sessionId,
                  (unsigned long)activeRideHeader.sampleCount,
                  (unsigned long)activeRideHeader.durationMs,
                  capacityReached ? " (capacity reached)" : "");
}

void updateRideLogging() {
    if (!rideSessionActive || !rideFile) return;
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - rideNextSampleMs) < 0) return;
    rideNextSampleMs += RIDE_LOG_SAMPLE_INTERVAL_MS;
    if (static_cast<int32_t>(now - rideNextSampleMs) >
        static_cast<int32_t>(RIDE_LOG_SAMPLE_INTERVAL_MS * 2)) {
        rideNextSampleMs = now + RIDE_LOG_SAMPLE_INTERVAL_MS;
    }
    if (activeRideHeader.sampleCount >= RIDE_LOG_MAX_SAMPLES) {
        finishRideSession(true);
        return;
    }

    RideTelemetrySample sample;
    sample.elapsedMs = now - rideSessionStartMs;
    sample.pitchCentiDeg = rideCentiValue(currentTriggerPitch);
    sample.rawPitchCentiDeg = rideCentiValue(currentAbsolutePitch);
    sample.rollCentiDeg = rideCentiValue(currentRollAngle);
    sample.gyroCentiDegSec = rideCentiValue(currentGyroRate);
    sample.gLoadMillig = rideMilliGValue(currentGLoad);
    sample.outputPercent = static_cast<uint8_t>((outputBrightness * 100U) / 255U);
    if (imuHealthy) sample.flags |= RIDE_SAMPLE_IMU_OK;
    if (controllerState == ControllerState::TRIGGER_PENDING) sample.flags |= RIDE_SAMPLE_PENDING;
    if (controllerState == ControllerState::WHEELIE) sample.flags |= RIDE_SAMPLE_WHEELIE;
    if (highAngleWarningActive) sample.flags |= RIDE_SAMPLE_WARNING;
    if (adaptiveBaselineFrozen) sample.flags |= RIDE_SAMPLE_BASELINE_FROZEN;

    if (rideFile.write(reinterpret_cast<const uint8_t*>(&sample), sizeof(sample)) != sizeof(sample)) {
        finishRideSession(true);
        return;
    }
    mbedtls_sha256_update(&rideHashContext,
        reinterpret_cast<const uint8_t*>(&sample), sizeof(sample));
    activeRideHeader.sampleCount++;
    activeRideHeader.durationMs = sample.elapsedMs;
    activeRideHeader.peakPitchCentiDeg = max(activeRideHeader.peakPitchCentiDeg, sample.pitchCentiDeg);
    const int16_t absoluteRoll = static_cast<int16_t>(abs(static_cast<int>(sample.rollCentiDeg)));
    const int16_t absoluteGyro = static_cast<int16_t>(abs(static_cast<int>(sample.gyroCentiDegSec)));
    const uint16_t positiveG = static_cast<uint16_t>(max(0, static_cast<int>(sample.gLoadMillig)));
    activeRideHeader.peakRollCentiDeg = max(activeRideHeader.peakRollCentiDeg, absoluteRoll);
    activeRideHeader.peakGyroCentiDegSec = max(activeRideHeader.peakGyroCentiDegSec, absoluteGyro);
    activeRideHeader.peakGMillig = max(activeRideHeader.peakGMillig, positiveG);

    if (++rideSamplesSinceFlush >= RIDE_LOG_SAMPLE_RATE_HZ * 5) {
        rideSamplesSinceFlush = 0;
        writeActiveRideHeader();
    }
}

// =====================================================
// OPERATING MODE
// =====================================================

void setOperatingMode(OperatingMode mode) {
    const OperatingMode previousMode = operatingMode;
    if (previousMode == OperatingMode::ARMED && mode != OperatingMode::ARMED) {
        finishRideSession();
    }
    operatingMode = mode;
    controllerState = ControllerState::NORMAL;
    triggerStartTime = 0;
    wheelieStartTime = 0;
    manualTestActive = false;
    forceOutputOff();

    if (mode == OperatingMode::ARMED) {
        // In adaptive mode, arming establishes the current road attitude
        // as the initial adaptive reference.
        resetAdaptiveBaselineToCurrent();
        if (previousMode != OperatingMode::ARMED) startRideSession();
    }

    oled.clearDisplay();
    displayDirty = true;
}

void toggleOperatingMode() {
    if (operatingMode == OperatingMode::ARMED) {
        setOperatingMode(OperatingMode::STANDBY);
        Serial.println("MODE -> STANDBY");
    } else {
        setOperatingMode(OperatingMode::ARMED);
        Serial.println("MODE -> ARMED");
    }
}

// =====================================================
// BUTTON
// =====================================================

void cycleDisplayPage() {
    if (operatingMode != OperatingMode::ARMED) {
        return;
    }

    switch (displayPage) {
        case DisplayPage::STATUS: displayPage = DisplayPage::SETTINGS; break;
        case DisplayPage::SETTINGS: displayPage = DisplayPage::NETWORK; break;
        case DisplayPage::NETWORK: displayPage = DisplayPage::DIAGNOSTICS; break;
        case DisplayPage::DIAGNOSTICS: displayPage = DisplayPage::STATUS; break;
    }

    oled.clearDisplay();
    displayDirty = true;
}

void toggleAngleModeFromButton() {
    // Stop any active/pending wheelie output before changing the
    // interpretation of pitch. This prevents a mode switch from
    // accidentally carrying an old trigger state into the new mode.
    controllerState = ControllerState::NORMAL;
    triggerStartTime = 0;
    wheelieStartTime = 0;
    forceOutputOff();

    if (settings.angleMode == AngleMode::ABSOLUTE) {
        settings.angleMode = AngleMode::ADAPTIVE;

        // Make the current road/bike attitude the starting adaptive
        // reference so switching modes does not create a pitch jump.
        adaptiveBaseline = currentAbsolutePitch;
        currentTriggerPitch = 0.0f;
        adaptiveBaselineFrozen = false;
    } else {
        settings.angleMode = AngleMode::ABSOLUTE;
        adaptiveBaseline = 0.0f;
        currentTriggerPitch = currentAbsolutePitch;
        adaptiveBaselineFrozen = false;
    }

    // Button-selected angle mode is persistent, just like a web change.
    saveSettings();
    writeToken = makeWriteToken();

    Serial.print("ANGLE MODE -> ");
    Serial.println(getAngleModeName());

    oled.clearDisplay();
    displayDirty = true;
}

void registerShortTap(unsigned long now) {
    // Add this release to the current multi-tap gesture. A triple tap
    // can be executed immediately because there is no four-tap action.
    shortTapCount++;
    lastTapReleaseTime = now;

    if (shortTapCount >= 3) {
        shortTapCount = 0;
        toggleAccessPointFromButton();
    }
}

void resolveShortTapGesture() {
    if (shortTapCount == 0) {
        return;
    }

    if (debouncedButtonState == LOW) {
        return;
    }

    if ((millis() - lastTapReleaseTime) <= BUTTON_MULTI_TAP_MS) {
        return;
    }

    uint8_t taps = shortTapCount;
    shortTapCount = 0;

    if (taps == 1) {
        cycleDisplayPage();
    } else if (taps == 2) {
        toggleAngleModeFromButton();
    }
}

void resetWiFiPasswordToDefault() {
    wifiApPassword = DEFAULT_WIFI_AP_PASSWORD;
    saveSettings();
    writeToken = makeWriteToken();

    // A 30-second physical hold is the recovery path, so make sure the AP
    // is actually available with the default credential afterward.
    if (accessPointEnabled) {
        scheduleAccessPointRestart(250);
    } else {
        startAccessPoint();
    }

    wifiPasswordResetNoticeUntil = millis() + 4000;
    oled.clearDisplay();
    displayDirty = true;

    Serial.println("Wi-Fi password reset to default: wheeliectrl");
}

void updateButton() {
    unsigned long now = millis();
    bool rawState = digitalRead(USER_BUTTON_PIN);

    if (rawState != lastRawButtonState) {
        lastButtonChangeTime = now;
        lastRawButtonState = rawState;
    }

    if ((now - lastButtonChangeTime) >= BUTTON_DEBOUNCE_MS) {
        if (rawState != debouncedButtonState) {
            debouncedButtonState = rawState;

            if (debouncedButtonState == LOW) {
                buttonPressStartTime = now;
                longPressHandled = false;
                longPressGestureActive = false;
            } else {
                unsigned long heldMs = now - buttonPressStartTime;

                if (!longPressHandled) {
                    if (heldMs >= BUTTON_LONG_PRESS_MS) {
                        // ARM/STANDBY occurs on release. This lets a 30 s
                        // password-reset hold coexist with the normal hold.
                        shortTapCount = 0;
                        toggleOperatingMode();
                    } else {
                        registerShortTap(now);
                    }
                }

                longPressGestureActive = false;
            }
        }
    }

    if (debouncedButtonState == LOW && !longPressHandled) {
        unsigned long heldMs = now - buttonPressStartTime;

        if (heldMs >= BUTTON_LONG_PRESS_MS) {
            // Once it is clearly a hold, discard any partial tap gesture.
            shortTapCount = 0;
            longPressGestureActive = true;
        }

        if (heldMs >= BUTTON_WIFI_PASSWORD_RESET_MS) {
            shortTapCount = 0;
            resetWiFiPasswordToDefault();
            longPressHandled = true;
            longPressGestureActive = false;
        }
    }

    resolveShortTapGesture();
}

// =====================================================
// WHEELIE STATE MACHINE
// =====================================================

uint8_t patternBrightness(LightPattern pattern, uint8_t brightness, unsigned long now) {
    return calculatePatternBrightness(pattern, brightness, now);
}

void updateHighAngleWarning(float triggerPitch) {
    highAngleWarningActive = stepHighAngleWarning(
        highAngleWarningActive, triggerPitch, currentGyroRate, settings);
}

void updateController(float triggerPitch) {
    if (operatingMode != OperatingMode::ARMED) {
        controllerState = ControllerState::NORMAL;
        highAngleWarningActive = false;

        if (!manualTestActive) {
            setOutputTarget(0);
        }
        return;
    }

    if (!imuHealthy) {
        controllerState = ControllerState::NORMAL;
        highAngleWarningActive = false;
        forceOutputOff();
        return;
    }

    ControllerState previousState = controllerState;
    bool outputOn = stepController(
        controllerState, triggerPitch, millis(), settings,
        triggerStartTime, wheelieStartTime
    );
    updateHighAngleWarning(triggerPitch);

    if (previousState != ControllerState::WHEELIE && controllerState == ControllerState::WHEELIE) {
        activeWheeliePeakAngle = triggerPitch;
        activeWheeliePeakG = currentGLoad;
    }
    if (controllerState == ControllerState::WHEELIE) {
        activeWheeliePeakAngle = max(activeWheeliePeakAngle, triggerPitch);
        activeWheeliePeakG = max(activeWheeliePeakG, currentGLoad);
    }
    if (previousState == ControllerState::WHEELIE && controllerState != ControllerState::WHEELIE) {
        completedWheelieCount++;
        lastWheelieDurationMs = millis() - wheelieStartTime;
        lastWheeliePeakAngle = activeWheeliePeakAngle;
        lastWheeliePeakG = activeWheeliePeakG;
    }

    uint8_t requestedBrightness = 0;
    if (highAngleWarningActive) {
        requestedBrightness = patternBrightness(
            settings.warningPattern, settings.warningBrightness, millis());
    } else if (outputOn) {
        requestedBrightness = patternBrightness(
            settings.wheeliePattern, settings.brightness, millis());
    }
    setOutputTarget(requestedBrightness, true);
    if (controllerState != previousState) displayDirty = true;
}

// =====================================================
// WRITE-TOKEN / WEB SECURITY HELPER
// =====================================================

String makeWriteToken() {
    char buffer[17];
    uint32_t a = esp_random();
    uint32_t b = esp_random();
    snprintf(buffer, sizeof(buffer), "%08lX%08lX", (unsigned long)a, (unsigned long)b);
    return String(buffer);
}

bool validWriteToken() {
    if (!server.hasArg("token")) return false;
    const String supplied = server.arg("token");
    if (supplied.length() != writeToken.length()) return false;
    uint8_t difference = 0;
    for (size_t index = 0; index < writeToken.length(); ++index) {
        difference |= (uint8_t)supplied[index] ^ (uint8_t)writeToken[index];
    }
    return difference == 0;
}

bool consumeWriteRequestLimit() {
    const uint32_t clientKey = (uint32_t)server.client().remoteIP();
    return checkWriteRateLimit(
        writeRateBuckets, WRITE_RATE_BUCKET_COUNT, clientKey, millis()) ==
        WriteRateLimitDecision::ALLOW;
}

bool allowWriteRequest() {
    if (!consumeWriteRequestLimit()) {
        server.sendHeader("Retry-After", "30");
        server.send(429, "text/plain", "Too many write requests — retry in 30 seconds");
        return false;
    }
    return true;
}

bool requireWriteToken() {
    if (!allowWriteRequest()) return false;
    if (!validWriteToken()) {
        server.send(403, "text/plain", "Invalid write token");
        return false;
    }
    return true;
}

void rotateWriteToken() {
    writeToken = makeWriteToken();
    server.sendHeader("X-Write-Token", writeToken);
}

String generateUniqueWiFiPassword() {
    static constexpr char alphabet[] =
        "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    String generated;
    generated.reserve(16);
    for (uint8_t index = 0; index < 16; ++index) {
        generated += alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    return generated;
}

// =====================================================
// WEB DASHBOARD
// =====================================================

const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>Wheelie Controller</title>
<style>
:root{color-scheme:dark;--bg:#070a0f;--panel:#101722;--panel2:#151e2b;--line:#263449;--text:#f6f8fc;--muted:#8b98aa;--blue:#4ca6ff;--violet:#8967ff;--cyan:#55e6ff;--green:#57e39a;--yellow:#ffd166;--red:#ff667a}
*{box-sizing:border-box}body{margin:0;min-height:100vh;background:radial-gradient(circle at 18% -10%,rgba(76,166,255,.22),transparent 34%),radial-gradient(circle at 90% 0,rgba(137,103,255,.16),transparent 30%),linear-gradient(180deg,#080c13,#06080c);color:var(--text);font-family:Inter,system-ui,-apple-system,Segoe UI,Roboto,sans-serif}.wrap{width:min(1080px,100%);margin:auto;padding:16px}.top{display:flex;align-items:center;justify-content:space-between;gap:12px;margin:4px 0 14px}.brand h1{font-size:clamp(22px,5vw,34px);margin:0;letter-spacing:-.04em}.brand p{margin:4px 0 0;color:var(--muted);font-size:13px}.topActions{display:flex;align-items:center;gap:8px}.iconBtn,.badge{height:42px;border:1px solid var(--line);background:rgba(16,23,34,.86);border-radius:13px;color:var(--text);display:flex;align-items:center;justify-content:center;text-decoration:none}.iconBtn{width:42px}.iconBtn svg{width:21px;height:21px}.badge{padding:0 12px;gap:7px;font-size:12px;font-weight:750}.dot{width:8px;height:8px;border-radius:50%;background:var(--green);box-shadow:0 0 12px var(--green)}.grid{display:grid;grid-template-columns:repeat(12,1fr);gap:13px}.card{grid-column:span 12;background:linear-gradient(180deg,rgba(19,28,41,.96),rgba(13,19,28,.96));border:1px solid var(--line);border-radius:19px;padding:16px;box-shadow:0 20px 60px rgba(0,0,0,.22)}@media(min-width:760px){.attitude{grid-column:span 8}.side{grid-column:span 4}.half{grid-column:span 6}}.eyebrow{color:var(--muted);font-size:11px;text-transform:uppercase;letter-spacing:.15em}.attitudeTop{display:flex;justify-content:space-between;gap:12px;align-items:end}.bigPitch{font-size:clamp(52px,11vw,88px);font-weight:850;line-height:.9;letter-spacing:-.075em}.pitchLabel{color:var(--muted);font-size:12px;margin-top:7px}.bikeStage{height:250px;margin:14px 0 10px;border-radius:17px;position:relative;overflow:hidden;background:radial-gradient(circle at 50% 30%,rgba(85,230,255,.10),transparent 35%),linear-gradient(180deg,#0b1320 0%,#0a1018 58%,#070a0f 100%);border:1px solid #26364a;perspective:700px}.horizon{position:absolute;left:-15%;right:-15%;bottom:36%;height:2px;background:linear-gradient(90deg,transparent,rgba(76,166,255,.6),rgba(137,103,255,.6),transparent);box-shadow:0 0 18px rgba(76,166,255,.32)}.road{position:absolute;width:150%;height:45%;left:-25%;bottom:-24%;background:linear-gradient(180deg,#101824,#070a0f);border-top:1px solid #2a3b51;transform:perspective(300px) rotateX(55deg)}.road:after{content:"";position:absolute;left:50%;top:0;width:3px;height:100%;background:repeating-linear-gradient(180deg,rgba(255,255,255,.65) 0 14px,transparent 14px 30px);transform:translateX(-50%)}.bikeWrap{position:absolute;left:50%;top:51%;width:min(480px,82%);transform:translate(-50%,-50%);transform-origin:50% 72%;transition:transform .12s linear;filter:drop-shadow(0 18px 14px rgba(0,0,0,.42))}.bikeSvg{width:100%;overflow:visible}.wheel{fill:#080a0e;stroke:#a6b7ca;stroke-width:7}.rim{fill:none;stroke:#394b60;stroke-width:3}.frame{fill:none;stroke:url(#frameGrad);stroke-width:8;stroke-linecap:round;stroke-linejoin:round}.metal{fill:none;stroke:#aab7c8;stroke-width:5;stroke-linecap:round}.bodyFill{fill:url(#bodyGrad);stroke:#7ac6ff;stroke-width:2}.bikeGlow{filter:drop-shadow(0 0 8px rgba(76,166,255,.28))}.telemetry{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}.mini{background:#0a1018;border:1px solid var(--line);border-radius:12px;padding:10px}.mini span{display:block;color:var(--muted);font-size:11px;margin-bottom:4px}.mini strong{font-size:16px}.side h2,.half h2{font-size:15px;margin:0 0 12px}.stats{display:grid;grid-template-columns:1fr 1fr;gap:8px}.stat{background:#0a1018;border:1px solid var(--line);border-radius:13px;padding:11px}.stat span{display:block;color:var(--muted);font-size:11px;margin-bottom:5px}.stat strong{font-size:15px}.peakGrid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:10px}.peak{position:relative;background:linear-gradient(180deg,#101a27,#0a1018);border:1px solid var(--line);border-radius:13px;padding:11px 36px 11px 11px}.peak span{display:block;color:var(--muted);font-size:11px;margin-bottom:5px}.peak strong{font-size:18px}.peakReset{position:absolute;right:8px;top:50%;transform:translateY(-50%);width:26px;height:26px;border-radius:8px;border:1px solid var(--line);background:#172131;color:var(--muted);font-size:16px;line-height:1;cursor:pointer}.peakReset:active{transform:translateY(-50%) scale(.95)}.meter{margin-top:13px}.meterHead{display:flex;justify-content:space-between;gap:8px;font-size:12px}.meterHead span{color:var(--muted)}.bar{height:10px;border-radius:999px;background:#080c12;border:1px solid var(--line);overflow:hidden;margin-top:7px;position:relative}.barFill{height:100%;width:0;background:linear-gradient(90deg,var(--blue),var(--cyan));transition:width .12s linear}.centerBar:after{content:"";position:absolute;left:50%;top:0;bottom:0;width:1px;background:#8190a4}.gyroFill{position:absolute;top:0;height:100%;left:50%;width:0;background:linear-gradient(90deg,var(--violet),var(--cyan));transition:left .12s linear,width .12s linear}.angleFlow{display:grid;grid-template-columns:1fr auto 1fr auto 1fr;gap:7px;align-items:center;text-align:center;margin-top:12px}.angleBox{padding:10px 5px;border-radius:11px;background:#0a1018;border:1px solid var(--line)}.angleBox small{display:block;color:var(--muted);font-size:10px;margin-bottom:4px}.arrow{color:var(--muted)}.statusGood{color:var(--green)}.statusBad{color:var(--red)}.statusWarn{color:var(--yellow)}.foot{color:var(--muted);font-size:11px;text-align:center;margin:14px 0 3px}@media(max-width:520px){.badge{display:none}.bikeStage{height:215px}.telemetry{grid-template-columns:1fr 1fr}.telemetry .mini:last-child{grid-column:1/-1}.angleFlow{gap:4px}.angleBox strong{font-size:13px}}
body[data-theme="light"]{color-scheme:light;--bg:#eef3f9;--panel:#fff;--panel2:#f5f8fc;--line:#cbd7e6;--text:#152033;--muted:#607087;background:radial-gradient(circle at 18% -10%,rgba(76,166,255,.18),transparent 34%),radial-gradient(circle at 90% 0,rgba(137,103,255,.12),transparent 30%),linear-gradient(180deg,#f8fbff,#eaf0f7)}
body[data-theme="light"] .card{background:linear-gradient(180deg,rgba(255,255,255,.98),rgba(245,248,252,.98));box-shadow:0 18px 50px rgba(45,67,94,.12)}body[data-theme="light"] .iconBtn,body[data-theme="light"] .badge{background:rgba(255,255,255,.9)}body[data-theme="light"] .mini,body[data-theme="light"] .stat,body[data-theme="light"] .angleBox{background:#f7faff}body[data-theme="light"] .peak{background:linear-gradient(180deg,#f7faff,#eef4fa)}body[data-theme="light"] .bar{background:#e2eaf4}body[data-theme="light"] .bikeStage{background:radial-gradient(circle at 50% 30%,rgba(85,230,255,.16),transparent 38%),linear-gradient(180deg,#eaf5ff 0%,#dce8f4 58%,#cdd9e6 100%)}
html,body{max-width:100%;overflow-x:hidden}.grid>*{min-width:0}.modeControl{position:relative;overflow:hidden;width:100%;font:inherit;color:var(--text);text-align:left;cursor:pointer;transition:border-color .18s,transform .12s,box-shadow .18s}.modeControl:after{content:"";position:absolute;left:0;bottom:0;width:var(--hold-progress,0%);height:3px;background:linear-gradient(90deg,var(--blue),var(--cyan));box-shadow:0 0 10px var(--blue);transition:width .08s linear}.modeControl.holding{border-color:var(--blue);box-shadow:0 0 0 3px rgba(76,166,255,.1)}.modeControl:hover{border-color:var(--blue);box-shadow:0 0 0 3px rgba(76,166,255,.1)}.modeControl:active{transform:scale(.98)}.modeControl[disabled]{cursor:wait;opacity:.65}.modeHint{font-size:9px!important;text-transform:uppercase;letter-spacing:.08em;margin-top:5px!important}.colorControl{position:relative;width:42px;height:42px;overflow:hidden}.colorControl input{position:absolute;inset:-8px;width:58px;height:58px;border:0;padding:0;cursor:pointer}.colorControl:after{content:"";position:absolute;inset:8px;border:2px solid rgba(255,255,255,.8);border-radius:50%;pointer-events:none}.bikeStage:after{content:"LIVE ATTITUDE";position:absolute;right:12px;bottom:10px;color:var(--muted);font-size:9px;letter-spacing:.16em}.bikeWrap{will-change:transform}.wheel{filter:drop-shadow(0 2px 2px rgba(0,0,0,.4))}
@media(max-width:900px){.wrap{padding:10px}.attitude,.side,.half{grid-column:1/-1!important}.bikeStage{height:clamp(190px,54vw,250px)}.card{padding:13px;border-radius:16px}.grid{gap:10px}.top{align-items:flex-start}.brand p{max-width:230px}.bigPitch{font-size:clamp(48px,15vw,72px)}}
@media(max-width:390px){.brand p{display:none}.topActions{gap:5px}.iconBtn,.colorControl{width:38px;height:38px}.stats,.peakGrid{grid-template-columns:1fr 1fr}.angleFlow{grid-template-columns:1fr auto 1fr auto 1fr}.card{padding:11px}.bikeStage{margin-top:10px}.foot{padding:0 8px}}
.instrumentCard{background:linear-gradient(180deg,rgba(16,23,34,.98),rgba(9,14,21,.98))}.stateBlock{text-align:right;padding:9px 12px;border:1px solid var(--line);border-radius:11px;background:rgba(8,13,20,.62)}.stateBlock strong{display:block;margin-top:4px;font-size:16px;letter-spacing:.04em}.instrumentCard .bikeStage{height:230px;background:linear-gradient(180deg,rgba(9,16,25,.98),rgba(7,11,17,.98));border-color:#243247;box-shadow:inset 0 0 45px rgba(0,0,0,.26)}.instrumentCard .bikeStage:after{content:none}.instrumentGrid{position:absolute;inset:0;background:repeating-linear-gradient(0deg,transparent 0 35px,rgba(130,153,180,.055) 35px 36px),linear-gradient(90deg,transparent 49.85%,rgba(130,153,180,.12) 50%,transparent 50.15%)}.horizon{bottom:34%;height:1px;background:linear-gradient(90deg,transparent 4%,rgba(76,166,255,.72) 20%,rgba(76,166,255,.72) 80%,transparent 96%);box-shadow:0 0 12px rgba(76,166,255,.2)}.horizon span{position:absolute;right:10px;top:-15px;font-size:8px;letter-spacing:.14em;color:var(--muted)}.pitchScale{position:absolute;z-index:2;left:8px;top:12px;bottom:12px;width:48px;overflow:hidden;color:var(--muted);font:9px ui-monospace,SFMono-Regular,Consolas,monospace;mask-image:linear-gradient(transparent,#000 14%,#000 86%,transparent)}.pitchScale:after{content:"";position:absolute;left:0;top:50%;width:36px;border-top:1px solid var(--cyan);box-shadow:0 0 7px rgba(85,230,255,.45)}.pitchTape{position:absolute;left:0;top:50%;width:100%;height:0;transform:translateY(0);transition:transform .12s linear;will-change:transform}.pitchTick{position:absolute;left:0;top:calc(var(--tick) * -3px);width:100%;transform:translateY(-50%);white-space:nowrap}.pitchTick:after{content:"";display:inline-block;width:12px;margin-left:6px;border-top:1px solid rgba(139,152,170,.42);vertical-align:middle}.pitchTick.major{color:var(--text);font-weight:700}.instrumentCard .bikeWrap{top:49%;width:min(440px,76%);filter:drop-shadow(0 15px 10px rgba(0,0,0,.34))}.instrumentCard .telemetry{grid-template-columns:1fr 1fr}.instrumentCard .mini{display:flex;align-items:baseline;justify-content:space-between;gap:12px;padding:11px 13px}.instrumentCard .mini span{margin:0}.instrumentCard .mini strong{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:15px}.instrumentCard .bigPitch{font-variant-numeric:tabular-nums}.instrumentCard .eyebrow{letter-spacing:.12em}
body[data-theme="light"] .instrumentCard{background:linear-gradient(180deg,#fff,#f3f7fb)}body[data-theme="light"] .instrumentCard .bikeStage{background:linear-gradient(180deg,#edf4fa,#dce6ef)}body[data-theme="light"] .stateBlock{background:#f6f9fc}
.modeControl{touch-action:manipulation;user-select:none;-webkit-user-select:none}
.pitchScale{-webkit-mask-image:linear-gradient(transparent,#000 14%,#000 86%,transparent)}
.instrumentGrid,.horizon{transition:opacity .2s ease}.tapeEnabled .instrumentGrid,.tapeEnabled .horizon{opacity:0}.tapeDisabled .pitchScale{display:none}.instrumentReadouts{position:absolute;z-index:4;right:10px;top:10px;bottom:10px;width:clamp(96px,24%,132px);display:flex;flex-direction:column;justify-content:space-between;pointer-events:none}.instrumentReadout{min-height:72px;padding:10px 11px;border:1px solid var(--line);border-radius:11px;background:rgba(8,13,20,.82);box-shadow:0 8px 24px rgba(0,0,0,.18);backdrop-filter:blur(5px)}.instrumentReadout .bigPitch{margin-top:4px;font-size:clamp(27px,5vw,39px);line-height:.95;letter-spacing:-.055em}.instrumentReadout strong{display:block;margin-top:7px;font-size:16px;letter-spacing:.035em}.instrumentReadout .pitchLabel{margin-top:5px;font-size:9px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.instrumentCard .bikeWrap{left:43%;width:min(390px,64%)}body[data-theme="light"] .instrumentReadout{background:rgba(247,250,253,.88)}
@media(max-width:420px){.instrumentReadouts{right:7px;top:7px;bottom:7px;width:96px}.instrumentReadout{min-height:66px;padding:8px}.instrumentReadout .bigPitch{font-size:27px}.instrumentCard .bikeWrap{left:41%;width:62%}}
@keyframes stageAlertFlash{0%,42%{opacity:.62}50%,92%{opacity:.08}100%{opacity:.62}}.bikeStage:before{content:"";position:absolute;z-index:1;inset:0;background:var(--stage-effect-color,transparent);opacity:0;pointer-events:none;transition:opacity .18s ease}.bikeStage.effectSolid:before{opacity:.52}.bikeStage.effectFlash:before{animation:stageAlertFlash .75s steps(1,end) infinite}.bikeStage.effectSolid,.bikeStage.effectFlash{border-color:var(--stage-effect-color);box-shadow:inset 0 0 45px rgba(0,0,0,.2),0 0 18px color-mix(in srgb,var(--stage-effect-color),transparent 55%)}
.bikeWrap{z-index:2}
.instrumentCard .telemetry{grid-template-columns:repeat(2,minmax(0,1fr))}.instrumentCard .telemetry .mini:last-child{grid-column:auto}.instrumentCard .telemetry .mini span{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.instrumentCard .telemetry .mini strong{flex:0 0 auto}
@media(max-width:380px){.instrumentCard .telemetry{grid-template-columns:1fr}.instrumentCard .telemetry .mini:last-child{grid-column:auto}}
.rideExit{display:none;position:absolute;z-index:8;right:10px;top:50%;transform:translateY(-50%);width:34px;height:34px;padding:0;border:1px solid var(--line);border-radius:10px;background:rgba(8,13,20,.78);color:var(--text);font-size:19px}.rideGraph{width:100%;height:122px;margin-top:12px;border:1px solid var(--line);border-radius:12px;background:#080d14}.eventStats{display:grid;grid-template-columns:repeat(2,1fr);gap:8px;margin-top:10px}.eventStat{padding:9px;border:1px solid var(--line);border-radius:11px;background:#0a1018}.eventStat span{display:block;color:var(--muted);font-size:10px}.eventStat strong{display:block;margin-top:4px;font-size:14px}.rideMode .top,.rideMode .side,.rideMode .telemetry,.rideMode .foot{display:none}.rideMode .wrap{width:100%;max-width:none;padding:0}.rideMode .grid{display:block}.rideMode .instrumentCard{min-height:100vh;border:0;border-radius:0;padding:8px}.rideMode .bikeStage{height:calc(100vh - 16px);margin:0;border-radius:14px}.rideMode .rideExit{display:block}
.pitchScale{left:calc(8px + env(safe-area-inset-left,0px));top:calc(12px + env(safe-area-inset-top,0px));bottom:calc(12px + env(safe-area-inset-bottom,0px))}
.viewSwitch{display:grid;grid-template-columns:1fr 1fr;gap:4px;width:min(390px,100%);margin:0 auto 13px;padding:4px;border:1px solid var(--line);border-radius:14px;background:rgba(10,16,24,.78)}.viewBtn{border:0;border-radius:10px;padding:9px 12px;background:transparent;color:var(--muted);font:inherit;font-size:12px;font-weight:750;cursor:pointer}.viewBtn.active{background:var(--panel2);color:var(--text);box-shadow:0 4px 14px rgba(0,0,0,.2)}.dashboardCard{display:none;grid-column:1/-1}.dashboardCard.activeView{display:block}.fullExit{display:none;position:absolute;z-index:8;right:calc(10px + env(safe-area-inset-right,0px));top:calc(10px + env(safe-area-inset-top,0px));width:38px;height:38px;padding:0;border:1px solid var(--line);border-radius:10px;background:rgba(8,13,20,.82);color:var(--text);font-size:21px}.side{position:relative}
.fullscreenView .top,.fullscreenView .viewSwitch,.fullscreenView .foot{display:none}.fullscreenView .wrap{width:100%;max-width:none;padding:0}.fullscreenView .grid{display:block}.fullscreenView .dashboardCard{display:none}.fullscreenView .dashboardCard.fullscreenActive{display:block;min-height:100svh;border:0;border-radius:0;padding:calc(8px + env(safe-area-inset-top,0px)) calc(8px + env(safe-area-inset-right,0px)) calc(8px + env(safe-area-inset-bottom,0px)) calc(8px + env(safe-area-inset-left,0px))}.fullscreenView .fullscreenActive .fullExit{display:block}.fullscreenView .instrumentCard .telemetry{display:none}.fullscreenView .instrumentCard .bikeStage{height:calc(100svh - 16px - env(safe-area-inset-top,0px) - env(safe-area-inset-bottom,0px));margin:0;border-radius:14px}.fullscreenView .side{overflow:auto}.fullscreenView .side h2{padding-right:52px}.fullscreenView .side .rideGraph{height:clamp(160px,28svh,280px)}
.fullscreenView .instrumentCard.fullscreenActive{padding:8px}.fullscreenView .instrumentCard .bikeStage{height:calc(100svh - 16px)}
body[data-theme="light"] .viewSwitch{background:rgba(255,255,255,.8)}body[data-theme="light"] .viewBtn.active{background:#fff}
.safetyStrip{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:7px;margin:0 0 10px}.safetyChip{min-width:0;padding:9px 10px;border:1px solid var(--line);border-radius:12px;background:rgba(10,16,24,.82)}.safetyChip span{display:block;color:var(--muted);font-size:9px;text-transform:uppercase;letter-spacing:.1em;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.safetyChip strong{display:block;margin-top:4px;font-size:13px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.safetyChip.modeArmed strong,.safetyChip.connected strong,.safetyChip.recording strong{color:var(--green)}.safetyChip.modeStandby strong,.safetyChip.loggingReady strong{color:var(--yellow)}.safetyChip.disconnected strong{color:var(--red)}.staleBanner{display:none;margin:-2px 0 10px;padding:9px 12px;border:1px solid #713545;border-radius:11px;background:#3d1c25;color:#ffabb7;font-size:12px;font-weight:750;text-align:center}.staleBanner.active{display:block}.warningRibbon{display:none;position:absolute;z-index:6;left:50%;top:8px;transform:translateX(-50%);padding:7px 12px;border:1px solid var(--red);border-radius:999px;background:rgba(78,19,29,.92);color:#ffbac3;font-size:11px;font-weight:900;letter-spacing:.09em;white-space:nowrap}.warningRibbon.on{display:block}.thresholdMarker{position:absolute;z-index:3;left:50px;right:108px;border-top:1px dashed;pointer-events:none;opacity:.8}.thresholdMarker span{position:absolute;left:4px;bottom:3px;font-size:8px;font-weight:800;letter-spacing:.08em}.thresholdMarker.trigger{color:var(--blue)}.thresholdMarker.warning{color:var(--red)}.leanGauge{position:absolute;z-index:5;left:42%;bottom:8px;width:min(160px,42%);transform:translateX(-50%);padding:6px 9px;border:1px solid var(--line);border-radius:10px;background:rgba(8,13,20,.82);backdrop-filter:blur(4px)}.leanHead{display:flex;justify-content:space-between;gap:8px;color:var(--muted);font-size:8px;text-transform:uppercase;letter-spacing:.08em}.leanHead strong{color:var(--text);font-size:10px}.leanTrack{position:relative;height:6px;margin-top:5px;border-radius:999px;background:#192536}.leanTrack:after{content:"";position:absolute;left:50%;top:-2px;bottom:-2px;border-left:1px solid var(--muted)}.leanDot{position:absolute;left:50%;top:50%;width:10px;height:10px;border:2px solid #081019;border-radius:50%;background:var(--cyan);box-shadow:0 0 8px rgba(85,230,255,.65);transform:translate(-50%,-50%)}.utilityActions{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:9px}.utilityBtn{min-height:44px;border:1px solid var(--line);border-radius:11px;background:#172131;color:var(--text);font:inherit;font-size:12px;font-weight:750;cursor:pointer}.graphHead{display:flex;align-items:end;justify-content:space-between;gap:10px;margin-top:13px}.graphHead strong{font-size:12px}.graphLegend{display:flex;gap:9px;color:var(--muted);font-size:9px}.graphLegend i{display:inline-block;width:12px;margin-right:3px;border-top:2px solid var(--cyan);vertical-align:middle}.graphLegend .triggerLine{border-color:var(--blue);border-top-style:dashed}.graphLegend .warningLine{border-color:var(--red);border-top-style:dashed}.diagnosticDetails{margin-top:10px;border:1px solid var(--line);border-radius:11px;background:#0a1018}.diagnosticDetails summary{min-height:44px;padding:13px;color:var(--muted);font-size:11px;font-weight:750;cursor:pointer}.diagnosticDetails .angleFlow{padding:0 10px 11px;margin:0}.lastRideCard{margin-top:10px;padding:11px;border:1px solid var(--line);border-radius:12px;background:#0a1018}.lastRideTop{display:flex;align-items:center;justify-content:space-between;gap:8px}.lastRideCard h3{margin:0;font-size:12px}.lastRideMeta{margin-top:5px;color:var(--muted);font-size:10px;line-height:1.4}.lastRideActions{display:flex;gap:7px;margin-top:9px}.lastRideActions button{min-height:44px;padding:0 13px;border:1px solid var(--line);border-radius:10px;background:#172131;color:var(--text);font:inherit;font-size:11px;font-weight:750}.peak{padding-right:55px}.peakReset,.fullExit,.rideExit{width:44px;height:44px}.peakReset{right:6px}.modeControl{min-height:86px}.modeControl:after{height:4px}.foot{line-height:1.4}
body[data-theme="sunlight"]{color-scheme:light;--bg:#fff;--panel:#fff;--panel2:#eef2f6;--line:#111827;--text:#05070a;--muted:#374151;--blue:#005fcc;--violet:#5432c7;--cyan:#006f82;--green:#006b3c;--yellow:#8a5700;--red:#b40020;background:#fff}body[data-theme="sunlight"] .card,body[data-theme="sunlight"] .instrumentCard{background:#fff;box-shadow:none;border-width:2px}body[data-theme="sunlight"] .instrumentCard .bikeStage{background:#f7fafc;border-width:2px}body[data-theme="sunlight"] .safetyChip,body[data-theme="sunlight"] .viewSwitch,body[data-theme="sunlight"] .stat,body[data-theme="sunlight"] .peak,body[data-theme="sunlight"] .lastRideCard,body[data-theme="sunlight"] .diagnosticDetails,body[data-theme="sunlight"] .eventStat{background:#fff}body[data-theme="sunlight"] .instrumentReadout,body[data-theme="sunlight"] .leanGauge{background:rgba(255,255,255,.96)}body[data-theme="sunlight"] .viewBtn.active{background:#dce7f2}body[data-theme="sunlight"] .bar,body[data-theme="sunlight"] .rideGraph{background:#f1f5f9}
body[data-theme="light"] .safetyChip,body[data-theme="light"] .stat,body[data-theme="light"] .lastRideCard,body[data-theme="light"] .diagnosticDetails,body[data-theme="light"] .eventStat{background:#f7faff}body[data-theme="light"] .leanGauge{background:rgba(247,250,253,.92)}body[data-theme="light"] .utilityBtn,body[data-theme="light"] .lastRideActions button{background:#e8f0f8;color:var(--text)}
@media(max-width:520px){.brand p{display:none}.top{margin-bottom:9px}.safetyStrip{gap:5px}.safetyChip{padding:8px 7px}.safetyChip strong{font-size:12px}.viewSwitch{margin-bottom:9px}.foot{display:none}.thresholdMarker{right:103px}.instrumentCard .bikeStage{height:clamp(255px,71vw,292px)}.instrumentCard .telemetry{margin-top:8px}.leanGauge{left:40%;width:43%}}
@media(max-width:360px){.safetyChip span{font-size:8px}.safetyChip strong{font-size:11px}.leanGauge{display:none}.thresholdMarker{right:98px}}
@media(prefers-reduced-motion:reduce){*,*:before,*:after{scroll-behavior:auto!important;transition:none!important;animation:none!important}.bikeStage.effectFlash:before{opacity:.52}}
.thresholdMarker span{top:3px;bottom:auto}
.warningRibbon{left:50px;right:108px;transform:none;text-align:center}
.safetyModeControl{position:relative;overflow:hidden;color:var(--text);font:inherit;text-align:left;cursor:pointer}.safetyModeControl:after{content:"";position:absolute;left:0;bottom:0;width:var(--hold-progress,0%);height:4px;background:linear-gradient(90deg,var(--blue),var(--cyan));box-shadow:0 0 10px var(--blue)}.safetyModeControl.holding{border-color:var(--blue);box-shadow:0 0 0 2px rgba(76,166,255,.14)}.safetyModeControl[disabled]{cursor:wait;opacity:.65}
</style>
</head>
<body>
<div class="wrap">
  <header class="top">
    <div class="brand"><h1>Wheelie Controller</h1><p>Live motorcycle attitude & lighting telemetry</p></div>
    <div class="topActions">
      <div class="badge"><span class="dot" id="connDot"></span><span id="connText">Connected</span></div>
      <button class="iconBtn" id="themeBtn" type="button" aria-label="Toggle day and night mode" title="Day / night mode">
        <svg id="themeIcon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 3a9 9 0 1 0 9 9 7 7 0 0 1-9-9Z"/></svg>
      </button>
      <button class="iconBtn" id="rideBtn" type="button" aria-label="Open full-screen riding view" title="Riding view">⛶</button>
      <a class="iconBtn" href="/settings" aria-label="Settings" title="Settings">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M14.7 6.3a4 4 0 0 0-5 5L3 18l3 3 6.7-6.7a4 4 0 0 0 5-5l-2.5 2.5-3-3 2.5-2.5Z"/></svg>
      </a>
    </div>
  </header>

  <div class="safetyStrip" aria-label="Controller safety status">
    <button class="safetyChip safetyModeControl modeStandby" id="quickModeChip" type="button" aria-label="Press and hold to change armed state"><span>Controller</span><strong id="quickMode">STANDBY</strong></button>
    <div class="safetyChip connected" id="quickConnectionChip"><span>Connection</span><strong id="quickConnection">LIVE</strong></div>
    <div class="safetyChip"><span>Light output</span><strong id="quickOutput">0%</strong></div>
    <div class="safetyChip" id="quickLogChip"><span>Ride logging</span><strong id="quickLog">OFF</strong></div>
  </div>
  <div class="staleBanner" id="staleBanner" role="alert">Telemetry paused — showing the last received values</div>

  <nav class="viewSwitch" aria-label="Dashboard view">
    <button class="viewBtn active" id="controllerViewBtn" type="button">Rider HUD</button>
    <button class="viewBtn" id="telemetryViewBtn" type="button">Telemetry</button>
  </nav>

  <main class="grid">
    <section class="card attitude instrumentCard dashboardCard activeView" id="controllerCard">
      <div class="bikeStage" id="bikeStage">
        <button class="fullExit" id="controllerExit" type="button" aria-label="Exit full screen">×</button>
        <div class="warningRibbon" id="warningRibbon" role="status">⚠ HIGH-ANGLE WARNING</div>
        <div class="thresholdMarker warning" id="warningMarker"><span>WARNING</span></div>
        <div class="thresholdMarker trigger" id="triggerMarker"><span>TRIGGER</span></div>
        <div class="instrumentGrid"></div><div class="horizon" id="hudHorizon"><span>LEVEL</span></div>
        <div class="pitchScale" aria-hidden="true"><div class="pitchTape" id="pitchTape"><span class="pitchTick major" style="--tick:60">60°</span><span class="pitchTick" style="--tick:50">50°</span><span class="pitchTick major" style="--tick:40">40°</span><span class="pitchTick" style="--tick:30">30°</span><span class="pitchTick major" style="--tick:20">20°</span><span class="pitchTick" style="--tick:10">10°</span><span class="pitchTick major" style="--tick:0">0°</span><span class="pitchTick" style="--tick:-10">−10°</span><span class="pitchTick major" style="--tick:-20">−20°</span><span class="pitchTick" style="--tick:-30">−30°</span><span class="pitchTick major" style="--tick:-40">−40°</span><span class="pitchTick" style="--tick:-50">−50°</span><span class="pitchTick major" style="--tick:-60">−60°</span></div></div>
        <div class="instrumentReadouts">
          <div class="instrumentReadout"><div class="eyebrow">Wheelie pitch</div><div class="bigPitch" id="pitch">0.0°</div><div class="pitchLabel" id="angleModeLine">ABSOLUTE mode</div></div>
          <div class="instrumentReadout"><div class="eyebrow">Detection</div><strong id="state">NORMAL</strong><div class="pitchLabel">Current state</div></div>
        </div>
        <div class="bikeWrap" id="bikeWrap">
          <svg class="bikeSvg bikeGlow" viewBox="0 0 520 210" aria-label="Motorcycle attitude display">
            <defs><linearGradient id="frameGrad" x1="0" x2="1"><stop id="frameColorA" stop-color="#55e6ff"/><stop id="frameColorB" offset="1" stop-color="#8967ff"/></linearGradient><linearGradient id="bodyGrad" x1="0" x2="1"><stop id="bikeColorA" stop-color="#1d86d9"/><stop id="bikeColorB" offset="1" stop-color="#6f4ed6"/></linearGradient></defs>
            <circle class="wheel" cx="115" cy="155" r="48"/><circle class="rim" cx="115" cy="155" r="31"/>
            <circle class="wheel" cx="410" cy="155" r="48"/><circle class="rim" cx="410" cy="155" r="31"/>
            <path class="frame" d="M115 155 L210 143 L272 90 L332 145 L410 155 M210 143 L265 145 L332 145 M272 90 L245 68"/>
            <path class="metal" d="M332 145 L370 73 L410 155 M370 73 L391 65 M245 68 L220 59"/>
            <path class="bodyFill" d="M214 105 C231 82 278 73 319 84 L348 109 L316 126 L246 122 Z"/>
            <path class="bodyFill" d="M214 104 L184 90 L169 101 L214 119 Z"/>
            <path class="metal" d="M310 84 L337 62 L370 73"/>
          </svg>
        </div>
        <div class="leanGauge" id="leanGauge"><div class="leanHead"><span>Lean</span><strong id="leanHud">0.0°</strong></div><div class="leanTrack"><i class="leanDot" id="leanDot"></i></div></div>
      </div>

      <div class="telemetry">
        <div class="mini"><span>Measured pitch</span><strong id="rawPitch">0.0°</strong></div>
        <div class="mini"><span>Baseline correction</span><strong id="baseline">0.0°</strong></div>
      </div>
      <span id="triggerPitch" hidden></span>
    </section>

    <section class="card side dashboardCard" id="telemetryCard">
      <button class="fullExit" id="telemetryExit" type="button" aria-label="Exit full screen">×</button>
      <h2>Live telemetry</h2>
      <div class="stats">
        <button class="stat modeControl" id="modeControl" type="button" aria-label="Press and hold to change armed state"><span>Controller</span><strong id="mode">---</strong><span class="modeHint" id="modeHint">Hold to arm</span></button>
        <div class="stat"><span>Output</span><strong id="output">0%</strong></div>
        <div class="stat"><span>IMU</span><strong id="imu">---</strong></div>
        <div class="stat"><span>Baseline</span><strong id="baselineState">---</strong></div>
        <div class="stat"><span>Lean angle</span><strong id="rollValue">0.0°</strong></div>
        <div class="stat"><span>Roll rate</span><strong id="rollRateValue">0.0°/s</strong></div>
      </div>
      <div class="utilityActions"><button class="utilityBtn" id="calibrateButton" type="button">Recalibrate level</button><a class="utilityBtn" href="/settings" style="display:grid;place-items:center;text-decoration:none">Settings</a></div>

      <div class="peakGrid">
        <div class="peak"><span>Highest angle</span><strong id="peakAngle">0.0°</strong><button class="peakReset" onclick="resetPeak('angle')" title="Reset highest angle" aria-label="Reset highest angle">↺</button></div>
        <div class="peak"><span>Highest +G</span><strong id="peakG">+0.00 g</strong><button class="peakReset" onclick="resetPeak('g')" title="Reset highest G load" aria-label="Reset highest G load">↺</button></div>
      </div>

      <div class="meter"><div class="meterHead"><span>Pitch rate / selected gyro</span><strong id="gyro">0.0°/s</strong></div><div class="bar centerBar"><div class="gyroFill" id="gyroFill"></div></div></div>
      <div class="meter"><div class="meterHead"><span>+G load</span><strong id="gload">+0.00 g</strong></div><div class="bar"><div class="barFill" id="gFill"></div></div></div>
      <div class="meter"><div class="meterHead"><span>Trigger threshold</span><strong id="threshold">20.0°</strong></div><div class="bar"><div class="barFill" id="pitchFill"></div></div></div>

      <div class="eventStats"><div class="eventStat"><span>Wheelies this session</span><strong id="eventCount">0</strong></div><div class="eventStat"><span>Current duration</span><strong id="activeDuration">0.0 s</strong></div><div class="eventStat"><span>Last duration</span><strong id="lastDuration">0.0 s</strong></div><div class="eventStat"><span>Last peak</span><strong id="lastEventPeak">0.0° · +0.00 g</strong></div></div>
      <div class="graphHead"><strong>Recent pitch · 30 seconds</strong><div class="graphLegend"><span><i></i>Pitch</span><span><i class="triggerLine"></i>Trigger</span><span><i class="warningLine"></i>Warning</span></div></div>
      <canvas class="rideGraph" id="rideGraph" aria-label="Recent pitch history with trigger and warning thresholds"></canvas>
      <details class="diagnosticDetails"><summary>Angle-processing diagnostics</summary><div class="angleFlow">
        <div class="angleBox"><small>RAW</small><strong id="raw2">0.0°</strong></div><div class="arrow">−</div>
        <div class="angleBox"><small>BASE</small><strong id="base2">0.0°</strong></div><div class="arrow">=</div>
        <div class="angleBox"><small>TRIGGER</small><strong id="trig2">0.0°</strong></div>
      </div></details>
      <div class="lastRideCard" id="lastRideCard"><div class="lastRideTop"><h3>Ride reports</h3><strong id="rideStorageSummary">Logging off</strong></div><div class="lastRideMeta" id="lastRideMeta">Enable ride logging in Settings to create hardware-recorded reports.</div><div class="lastRideActions" id="lastRideActions" hidden></div></div>
    </section>
  </main>
  <div class="foot">1× display · 2× angle mode · 3× Wi-Fi AP · hold armed/standby · 30 s hold resets Wi-Fi password</div>
</div>
<script>
let token='',refreshBusy=false,lastStatusAt=0,lastGraphAt=0;const $=id=>document.getElementById(id);const clamp=(v,a,b)=>Math.max(a,Math.min(b,v));const setText=(id,value)=>{const node=$(id);if(node&&node.textContent!==String(value))node.textContent=value};
const plusG=v=>'+'+Math.max(0,v).toFixed(2)+' g';
function gyroBar(v){v=clamp(v,-120,120);if(v>=0){$('gyroFill').style.left='50%';$('gyroFill').style.width=(v/120*50)+'%';}else{$('gyroFill').style.left=(50+v/120*50)+'%';$('gyroFill').style.width=(-v/120*50)+'%';}}
const saved=(key,fallback)=>{try{return localStorage.getItem(key)||fallback}catch(e){return fallback}};
const persist=(key,value)=>{try{localStorage.setItem(key,value)}catch(e){}};
function setTheme(theme){if(!['dark','light','sunlight'].includes(theme))theme='dark';document.body.dataset.theme=theme;persist('wheelieTheme',theme);$('themeBtn').title=theme==='dark'?'Switch to day theme':theme==='light'?'Switch to sunlight theme':'Switch to night theme';$('themeIcon').innerHTML=theme==='dark'?'<path d="M12 3a9 9 0 1 0 9 9 7 7 0 0 1-9-9Z"/>':theme==='light'?'<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>':'<path d="M3 12h18M12 3v18"/><circle cx="12" cy="12" r="7"/>';}
function shade(hex,amount){const n=parseInt(hex.slice(1),16),c=v=>Math.max(0,Math.min(255,v+amount)).toString(16).padStart(2,'0');return '#'+c(n>>16)+c((n>>8)&255)+c(n&255)}
function setBikeColor(color){$('bikeColorA').setAttribute('stop-color',shade(color,25));$('bikeColorB').setAttribute('stop-color',shade(color,-45));$('frameColorA').setAttribute('stop-color',shade(color,55));$('frameColorB').setAttribute('stop-color',shade(color,-15))}
function setPitchTape(enabled){document.body.classList.toggle('tapeEnabled',enabled);document.body.classList.toggle('tapeDisabled',!enabled)}
function setLeanGauge(enabled){$('leanGauge').hidden=!enabled}
let activeDashboardView=saved('wheelieDashboardView','controller');
function setDashboardView(view){activeDashboardView=view==='telemetry'?'telemetry':'controller';persist('wheelieDashboardView',activeDashboardView);$('controllerCard').classList.toggle('activeView',activeDashboardView==='controller');$('telemetryCard').classList.toggle('activeView',activeDashboardView==='telemetry');$('controllerViewBtn').classList.toggle('active',activeDashboardView==='controller');$('telemetryViewBtn').classList.toggle('active',activeDashboardView==='telemetry');}
function activeDashboardCard(){return $(activeDashboardView==='telemetry'?'telemetryCard':'controllerCard')}
function setFullscreen(enabled){const card=activeDashboardCard();document.body.classList.toggle('fullscreenView',enabled);document.querySelectorAll('.dashboardCard').forEach(c=>c.classList.toggle('fullscreenActive',enabled&&c===card));if(enabled){const request=card.requestFullscreen||card.webkitRequestFullscreen;if(request)try{const result=request.call(card);if(result&&result.catch)result.catch(()=>{})}catch(e){}}else{const exit=document.exitFullscreen||document.webkitExitFullscreen;if((document.fullscreenElement||document.webkitFullscreenElement)&&exit)try{const result=exit.call(document);if(result&&result.catch)result.catch(()=>{})}catch(e){}}}
$('controllerViewBtn').addEventListener('click',()=>setDashboardView('controller'));$('telemetryViewBtn').addEventListener('click',()=>setDashboardView('telemetry'));$('rideBtn').addEventListener('click',()=>setFullscreen(true));$('controllerExit').addEventListener('click',()=>setFullscreen(false));$('telemetryExit').addEventListener('click',()=>setFullscreen(false));document.addEventListener('fullscreenchange',()=>{if(!document.fullscreenElement)document.body.classList.remove('fullscreenView')});document.addEventListener('webkitfullscreenchange',()=>{if(!document.webkitFullscreenElement)document.body.classList.remove('fullscreenView')});setDashboardView(activeDashboardView);
const rideSamples=[];
function drawRideGraph(data){rideSamples.push({p:data.pitch,t:data.trigger,w:data.warningAngle});if(rideSamples.length>120)rideSamples.shift();const canvas=$('rideGraph'),dpr=Math.min(devicePixelRatio||1,2),w=Math.max(240,canvas.clientWidth),h=canvas.clientHeight;canvas.width=w*dpr;canvas.height=h*dpr;const c=canvas.getContext('2d');c.scale(dpr,dpr);c.clearRect(0,0,w,h);const maxY=Math.max(60,...rideSamples.map(s=>Math.max(s.p,s.w)))+5,toY=v=>h-8-(Math.max(0,v)/maxY)*(h-16);c.strokeStyle=getComputedStyle(document.body).getPropertyValue('--line');c.lineWidth=1;[0,20,40,60].forEach(v=>{c.beginPath();c.moveTo(0,toY(v));c.lineTo(w,toY(v));c.stroke()});const threshold=(key,color)=>{if(!rideSamples.length)return;c.strokeStyle=color;c.setLineDash([5,4]);c.beginPath();c.moveTo(0,toY(rideSamples[rideSamples.length-1][key]));c.lineTo(w,toY(rideSamples[rideSamples.length-1][key]));c.stroke();c.setLineDash([])};threshold('t','#4ca6ff');threshold('w','#ef4444');c.strokeStyle='#55e6ff';c.lineWidth=2;c.beginPath();rideSamples.forEach((s,i)=>{const x=i/Math.max(1,rideSamples.length-1)*w,y=toY(s.p);i?c.lineTo(x,y):c.moveTo(x,y)});c.stroke()}
const rideDuration=ms=>{const total=Math.round(ms/1000),minutes=Math.floor(total/60),seconds=total%60;return minutes+'m '+String(seconds).padStart(2,'0')+'s'};
async function downloadDashboardRide(id,format){const path=format==='csv'?'/api/ride/csv':'/api/ride/report';try{const r=await fetch(path+'?id='+encodeURIComponent(id));if(!r.ok)throw new Error(await r.text());const blob=await r.blob(),a=document.createElement('a');a.href=URL.createObjectURL(blob);a.download='wheelie-ride-'+id+(format==='csv'?'.csv':'-report.html');a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000)}catch(e){alert('Download failed: '+e.message)}}
async function refreshRideSummary(){try{const r=await fetch('/api/rides',{cache:'no-store'}),d=await r.json();setText('rideStorageSummary',d.enabled?d.sessions.length+' of '+d.maxSessions+' stored':'Logging off');const latest=d.sessions.find(s=>!s.active);if(!latest){setText('lastRideMeta',d.enabled?'No completed rides yet. Return to STANDBY to finalize a ride.':'Enable ride logging in Settings to create hardware-recorded reports.');$('lastRideActions').hidden=true;return}setText('lastRideMeta','Ride #'+latest.id+' · '+rideDuration(latest.durationMs)+' · '+latest.wheelies+' wheelies · '+latest.peakPitch.toFixed(1)+'° pitch · '+latest.peakRoll.toFixed(1)+'° lean');$('lastRideActions').innerHTML='<button onclick="downloadDashboardRide('+latest.id+',\'report\')">Report</button><button onclick="downloadDashboardRide('+latest.id+',\'csv\')">CSV</button>';$('lastRideActions').hidden=false}catch(e){setText('lastRideMeta','Ride report summary unavailable')}}
const stageEffectPrefs={wheelieMode:saved('wheelieEffectMode','off'),wheelieColor:saved('wheelieEffectColor','#22c55e'),warningMode:saved('warningEffectMode','off'),warningColor:saved('warningEffectColor','#ef4444'),warningAngle:clamp(parseFloat(saved('warningEffectAngle','15'))||15,0,70)};
function updateStageEffect(){const stage=$('bikeStage'),isWheelie=$('state').textContent==='WHEELIE',angle=parseFloat($('pitch').textContent)||0;let mode='off',color='transparent';if(angle>=stageEffectPrefs.warningAngle&&stageEffectPrefs.warningMode!=='off'){mode=stageEffectPrefs.warningMode;color=stageEffectPrefs.warningColor}else if(isWheelie&&stageEffectPrefs.wheelieMode!=='off'){mode=stageEffectPrefs.wheelieMode;color=stageEffectPrefs.wheelieColor}stage.classList.toggle('effectSolid',mode==='solid');stage.classList.toggle('effectFlash',mode==='flash');stage.style.setProperty('--stage-effect-color',color)}
$('themeBtn').addEventListener('click',()=>setTheme(document.body.dataset.theme==='dark'?'light':document.body.dataset.theme==='light'?'sunlight':'dark'));
setTheme(saved('wheelieTheme','dark'));setBikeColor(saved('wheelieBikeColor','#4ca6ff'));setPitchTape(saved('wheeliePitchTape','on')!=='off');setLeanGauge(saved('wheelieLeanGauge','on')!=='off');
new MutationObserver(()=>{const armed=$('mode').textContent==='ARMED';$('mode').className=armed?'statusGood':'statusWarn';$('modeHint').textContent=armed?'Hold to disarm':'Hold to arm';}).observe($('mode'),{childList:true});
new MutationObserver(()=>{const angle=clamp(parseFloat($('pitch').textContent)||0,-60,60);$('pitchTape').style.transform='translateY('+(angle*3)+'px)';updateStageEffect()}).observe($('pitch'),{childList:true});
new MutationObserver(updateStageEffect).observe($('state'),{childList:true});
const modeButtons=[$('modeControl'),$('quickModeChip')];
async function toggleMode(){if(!token)return;const next=$('mode').textContent==='ARMED'?'standby':'armed';modeButtons.forEach(button=>button.disabled=true);try{const r=await fetch('/api/mode?token='+encodeURIComponent(token)+'&mode='+next,{method:'POST'});if(!r.ok)alert(await r.text());await refresh();}catch(e){alert('Controller connection lost');}finally{modeButtons.forEach(button=>button.disabled=false)}}
let modeHoldStarted=0,modeHoldFrame=0,modeHoldButton=null;
function clearModeHold(){cancelAnimationFrame(modeHoldFrame);modeHoldStarted=0;if(modeHoldButton){modeHoldButton.classList.remove('holding');modeHoldButton.style.setProperty('--hold-progress','0%')}modeHoldButton=null}
async function recalibrateFromTile(){if(!confirm('Recalibrate the controller now? Keep the motorcycle level and completely still.'))return;const button=$('calibrateButton');button.disabled=true;try{const r=await fetch('/api/calibrate?token='+encodeURIComponent(token),{method:'POST'});alert(await r.text());await refresh()}catch(e){alert('Calibration request failed')}finally{button.disabled=false}}
function updateModeHold(now){if(!modeHoldStarted||!modeHoldButton)return;const progress=Math.min(100,(now-modeHoldStarted)/12);modeHoldButton.style.setProperty('--hold-progress',progress+'%');if(progress>=100){clearModeHold();toggleMode();return}modeHoldFrame=requestAnimationFrame(updateModeHold)}
function bindModeHold(button){button.addEventListener('pointerdown',e=>{if(e.button!==0||button.disabled)return;clearModeHold();modeHoldButton=button;modeHoldStarted=performance.now();button.classList.add('holding');if(button.setPointerCapture)button.setPointerCapture(e.pointerId);modeHoldFrame=requestAnimationFrame(updateModeHold)});button.addEventListener('pointerup',clearModeHold);button.addEventListener('pointercancel',clearModeHold);button.addEventListener('contextmenu',e=>e.preventDefault());button.addEventListener('keydown',e=>{if((e.key==='Enter'||e.key===' ')&&!e.repeat){e.preventDefault();if(confirm('Change controller mode?'))toggleMode()}})}
modeButtons.forEach(bindModeHold);
$('calibrateButton').addEventListener('click',recalibrateFromTile);
async function refresh(){if(refreshBusy)return;refreshBusy=true;try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw new Error('status');const d=await r.json();token=d.token;lastStatusAt=performance.now();$('staleBanner').classList.remove('active');setText('pitch',d.pitch.toFixed(1)+'°');setText('rawPitch',d.rawPitch.toFixed(1)+'°');setText('baseline',d.baseline.toFixed(1)+'°');setText('triggerPitch',d.pitch.toFixed(1)+'°');setText('raw2',d.rawPitch.toFixed(1)+'°');setText('base2',d.baseline.toFixed(1)+'°');setText('trig2',d.pitch.toFixed(1)+'°');setText('state',d.state);$('state').className=d.state==='WHEELIE'?'statusGood':d.state==='PENDING'?'statusWarn':'';setText('mode',d.mode);setText('output',d.output+'%');setText('imu',d.imu?'OK':'FAULT');$('imu').className=d.imu?'statusGood':'statusBad';setText('baselineState',d.angleMode==='adaptive'?(d.baselineFrozen?'FROZEN':'TRACKING'):'FIXED');setText('angleModeLine',d.angleMode.toUpperCase()+' angle mode');setText('gyro',d.gyroRate.toFixed(1)+'°/s');setText('gload',plusG(d.gLoad));setText('peakAngle',d.peakAngle.toFixed(1)+'°');setText('peakG',plusG(d.peakG));setText('threshold',d.trigger.toFixed(1)+'°');setText('rollValue',d.roll.toFixed(1)+'°');setText('rollRateValue',d.rollRate.toFixed(1)+'°/s');setText('leanHud',(d.roll>0?'+':'')+d.roll.toFixed(1)+'°');setText('eventCount',d.eventCount);setText('activeDuration',(d.activeDuration/1000).toFixed(1)+' s');setText('lastDuration',(d.lastDuration/1000).toFixed(1)+' s');setText('lastEventPeak',d.lastPeakAngle.toFixed(1)+'° · +'+d.lastPeakG.toFixed(2)+' g');const armed=d.mode==='ARMED';setText('quickMode',d.mode);$('quickModeChip').classList.toggle('modeArmed',armed);$('quickModeChip').classList.toggle('modeStandby',!armed);setText('quickConnection','LIVE');$('quickConnectionChip').className='safetyChip connected';setText('quickOutput',d.output+'%');const logText=!d.rideLoggingAvailable?'UNAVAILABLE':d.rideLoggingActive?'REC '+rideDuration(d.rideSampleCount/d.rideSampleRateHz*1000):d.rideLoggingEnabled?'READY':'OFF';setText('quickLog',logText);$('quickLogChip').className='safetyChip '+(d.rideLoggingActive?'recording':d.rideLoggingEnabled?'loggingReady':'');$('warningRibbon').classList.toggle('on',d.warningActive);$('warningMarker').style.top='calc(50% - '+clamp(d.warningAngle,-60,60)*3+'px)';$('triggerMarker').style.top='calc(50% - '+clamp(d.trigger,-60,60)*3+'px)';$('hudHorizon').style.transform='rotate('+(-clamp(d.roll,-45,45))+'deg)';$('leanDot').style.transform='translate(calc(-50% + '+(clamp(d.roll,-50,50)/50*65)+'px),-50%)';gyroBar(d.gyroRate);$('gFill').style.width=clamp(d.gLoad/2*100,0,100)+'%';$('pitchFill').style.width=clamp(Math.max(0,d.pitch)/Math.max(1,d.trigger)*100,0,100)+'%';$('bikeWrap').style.transform='translate(-50%,-50%) rotate('+(-clamp(d.rawPitch,-55,55))+'deg)';$('connDot').style.background='var(--green)';setText('connText','Connected');if(performance.now()-lastGraphAt>=200){lastGraphAt=performance.now();drawRideGraph(d)}}catch(e){$('connDot').style.background='var(--red)';setText('connText','Disconnected');setText('quickConnection','LOST');$('quickConnectionChip').className='safetyChip disconnected';$('staleBanner').classList.add('active')}finally{refreshBusy=false}}
function updateStaleState(){if(lastStatusAt&&performance.now()-lastStatusAt>1500){$('staleBanner').classList.add('active');setText('quickConnection','STALE');$('quickConnectionChip').className='safetyChip disconnected'}}
async function resetPeak(kind){if(!token||!confirm('Reset the highest '+(kind==='g'?'+G load':'angle')+' value?'))return;try{await fetch('/api/peak/reset?token='+encodeURIComponent(token)+'&kind='+encodeURIComponent(kind),{method:'POST'});refresh()}catch(e){}}
refresh();refreshRideSummary();setInterval(()=>{if(!document.hidden)refresh()},100);setInterval(updateStaleState,500);setInterval(()=>{if(!document.hidden)refreshRideSummary()},5000);document.addEventListener('visibilitychange',()=>{if(!document.hidden){refresh();refreshRideSummary()}});
</script>
</body>
</html>
)rawliteral";

const char SETTINGS_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"><title>Wheelie Settings</title>
<style>
:root{color-scheme:dark;--bg:#080b10;--panel:#111720;--line:#273449;--text:#f4f7fb;--muted:#8f9bad;--accent:#4ea1ff;--accent2:#8f66ff;--good:#52df9a;--warn:#ffc857;--bad:#ff6577}*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 12% 0,rgba(78,161,255,.18),transparent 30%),var(--bg);color:var(--text);font-family:Inter,system-ui,-apple-system,Segoe UI,Roboto,sans-serif}.wrap{width:min(940px,100%);margin:auto;padding:16px}.top{display:flex;align-items:center;gap:10px;margin:4px 0 15px}.back{width:42px;height:42px;display:grid;place-items:center;border:1px solid var(--line);border-radius:13px;background:#111720;color:white;text-decoration:none;font-size:24px}.title h1{font-size:25px;margin:0}.title p{margin:3px 0 0;color:var(--muted);font-size:12px}.grid{display:grid;grid-template-columns:repeat(12,1fr);gap:13px}.card{grid-column:span 12;background:linear-gradient(180deg,#151d28,#0f151e);border:1px solid var(--line);border-radius:18px;padding:17px}@media(min-width:740px){.half{grid-column:span 6}}h2{font-size:15px;margin:0 0 13px}.rows{display:grid}.row{display:flex;justify-content:space-between;gap:12px;padding:10px 0;border-bottom:1px solid var(--line)}.row:last-child{border-bottom:0}.row span,label{color:var(--muted)}label{display:block;font-size:12px;margin:12px 0 6px}input,select{width:100%;padding:12px;border-radius:11px;border:1px solid var(--line);background:#090e15;color:white;font:inherit}.range{display:grid;grid-template-columns:1fr 78px;gap:9px;align-items:center}.range input{padding:0}.range output{text-align:center;background:#090e15;border:1px solid var(--line);border-radius:10px;padding:9px 4px}.actions{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:12px}.wide{grid-column:1/-1}button{border:0;border-radius:11px;padding:13px;font:inherit;font-weight:750;background:#29364a;color:white;cursor:pointer}.primary{background:linear-gradient(135deg,var(--accent),var(--accent2))}.good{background:#173b2d;color:#7cf5b9;border:1px solid #245c43}.warn{background:#3b3118;color:#ffd978;border:1px solid #665326}.bad{background:#421e27;color:#ff9dab;border:1px solid #6d2f3d}.note{font-size:12px;line-height:1.5;color:var(--muted)}.message{position:sticky;bottom:10px;width:max-content;max-width:100%;margin:14px auto 0;padding:9px 14px;background:#121a25;border:1px solid var(--line);border-radius:999px;color:var(--muted);font-size:12px}.hidden{display:none}.file{border-style:dashed}.statusGood{color:var(--good)}.statusBad{color:var(--bad)}.statusWarn{color:var(--warn)}.proof{display:inline-grid;place-items:center;text-align:center;padding:11px 14px;border:2px solid var(--good);border-radius:13px;color:#7cf5b9;background:#102a21;font-size:12px;font-weight:900;letter-spacing:.08em;transform:rotate(1deg)}.proof small{display:block;margin-top:3px;color:#a9c6b8;font-size:8px}.reportHead{display:flex;justify-content:space-between;align-items:start;gap:14px}.rideList{display:grid;gap:9px;margin-top:14px}.rideItem{padding:12px;border:1px solid var(--line);border-radius:13px;background:#090e15}.rideTop{display:flex;justify-content:space-between;gap:10px}.rideMeta{margin-top:5px;color:var(--muted);font-size:11px}.rideActions{display:flex;gap:7px;margin-top:10px}.rideActions button{padding:9px 11px;font-size:12px}@media(max-width:460px){.reportHead{display:block}.proof{margin:6px 0 10px}.rideTop{display:block}}
</style>
</head>
<body><div class="wrap">
<header class="top"><a class="back" href="/" aria-label="Dashboard">‹</a><div class="title"><h1>Controller Settings</h1><p>Configuration, service controls & firmware</p></div></header>
<div class="grid">
<section class="card half"><h2>Wheelie Detection</h2><label>Angle processing mode</label><select id="angleMode"><option value="absolute">Absolute — fixed calibration zero</option><option value="adaptive">Adaptive — follows gradual terrain</option></select><div id="adaptiveSettings"><label>Adaptive zero time constant (seconds)</label><input id="adaptiveTau" type="number" min="0.5" max="60" step="0.5"><label>Freeze baseline above pitch rate (°/sec)</label><input id="freezeRate" type="number" min="1" max="100" step="0.5"><p class="note">The adaptive baseline follows hills slowly, but freezes during rapid pitch changes and wheelie detection.</p></div><label>Trigger angle (°)</label><input id="trigger" type="number" min="5" max="70" step="0.5"><label>Reset angle (°)</label><input id="reset" type="number" min="0" max="69" step="0.5"><label>Trigger hold (ms)</label><input id="hold" type="number" min="0" max="5000" step="10"><label>Minimum ON time (ms)</label><input id="minon" type="number" min="0" max="15000" step="50"></section>
<section class="card half"><h2>Output & Startup</h2><label>Wheelie brightness</label><div class="range"><input id="brightness" type="range" min="1" max="100"><output id="brightnessOut">100%</output></div><label>Fade time</label><div class="range"><input id="fade" type="range" min="0" max="3000" step="50"><output id="fadeOut">200 ms</output></div><label>Default power-up mode</label><select id="bootMode"><option value="standby">Standby</option><option value="armed">Armed</option></select><label>Ride telemetry logging</label><select id="rideLogging"><option value="disabled">Disabled — default</option><option value="enabled">Enabled — retain 3 rides</option></select><div class="actions"><button class="primary wide" onclick="saveSettings()">Save Settings</button></div><p class="note">Ride logging is opt-in and disabled by default. Saving settings cancels any pending/active wheelie state and commands the output OFF while the new values are applied.</p></section>
<section class="card half"><h2>Mounting & Orientation</h2><input type="hidden" id="rotationAxis" value="y"><div class="rows"><div class="row"><span>Setup</span><strong id="orientationState">---</strong></div><div class="row"><span>Pitch axis</span><strong id="pitchAxisLive">---</strong></div><div class="row"><span>Roll axis</span><strong id="rollAxisLive">---</strong></div><div class="row"><span>Vertical axis</span><strong id="verticalAxisLive">---</strong></div><div class="row"><span>Live lean</span><strong id="leanAngle">0.0°</strong></div></div><div class="actions"><button class="primary wide" onclick="runOrientationWizard()">Run Mounting Wizard</button></div><p class="note">This runs automatically on first setup and is saved in device memory. Park securely, hold the bike upright, then lean it smoothly side to side several times without lifting the wheels. Output remains OFF and the controller stays in STANDBY afterward.</p></section>
<section class="card half"><h2>Dashboard Appearance</h2><label for="bikeColor">Motorcycle accent color</label><div style="display:grid;grid-template-columns:70px 1fr;gap:12px;align-items:center"><input id="bikeColor" type="color" value="#4ca6ff" style="height:54px;padding:5px;cursor:pointer"><div id="colorSwatch" style="height:54px;border-radius:11px;border:1px solid var(--line);background:var(--bikePreview);box-shadow:inset 0 0 0 1px rgba(255,255,255,.08)"></div></div><label for="pitchTapeSetting">Pitch tape</label><select id="pitchTapeSetting"><option value="on">Shown</option><option value="off">Hidden</option></select><label for="leanGaugeSetting">Rider HUD lean gauge</label><select id="leanGaugeSetting"><option value="on">Shown</option><option value="off">Hidden</option></select><div class="actions"><button class="wide" type="button" onclick="resetBikeColor()">Reset Default Color</button></div><p class="note">The lean gauge uses the roll axis saved by the mounting wizard. Appearance preferences are stored by this browser and apply to the dashboard on this device.</p></section>
<section class="card half"><h2>Pitch Display Alerts</h2><label for="wheelieEffectMode">Wheelie background</label><select id="wheelieEffectMode"><option value="off">Off</option><option value="solid">Solid color</option><option value="flash">Flashing color</option></select><label for="wheelieEffectColor">Wheelie color</label><input id="wheelieEffectColor" type="color" value="#22c55e" style="height:48px;padding:5px;cursor:pointer"><label for="warningEffectMode">Angle warning background</label><select id="warningEffectMode"><option value="off">Off</option><option value="solid">Solid color</option><option value="flash">Flashing color</option></select><div style="display:grid;grid-template-columns:1fr 82px;gap:10px"><div><label for="warningEffectAngle">Warning angle (°)</label><input id="warningEffectAngle" type="number" min="0" max="70" step="0.5" value="15"></div><div><label for="warningEffectColor">Color</label><input id="warningEffectColor" type="color" value="#ef4444" style="height:48px;padding:5px;cursor:pointer"></div></div><p class="note">Both effects are disabled by default. The high-angle warning takes priority whenever its threshold is reached.</p></section>
<section class="card half"><h2>Physical Light Patterns</h2><label for="wheeliePattern">Wheelie pattern</label><select id="wheeliePattern"><option value="0">Off</option><option value="1">Solid</option><option value="2">Slow pulse</option><option value="3">Fast pulse</option><option value="4">Strobe</option></select><label for="warningPattern">High-angle warning pattern</label><select id="warningPattern"><option value="0">Off</option><option value="1">Solid</option><option value="2">Slow pulse</option><option value="3">Fast pulse</option><option value="4">Strobe</option></select><label for="warningBrightness">Warning brightness</label><div class="range"><input id="warningBrightness" type="range" min="1" max="100"><output id="warningBrightnessOut">100%</output></div><label for="warningAngleFirmware">Warning activation angle (°)</label><input id="warningAngleFirmware" type="number" min="5" max="85" step="0.5"><label for="warningResetFirmware">Warning release angle (°)</label><input id="warningResetFirmware" type="number" min="0" max="84" step="0.5"><label for="warningRateFirmware">Early warning pitch rate (°/s, 0 disables)</label><input id="warningRateFirmware" type="number" min="0" max="250" step="1"><p class="note">The warning uses angle hysteresis to prevent chatter. A rapid rise can activate it early after the normal wheelie threshold is crossed.</p></section>
<section class="card half"><h2>Controller</h2><div class="rows"><div class="row"><span>Current mode</span><strong id="mode">---</strong></div><div class="row"><span>IMU</span><strong id="imu">---</strong></div><div class="row"><span>Angle mode</span><strong id="angleModeLive">---</strong></div><div class="row"><span>Current +G load</span><strong id="gload">---</strong></div></div><div class="actions"><button class="good" onclick="setMode('armed')">ARM</button><button class="warn" onclick="setMode('standby')">STANDBY</button><button class="bad wide" onclick="calibrate()">Recalibrate Level</button></div><p class="note">Engine-idle vibration is acceptable during calibration as long as the motorcycle remains stationary and is not being rocked.</p></section>
<section class="card half"><h2>Manual Output Test</h2><div class="actions"><button onclick="testOutput(25)">25%</button><button onclick="testOutput(50)">50%</button><button onclick="testOutput(75)">75%</button><button onclick="testOutput(100)">100%</button><button class="bad wide" onclick="testOutput(0)">OFF</button></div><p class="note">Only available in STANDBY. Non-zero test commands automatically expire after 10 seconds.</p></section>
<section class="card half"><h2>Network & System</h2><div class="rows"><div class="row"><span>Access point</span><strong id="apState">ON</strong></div><div class="row"><span>SSID</span><strong id="apSsid">---</strong></div><div class="row"><span>Friendly address</span><strong>wheelie.local</strong></div><div class="row"><span>Guaranteed fallback</span><strong>192.168.4.1</strong></div><div class="row"><span>Discovery</span><strong id="discovery">---</strong></div><div class="row"><span>Clients</span><strong id="clients">0</strong></div><div class="row"><span>Uptime</span><strong id="uptime">0 sec</strong></div><div class="row"><span>Firmware</span><strong id="firmware">---</strong></div><div class="row"><span>Build commit</span><strong id="buildCommit">---</strong></div><div class="row"><span>Build date</span><strong id="buildDate">---</strong></div><div class="row"><span>Release channel</span><strong id="releaseChannel">---</strong></div><div class="row"><span>Hardware target</span><strong id="hardwareTarget">---</strong></div></div><label>Wi-Fi password</label><input id="wifiPassword" type="password" minlength="8" maxlength="63" autocomplete="new-password" placeholder="Enter a new 8-63 character password"><div class="actions"><button class="primary wide" onclick="saveWifiPassword()">Update Wi-Fi Password</button><button class="wide" onclick="generateWifiPassword()">Generate Unique Device Password</button></div><p class="note">Changing or generating the password restarts the access point and disconnects this browser. A generated password is shown once so it can be copied before reconnecting. Hold the physical button for <b>30 seconds</b> to restore <b>wheeliectrl</b>.</p><p class="note"><b>Triple-tap</b> toggles the access point. mDNS and the captive DNS/portal fallback remain enabled; use <b>192.168.4.1</b> if <b>wheelie.local</b> is unavailable.</p></section>
<section class="card"><div class="reportHead"><div><h2>Ride Reports</h2><p class="note">When enabled, telemetry is sampled at 5 Hz for up to 90 minutes per ride. Only the newest 3 sessions are kept; the oldest is overwritten automatically.</p></div><div class="proof">DEVICE-RECORDED<small>SENSOR DATA · SHA-256</small></div></div><div id="rideReports" class="rideList"><div class="rideItem note">Loading ride sessions…</div></div><p class="note">The checksum can detect changes to the sample stream. It is not a third-party certification or a cryptographic device signature.</p></section>
<section class="card half"><h2>Configuration Backup</h2><div class="actions"><button onclick="exportSettings()">Export JSON</button><button onclick="$('settingsFile').click()">Import JSON</button></div><input class="file hidden" id="settingsFile" type="file" accept=".json,application/json"><p class="note">Export includes controller settings and browser-local dashboard appearance. Imported values are validated by the controller when applied.</p></section>
<section class="card half"><h2>Firmware Update & Recovery</h2><label for="otaChannel">Accepted OTA channel</label><select id="otaChannel"><option value="stable">Stable — main branch releases</option><option value="testing">Testing — testing branch builds</option></select><input class="file" id="firmwareFile" type="file" accept=".wctrl,application/octet-stream"><div class="actions"><button class="primary wide" onclick="uploadFirmware()">Upload Signed OTA Package</button><button class="bad wide" onclick="rollbackFirmware()">Return to Previous Firmware</button></div><p class="note">Only signed <b>.wctrl</b> packages for this XIAO ESP32-S3 and the selected channel are accepted. The firmware SHA-256 and ECDSA signature are verified before the OTA partition is activated. Save Settings after changing channels.</p><p class="note">The output is forced OFF during updates and rollback. The previous image remains in the alternate OTA partition until another update replaces it.</p></section>
</div><div class="message" id="message">Ready</div></div>
<script>
let token='',settingsLoaded=false,axisLoaded=false,advancedLoaded=false;const $=id=>document.getElementById(id);const msg=t=>$('message').textContent=t;function syncAdaptive(){$('adaptiveSettings').classList.toggle('hidden',$('angleMode').value!=='adaptive');}$('angleMode').addEventListener('change',syncAdaptive);$('brightness').addEventListener('input',()=>$('brightnessOut').textContent=$('brightness').value+'%');$('fade').addEventListener('input',()=>$('fadeOut').textContent=$('fade').value+' ms');$('warningBrightness').addEventListener('input',()=>$('warningBrightnessOut').textContent=$('warningBrightness').value+'%');
const savedBikeColor=(()=>{try{return localStorage.getItem('wheelieBikeColor')||'#4ca6ff'}catch(e){return '#4ca6ff'}})();
function applyBikeColor(color){$('bikeColor').value=color;$('colorSwatch').style.setProperty('--bikePreview',color);try{localStorage.setItem('wheelieBikeColor',color)}catch(e){}}
function resetBikeColor(){applyBikeColor('#4ca6ff');msg('Dashboard bike color reset')}
$('bikeColor').addEventListener('input',e=>applyBikeColor(e.target.value));applyBikeColor(savedBikeColor);
let savedPitchTape='on';try{savedPitchTape=localStorage.getItem('wheeliePitchTape')||'on'}catch(e){}$('pitchTapeSetting').value=savedPitchTape;$('pitchTapeSetting').addEventListener('change',e=>{try{localStorage.setItem('wheeliePitchTape',e.target.value)}catch(err){}msg('Pitch tape preference saved')});
let savedLeanGauge='on';try{savedLeanGauge=localStorage.getItem('wheelieLeanGauge')||'on'}catch(e){}$('leanGaugeSetting').value=savedLeanGauge;$('leanGaugeSetting').addEventListener('change',e=>{try{localStorage.setItem('wheelieLeanGauge',e.target.value)}catch(err){}msg('Lean gauge preference saved')});
const localValue=(key,fallback)=>{try{return localStorage.getItem(key)||fallback}catch(e){return fallback}};const effectFields={wheelieEffectMode:'off',wheelieEffectColor:'#22c55e',warningEffectMode:'off',warningEffectAngle:'15',warningEffectColor:'#ef4444'};Object.entries(effectFields).forEach(([id,fallback])=>{$(id).value=localValue(id,fallback);$(id).addEventListener('change',e=>{let value=e.target.value;if(id==='warningEffectAngle'){value=String(Math.max(0,Math.min(70,parseFloat(value)||15)));e.target.value=value}try{localStorage.setItem(id,value)}catch(err){}msg('Pitch alert preference saved')})});
async function refresh(){try{const r=await fetch('/api/status',{cache:'no-store'});const d=await r.json();token=d.token;$('mode').textContent=d.mode;$('imu').textContent=d.imu?'OK':'FAULT';$('imu').className=d.imu?'statusGood':'statusBad';$('angleModeLive').textContent=d.angleMode.toUpperCase();$('gload').textContent='+'+d.gLoad.toFixed(2)+' g';$('orientationState').textContent=d.orientationConfigured?'SAVED':'REQUIRED';$('orientationState').className=d.orientationConfigured?'statusGood':'statusWarn';$('pitchAxisLive').textContent=d.rotationAxis.toUpperCase();$('rollAxisLive').textContent=d.rollAxis.toUpperCase();$('verticalAxisLive').textContent=d.verticalAxis.toUpperCase();$('leanAngle').textContent=d.roll.toFixed(1)+'°';$('rotationAxis').value=d.rotationAxis;$('apState').textContent=d.apEnabled?'ON':'OFF';$('discovery').textContent=(d.mdns?'mDNS ':'')+(d.dns?'Captive DNS':'')||'IP only';$('clients').textContent=d.clients;$('uptime').textContent=d.uptime+' sec';$('firmware').textContent=d.firmware;$('buildCommit').textContent=d.buildCommit;$('buildDate').textContent=d.buildDate;$('releaseChannel').textContent=d.releaseChannel.toUpperCase();$('hardwareTarget').textContent=d.board+' / '+d.chip;if(!settingsLoaded){$('angleMode').value=d.angleMode;$('adaptiveTau').value=d.adaptiveTau;$('freezeRate').value=d.freezeRate;$('trigger').value=d.trigger;$('reset').value=d.reset;$('hold').value=d.hold;$('minon').value=d.minon;$('brightness').value=d.brightness;$('brightnessOut').textContent=d.brightness+'%';$('fade').value=d.fade;$('fadeOut').textContent=d.fade+' ms';$('bootMode').value=d.bootArmed?'armed':'standby';$('rideLogging').value=d.rideLoggingEnabled?'enabled':'disabled';$('otaChannel').value=d.otaChannel;syncAdaptive();settingsLoaded=true;}}catch(e){msg('Connection lost');}}
async function saveSettings(){const body=new URLSearchParams({angleMode:$('angleMode').value,rotationAxis:$('rotationAxis').value,adaptiveTau:$('adaptiveTau').value,freezeRate:$('freezeRate').value,trigger:$('trigger').value,reset:$('reset').value,hold:$('hold').value,minon:$('minon').value,brightness:$('brightness').value,fade:$('fade').value,bootMode:$('bootMode').value,rideLogging:$('rideLogging').value,wheeliePattern:$('wheeliePattern').value,warningPattern:$('warningPattern').value,warningBrightness:$('warningBrightness').value,warningAngle:$('warningAngleFirmware').value,warningReset:$('warningResetFirmware').value,warningRate:$('warningRateFirmware').value,otaChannel:$('otaChannel').value});const r=await fetch('/api/settings?token='+encodeURIComponent(token),{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});msg(await r.text());settingsLoaded=false;axisLoaded=false;advancedLoaded=false;refresh();refreshSsid();refreshRideReports();}
async function setMode(mode){const r=await fetch('/api/mode?token='+encodeURIComponent(token)+'&mode='+mode,{method:'POST'});msg(await r.text());refresh();}
async function calibrate(){msg('Calibrating — keep bike stationary...');const r=await fetch('/api/calibrate?token='+encodeURIComponent(token),{method:'POST'});msg(await r.text());refresh();}
async function runOrientationWizard(){if(!confirm('Park securely and run the mounting wizard now? The controller will enter STANDBY. Follow the OLED prompts: upright first, then lean side to side.'))return;msg('Wizard running — follow the OLED prompts on the bike...');try{const r=await fetch('/api/orientation?token='+encodeURIComponent(token),{method:'POST'});msg(await r.text());settingsLoaded=false;axisLoaded=false;refresh();refreshSsid()}catch(e){msg('Wizard connection failed — check the controller display')}}
async function refreshSsid(){try{const r=await fetch('/api/status',{cache:'no-store'});const d=await r.json();$('apSsid').textContent=d.ssid;if(!axisLoaded){$('rotationAxis').value=d.rotationAxis;axisLoaded=true;}if(!advancedLoaded){$('wheeliePattern').value=d.wheeliePattern;$('warningPattern').value=d.warningPattern;$('warningBrightness').value=d.warningBrightness;$('warningBrightnessOut').textContent=d.warningBrightness+'%';$('warningAngleFirmware').value=d.warningAngle;$('warningResetFirmware').value=d.warningReset;$('warningRateFirmware').value=d.warningRate;advancedLoaded=true;}}catch(e){}}
async function testOutput(level){const r=await fetch('/api/output?token='+encodeURIComponent(token)+'&level='+level,{method:'POST'});msg(await r.text());refresh();}
async function saveWifiPassword(){const p=$('wifiPassword').value;if(p.length<8||p.length>63){msg('Wi-Fi password must be 8-63 characters');return;}if(!confirm('Change Wi-Fi password? You will need to reconnect.'))return;const body=new URLSearchParams({password:p});try{const r=await fetch('/api/wifi?token='+encodeURIComponent(token),{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});msg(await r.text());$('wifiPassword').value='';}catch(e){msg('Password saved; reconnect to the controller AP');}}
async function generateWifiPassword(){if(!confirm('Generate a unique 16-character password for this controller? Copy it before reconnecting.'))return;const body=new URLSearchParams({generate:'1'});try{const r=await fetch('/api/wifi?token='+encodeURIComponent(token),{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body}),text=await r.text();alert(text);msg('Password generated; reconnect to the controller AP')}catch(e){msg('Password generated; reconnect and use the password shown')}}
function exportSettings(){const ids=['angleMode','rotationAxis','adaptiveTau','freezeRate','trigger','reset','hold','minon','brightness','fade','bootMode','rideLogging','wheeliePattern','warningPattern','warningBrightness','warningAngleFirmware','warningResetFirmware','warningRateFirmware','otaChannel'];const controller={};ids.forEach(id=>controller[id]=$(id).value);const localKeys=['wheelieTheme','wheelieBikeColor','wheeliePitchTape','wheelieLeanGauge','wheelieEffectMode','wheelieEffectColor','warningEffectMode','warningEffectAngle','warningEffectColor'];const appearance={};localKeys.forEach(key=>{try{appearance[key]=localStorage.getItem(key)}catch(e){}});const blob=new Blob([JSON.stringify({format:'wheelie-controller-settings',version:1,controller,appearance},null,2)],{type:'application/json'}),a=document.createElement('a');a.href=URL.createObjectURL(blob);a.download='wheelie-controller-settings.json';a.click();URL.revokeObjectURL(a.href);msg('Settings exported')}
function rideDuration(ms){const total=Math.round(ms/1000),minutes=Math.floor(total/60),seconds=total%60;return minutes+'m '+String(seconds).padStart(2,'0')+'s'}
async function downloadRide(id,format){const path=format==='csv'?'/api/ride/csv':'/api/ride/report';try{const r=await fetch(path+'?id='+encodeURIComponent(id));if(!r.ok)throw new Error(await r.text());const blob=await r.blob(),a=document.createElement('a');a.href=URL.createObjectURL(blob);a.download='wheelie-ride-'+id+(format==='csv'?'.csv':'-report.html');a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000);msg('Ride '+id+' '+format.toUpperCase()+' downloaded')}catch(e){msg('Download failed: '+e.message)}}
async function refreshRideReports(){try{const r=await fetch('/api/rides',{cache:'no-store'}),d=await r.json(),list=$('rideReports');if(!d.available){list.innerHTML='<div class="rideItem note">Ride storage is unavailable.</div>';return}if(!d.sessions.length){list.innerHTML='<div class="rideItem note">'+(d.enabled?'No completed rides yet. Arm the controller to begin a session.':'Logging is disabled. Enable it under Output &amp; Startup, then save settings.')+'</div>';return}list.innerHTML=d.sessions.map(s=>'<div class="rideItem"><div class="rideTop"><strong>Ride #'+s.id+(s.active?' · RECORDING':'')+'</strong><span>'+rideDuration(s.durationMs)+'</span></div><div class="rideMeta">'+s.samples.toLocaleString()+' samples · '+s.wheelies+' wheelies · '+s.peakPitch.toFixed(1)+'° peak pitch · '+s.peakRoll.toFixed(1)+'° peak lean</div>'+(s.active?'<div class="rideMeta">Return to STANDBY to finalize and download this ride.</div>':'<div class="rideActions"><button onclick="downloadRide('+s.id+',\'report\')">Ride Report</button><button onclick="downloadRide('+s.id+',\'csv\')">CSV Data</button></div>')+'</div>').join('')}catch(e){$('rideReports').innerHTML='<div class="rideItem note">Unable to load ride reports.</div>'}}
$('settingsFile').addEventListener('change',async e=>{const file=e.target.files[0];if(!file)return;try{const data=JSON.parse(await file.text());if(data.format!=='wheelie-controller-settings'||!data.controller)throw new Error('Unsupported settings file');Object.entries(data.controller).forEach(([id,value])=>{if($(id))$(id).value=value});if(data.appearance)Object.entries(data.appearance).forEach(([key,value])=>{if(value!==null)localStorage.setItem(key,value)});$('brightnessOut').textContent=$('brightness').value+'%';$('fadeOut').textContent=$('fade').value+' ms';$('warningBrightnessOut').textContent=$('warningBrightness').value+'%';if(confirm('Settings loaded. Apply them to the controller now?'))await saveSettings();else msg('Settings loaded for review; use Save Settings to apply')}catch(err){msg('Import failed: '+err.message)}e.target.value=''});
async function rollbackFirmware(){if(!confirm('Reboot into the previous firmware image?'))return;try{const r=await fetch('/api/rollback?token='+encodeURIComponent(token),{method:'POST'});msg(await r.text())}catch(e){msg('Controller is rebooting or rollback failed')}}
async function uploadFirmware(){const f=$('firmwareFile').files[0];if(!f){msg('Choose a signed .wctrl package first');return;}if(!f.name.toLowerCase().endsWith('.wctrl')){msg('Unsigned .bin files are not accepted');return;}if(!confirm('Verify and install '+f.name+'?'))return;const form=new FormData();form.append('update',f,f.name);msg('Verifying signed firmware — do not remove power...');try{const r=await fetch('/api/update?token='+encodeURIComponent(token),{method:'POST',body:form});msg(await r.text());}catch(e){msg('Controller rebooting — reconnect shortly');}}
refresh();refreshSsid();refreshRideReports();setInterval(refresh,500);setInterval(refreshSsid,5000);setInterval(refreshRideReports,5000);
</script></body></html>
)rawliteral";

// =====================================================
// WEB API
// =====================================================

void handleRoot() {
    server.send_P(200, "text/html", DASHBOARD_HTML);
}

void handleSettingsPage() {
    server.send_P(200, "text/html", SETTINGS_HTML);
}

void handleCaptivePortalRedirect() {
    // Give connectivity assistants a canonical local destination. In
    // particular, Windows NCSI may otherwise continue from its failed probe
    // to msftconnecttest.com/redirect and then open the MSN portal.
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.sendHeader("Cache-Control", "no-store");
    server.send(302, "text/plain", "Open Wheelie Controller");
}

void handleStatus() {
    int outputPercent = (outputBrightness * 100) / 255;
    int brightnessPercent = (settings.brightness * 100) / 255;

    String json = "{";
    json += "\"pitch\":" + String(currentTriggerPitch, 2);
    json += ",\"rawPitch\":" + String(currentAbsolutePitch, 2);
    json += ",\"roll\":" + String(currentRollAngle, 2);
    json += ",\"rollRate\":" + String(currentRollRate, 2);
    json += ",\"baseline\":" + String(adaptiveBaseline, 2);
    json += ",\"gyroRate\":" + String(currentGyroRate, 2);
    json += ",\"gLoad\":" + String(currentGLoad, 3);
    json += ",\"peakAngle\":" + String(highestAngle, 2);
    json += ",\"peakG\":" + String(highestGLoad, 3);
    json += ",\"warningActive\":" + String(highAngleWarningActive ? "true" : "false");
    json += ",\"warningAngle\":" + String(settings.warningAngle, 1);
    json += ",\"warningReset\":" + String(settings.warningResetAngle, 1);
    json += ",\"warningRate\":" + String(settings.warningPitchRateDegSec, 1);
    json += ",\"wheeliePattern\":" + String(static_cast<uint8_t>(settings.wheeliePattern));
    json += ",\"warningPattern\":" + String(static_cast<uint8_t>(settings.warningPattern));
    json += ",\"warningBrightness\":" + String((settings.warningBrightness * 100) / 255);
    json += ",\"eventCount\":" + String(completedWheelieCount);
    json += ",\"activeDuration\":" + String(controllerState == ControllerState::WHEELIE ? millis() - wheelieStartTime : 0);
    json += ",\"lastDuration\":" + String(lastWheelieDurationMs);
    json += ",\"lastPeakAngle\":" + String(lastWheeliePeakAngle, 1);
    json += ",\"lastPeakG\":" + String(lastWheeliePeakG, 2);
    json += ",\"accelX\":" + String(currentAccelX, 3);
    json += ",\"accelY\":" + String(currentAccelY, 3);
    json += ",\"accelZ\":" + String(currentAccelZ, 3);
    json += ",\"baselineFrozen\":" + String(adaptiveBaselineFrozen ? "true" : "false");
    json += ",\"mode\":\"" + String(getModeName()) + "\"";
    json += ",\"state\":\"" + String(getStateName()) + "\"";
    json += ",\"output\":" + String(outputPercent);
    json += ",\"imu\":" + String(imuHealthy ? "true" : "false");
    json += ",\"angleMode\":\"" + String(settings.angleMode == AngleMode::ADAPTIVE ? "adaptive" : "absolute") + "\"";
    json += ",\"rotationAxis\":\"" + String(
        rotationAxis == RotationAxis::X ? "x" :
        (rotationAxis == RotationAxis::Z ? "z" : "y")
    ) + "\"";
    json += ",\"rollAxis\":\"" + String(getAxisJsonName(rollAxis)) + "\"";
    json += ",\"verticalAxis\":\"" + String(getAxisJsonName(verticalAxis)) + "\"";
    json += ",\"orientationConfigured\":" + String(orientationConfigured ? "true" : "false");
    json += ",\"adaptiveTau\":" + String(settings.adaptiveTimeConstantSec, 1);
    json += ",\"freezeRate\":" + String(settings.adaptiveFreezeRateDegSec, 1);
    json += ",\"trigger\":" + String(settings.triggerAngle, 1);
    json += ",\"reset\":" + String(settings.resetAngle, 1);
    json += ",\"hold\":" + String(settings.triggerHoldMs);
    json += ",\"minon\":" + String(settings.minimumOnTimeMs);
    json += ",\"brightness\":" + String(brightnessPercent);
    json += ",\"fade\":" + String(settings.fadeMs);
    json += ",\"bootArmed\":" + String(settings.bootArmed ? "true" : "false");
    json += ",\"apEnabled\":" + String(accessPointEnabled ? "true" : "false");
    json += ",\"ssid\":\"" + wifiApSsid + "\"";
    json += ",\"mdns\":" + String(mdnsHealthy ? "true" : "false");
    json += ",\"dns\":" + String(dnsHealthy ? "true" : "false");
    json += ",\"clients\":" + String(accessPointEnabled ? WiFi.softAPgetStationNum() : 0);
    json += ",\"uptime\":" + String(millis() / 1000);
    json += ",\"firmware\":\"" + String(FIRMWARE_VERSION) + "\"";
    json += ",\"board\":\"" + String(TARGET_BOARD_ID) + "\"";
    json += ",\"chip\":\"" + String(TARGET_CHIP_ID) + "\"";
    json += ",\"buildCommit\":\"" + String(BUILD_COMMIT) + "\"";
    json += ",\"buildDate\":\"" + String(BUILD_DATE) + "\"";
    json += ",\"releaseChannel\":\"" + String(RELEASE_CHANNEL) + "\"";
    json += ",\"otaChannel\":\"" + otaChannel + "\"";
    json += ",\"signedOta\":true";
    json += ",\"rideLoggingEnabled\":" + String(rideLoggingEnabled ? "true" : "false");
    json += ",\"rideLoggingAvailable\":" + String(rideStorageReady ? "true" : "false");
    json += ",\"rideLoggingActive\":" + String(rideSessionActive ? "true" : "false");
    json += ",\"rideSessionId\":" + String(rideSessionActive ? activeRideHeader.sessionId : 0);
    json += ",\"rideSampleCount\":" + String(rideSessionActive ? activeRideHeader.sampleCount : 0);
    json += ",\"rideStorageUsed\":" + String(rideStorageReady ? LittleFS.usedBytes() : 0);
    json += ",\"rideStorageTotal\":" + String(rideStorageReady ? LittleFS.totalBytes() : 0);
    json += ",\"rideSessionLimit\":" + String(RIDE_LOG_MAX_SESSIONS);
    json += ",\"rideSampleRateHz\":" + String(RIDE_LOG_SAMPLE_RATE_HZ);
    json += ",\"calOneGRaw\":" + String(calibrationRestMagnitudeRaw, 4);
    json += ",\"calAccelRms\":" + String(calibrationAccelNoiseRms, 4);
    json += ",\"calGyroRms\":" + String(calibrationGyroNoiseRms, 2);
    json += ",\"calHighVibration\":" + String(calibrationHighVibration ? "true" : "false");
    json += ",\"token\":\"" + writeToken + "\"";
    json += "}";

    server.send(200, "application/json", json);
}

String rideDigestHex(const uint8_t digest[32]) {
    static const char* hex = "0123456789abcdef";
    String result;
    result.reserve(64);
    for (size_t index = 0; index < 32; ++index) {
        result += hex[digest[index] >> 4];
        result += hex[digest[index] & 0x0F];
    }
    return result;
}

uint8_t collectRideHeaders(RideLogHeader headers[RIDE_LOG_MAX_SESSIONS],
                           uint8_t slots[RIDE_LOG_MAX_SESSIONS]) {
    if (rideSessionActive) writeActiveRideHeader();
    uint8_t count = 0;
    for (uint8_t slot = 0; slot < RIDE_LOG_MAX_SESSIONS; ++slot) {
        RideLogHeader header;
        if (readRideHeader(slot, header)) {
            headers[count] = header;
            slots[count] = slot;
            count++;
        }
    }
    for (uint8_t left = 0; left < count; ++left) {
        for (uint8_t right = left + 1; right < count; ++right) {
            if (headers[right].sessionId > headers[left].sessionId) {
                RideLogHeader temporaryHeader = headers[left];
                headers[left] = headers[right];
                headers[right] = temporaryHeader;
                const uint8_t temporarySlot = slots[left];
                slots[left] = slots[right];
                slots[right] = temporarySlot;
            }
        }
    }
    return count;
}

bool findRideSession(uint32_t sessionId, RideLogHeader& header, uint8_t& slot) {
    RideLogHeader headers[RIDE_LOG_MAX_SESSIONS];
    uint8_t slots[RIDE_LOG_MAX_SESSIONS];
    const uint8_t count = collectRideHeaders(headers, slots);
    for (uint8_t index = 0; index < count; ++index) {
        if (headers[index].sessionId == sessionId) {
            header = headers[index];
            slot = slots[index];
            return true;
        }
    }
    return false;
}

void handleRideList() {
    RideLogHeader headers[RIDE_LOG_MAX_SESSIONS];
    uint8_t slots[RIDE_LOG_MAX_SESSIONS];
    const uint8_t count = collectRideHeaders(headers, slots);
    String json;
    json.reserve(1800);
    json = "{\"available\":" + String(rideStorageReady ? "true" : "false");
    json += ",\"enabled\":" + String(rideLoggingEnabled ? "true" : "false");
    json += ",\"active\":" + String(rideSessionActive ? "true" : "false");
    json += ",\"maxSessions\":" + String(RIDE_LOG_MAX_SESSIONS);
    json += ",\"sampleRateHz\":" + String(RIDE_LOG_SAMPLE_RATE_HZ);
    json += ",\"maxSessionMinutes\":" + String(RIDE_LOG_MAX_DURATION_MS / 60000.0f, 1);
    json += ",\"usedBytes\":" + String(rideStorageReady ? LittleFS.usedBytes() : 0);
    json += ",\"totalBytes\":" + String(rideStorageReady ? LittleFS.totalBytes() : 0);
    json += ",\"sessions\":[";
    for (uint8_t index = 0; index < count; ++index) {
        const RideLogHeader& header = headers[index];
        if (index) json += ',';
        json += "{\"id\":" + String(header.sessionId);
        json += ",\"active\":" + String(rideSessionActive && header.sessionId == activeRideHeader.sessionId ? "true" : "false");
        json += ",\"complete\":" + String((header.flags & RIDE_HEADER_COMPLETE) ? "true" : "false");
        json += ",\"recovered\":" + String((header.flags & RIDE_HEADER_RECOVERED) ? "true" : "false");
        json += ",\"capacityReached\":" + String((header.flags & RIDE_HEADER_CAPACITY_REACHED) ? "true" : "false");
        json += ",\"durationMs\":" + String(header.durationMs);
        json += ",\"samples\":" + String(header.sampleCount);
        json += ",\"wheelies\":" + String(header.wheelieCount);
        json += ",\"peakPitch\":" + String(header.peakPitchCentiDeg / 100.0f, 2);
        json += ",\"peakRoll\":" + String(header.peakRollCentiDeg / 100.0f, 2);
        json += ",\"peakGyro\":" + String(header.peakGyroCentiDegSec / 100.0f, 2);
        json += ",\"peakG\":" + String(header.peakGMillig / 1000.0f, 3);
        json += ",\"firmware\":\"" + String(header.firmware) + "\"";
        json += ",\"commit\":\"" + String(header.commit) + "\"";
        json += ",\"channel\":\"" + String(header.channel) + "\"";
        json += ",\"sha256\":\"" + rideDigestHex(header.sampleSha256) + "\"}";
    }
    json += "]}";
    server.send(200, "application/json", json);
}

bool requestedRide(RideLogHeader& header, uint8_t& slot) {
    if (!server.hasArg("id")) {
        server.send(400, "text/plain", "Missing ride session id");
        return false;
    }
    const uint32_t sessionId = static_cast<uint32_t>(server.arg("id").toInt());
    if (sessionId == 0 || !findRideSession(sessionId, header, slot)) {
        server.send(404, "text/plain", "Ride session not found");
        return false;
    }
    if (rideSessionActive && sessionId == activeRideHeader.sessionId) {
        server.send(409, "text/plain", "Finish the ride before downloading its report");
        return false;
    }
    return true;
}

void handleRideCsv() {
    RideLogHeader header;
    uint8_t slot = 0;
    if (!requestedRide(header, slot)) return;
    File file = LittleFS.open(ridePathForSlot(slot), "r");
    if (!file || !file.seek(sizeof(RideLogHeader))) {
        if (file) file.close();
        server.send(500, "text/plain", "Unable to read ride telemetry");
        return;
    }

    const String filename = "wheelie-ride-" + String(header.sessionId) + ".csv";
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("X-Ride-SHA256", rideDigestHex(header.sampleSha256));
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/csv; charset=utf-8", "");
    server.sendContent("# Wheelie Controller Ride Telemetry\r\n");
    server.sendContent("# provenance=DEVICE-RECORDED; certification=none; integrity=SHA-256 sample checksum\r\n");
    server.sendContent("# session_id=" + String(header.sessionId) +
                       "; firmware=" + String(header.firmware) +
                       "; commit=" + String(header.commit) +
                       "; channel=" + String(header.channel) + "\r\n");
    server.sendContent("# sample_sha256=" + rideDigestHex(header.sampleSha256) + "\r\n");
    server.sendContent("elapsed_ms,pitch_deg,raw_pitch_deg,roll_deg,pitch_rate_dps,g_load,baseline_deg,output_percent,imu_ok,state,warning,baseline_frozen\r\n");

    RideTelemetrySample sample;
    char line[240];
    uint32_t samplesSent = 0;
    while (samplesSent < header.sampleCount &&
           file.read(reinterpret_cast<uint8_t*>(&sample), sizeof(sample)) == sizeof(sample)) {
        const char* state = (sample.flags & RIDE_SAMPLE_WHEELIE) ? "WHEELIE" :
                            ((sample.flags & RIDE_SAMPLE_PENDING) ? "PENDING" : "NORMAL");
        snprintf(line, sizeof(line),
                 "%lu,%.2f,%.2f,%.2f,%.2f,%.3f,%.2f,%u,%u,%s,%u,%u\r\n",
                 (unsigned long)sample.elapsedMs,
                 sample.pitchCentiDeg / 100.0f,
                 sample.rawPitchCentiDeg / 100.0f,
                 sample.rollCentiDeg / 100.0f,
                 sample.gyroCentiDegSec / 100.0f,
                 sample.gLoadMillig / 1000.0f,
                 (sample.rawPitchCentiDeg - sample.pitchCentiDeg) / 100.0f,
                 sample.outputPercent,
                 (sample.flags & RIDE_SAMPLE_IMU_OK) ? 1 : 0,
                 state,
                 (sample.flags & RIDE_SAMPLE_WARNING) ? 1 : 0,
                 (sample.flags & RIDE_SAMPLE_BASELINE_FROZEN) ? 1 : 0);
        server.sendContent(line);
        samplesSent++;
        if ((samplesSent % 100) == 0) delay(0);
    }
    file.close();
    server.sendContent("");
}

void handleRideReport() {
    RideLogHeader header;
    uint8_t slot = 0;
    if (!requestedRide(header, slot)) return;
    const String digest = rideDigestHex(header.sampleSha256);
    const String filename = "wheelie-ride-" + String(header.sessionId) + "-report.html";
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
    server.sendHeader("Cache-Control", "no-store");

    String report;
    report.reserve(7000);
    report = "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>Ride Report #" + String(header.sessionId) + "</title><style>";
    report += "body{margin:0;background:#080b10;color:#f4f7fb;font:15px system-ui,-apple-system,sans-serif}.wrap{width:min(760px,100%);margin:auto;padding:28px}.top{display:flex;justify-content:space-between;gap:20px;align-items:start}.kicker{color:#55e6ff;font-size:12px;letter-spacing:.16em}.badge{display:inline-grid;place-items:center;text-align:center;padding:12px 16px;border:2px solid #52df9a;border-radius:14px;color:#8dffc7;background:#102a21;font-weight:900;letter-spacing:.08em;transform:rotate(1deg)}.badge small{display:block;margin-top:4px;font-size:9px;color:#a9c6b8}.card{margin-top:18px;padding:18px;border:1px solid #273449;border-radius:16px;background:#111720}.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:0 22px}.row{display:flex;justify-content:space-between;gap:12px;padding:10px 0;border-bottom:1px solid #273449}.row span{color:#8f9bad}.hash{overflow-wrap:anywhere;font:12px ui-monospace,monospace;color:#9fb4c8}.note{color:#8f9bad;line-height:1.5;font-size:12px}@media print{body{background:white;color:#111}.card{background:white}.badge{color:#14663f;background:white}}@media(max-width:560px){.top{display:block}.badge{margin-top:16px}.grid{grid-template-columns:1fr}}";
    report += "</style></head><body><main class=wrap><div class=top><div><div class=kicker>WHEELIE CONTROLLER</div><h1>Ride Report #" + String(header.sessionId) + "</h1><p>Hardware sensor telemetry captured at " + String(header.sampleRateHz) + " Hz.</p></div><div class=badge>DEVICE-RECORDED<small>SENSOR DATA · SHA-256</small></div></div>";
    report += "<section class=card><h2>Ride summary</h2><div class=grid>";
    report += "<div class=row><span>Duration</span><b>" + String(header.durationMs / 1000.0f, 1) + " s</b></div>";
    report += "<div class=row><span>Samples</span><b>" + String(header.sampleCount) + "</b></div>";
    report += "<div class=row><span>Wheelies</span><b>" + String(header.wheelieCount) + "</b></div>";
    report += "<div class=row><span>Peak pitch</span><b>" + String(header.peakPitchCentiDeg / 100.0f, 1) + "°</b></div>";
    report += "<div class=row><span>Peak lean</span><b>" + String(header.peakRollCentiDeg / 100.0f, 1) + "°</b></div>";
    report += "<div class=row><span>Peak +G</span><b>" + String(header.peakGMillig / 1000.0f, 2) + " g</b></div></div></section>";
    report += "<section class=card><h2>Recorder provenance</h2><div class=grid>";
    report += "<div class=row><span>Firmware</span><b>" + String(header.firmware) + "</b></div>";
    report += "<div class=row><span>Build commit</span><b>" + String(header.commit) + "</b></div>";
    report += "<div class=row><span>Release channel</span><b>" + String(header.channel) + "</b></div>";
    report += "<div class=row><span>Hardware</span><b>" + String(header.board) + "</b></div></div>";
    report += "<p>Sample-stream SHA-256</p><div class=hash>" + digest + "</div></section>";
    report += "<p class=note><b>What the badge means:</b> this report was generated from telemetry recorded by the controller hardware and includes a checksum for detecting accidental changes. It is not a third-party certification and is not yet a cryptographic device signature.</p>";
    report += "<p class=note>Session start is recorded as controller uptime because the controller has no real-time clock. Downloaded <span id=downloaded></span>.</p><script>document.getElementById('downloaded').textContent=new Date().toLocaleString()</script></main></body></html>";
    server.send(200, "text/html; charset=utf-8", report);
}

void handleSettings() {
    if (!requireWriteToken()) return;

    const char* required[] = {
        "angleMode", "rotationAxis", "adaptiveTau", "freezeRate", "trigger", "reset",
        "hold", "minon", "brightness", "fade", "bootMode", "wheeliePattern",
        "warningPattern", "warningBrightness", "warningAngle", "warningReset", "warningRate",
        "otaChannel", "rideLogging"
    };

    for (const char* key : required) {
        if (!server.hasArg(key)) {
            server.send(400, "text/plain", String("Missing setting: ") + key);
            return;
        }
    }

    String angleMode = server.arg("angleMode");
    String newRotationAxis = server.arg("rotationAxis");
    float newAdaptiveTau = server.arg("adaptiveTau").toFloat();
    float newFreezeRate = server.arg("freezeRate").toFloat();
    float newTrigger = server.arg("trigger").toFloat();
    float newReset = server.arg("reset").toFloat();
    uint32_t newHold = (uint32_t)server.arg("hold").toInt();
    uint32_t newMinOn = (uint32_t)server.arg("minon").toInt();
    int newBrightnessPercent = server.arg("brightness").toInt();
    uint32_t newFade = (uint32_t)server.arg("fade").toInt();
    String bootMode = server.arg("bootMode");
    int newWheeliePattern = server.arg("wheeliePattern").toInt();
    int newWarningPattern = server.arg("warningPattern").toInt();
    int newWarningBrightnessPercent = server.arg("warningBrightness").toInt();
    float newWarningAngle = server.arg("warningAngle").toFloat();
    float newWarningReset = server.arg("warningReset").toFloat();
    float newWarningRate = server.arg("warningRate").toFloat();
    String newOtaChannel = server.arg("otaChannel");
    String newRideLogging = server.arg("rideLogging");

    if (angleMode != "absolute" && angleMode != "adaptive") {
        server.send(400, "text/plain", "Invalid angle mode");
        return;
    }

    if (newRotationAxis != "x" && newRotationAxis != "y" && newRotationAxis != "z") {
        server.send(400, "text/plain", "Invalid rotation axis");
        return;
    }
    if (newRotationAxis != getAxisJsonName(rotationAxis)) {
        server.send(409, "text/plain", "Use the mounting wizard to change sensor orientation");
        return;
    }

    if (newAdaptiveTau < 0.5f || newAdaptiveTau > 60.0f) {
        server.send(400, "text/plain", "Adaptive time must be 0.5-60 seconds");
        return;
    }

    if (newFreezeRate < 1.0f || newFreezeRate > 100.0f) {
        server.send(400, "text/plain", "Freeze rate must be 1-100 deg/sec");
        return;
    }

    if (newTrigger < 5.0f || newTrigger > 70.0f) {
        server.send(400, "text/plain", "Trigger must be 5-70 degrees");
        return;
    }

    if (newReset < 0.0f || newReset >= newTrigger) {
        server.send(400, "text/plain", "Reset must be below trigger");
        return;
    }

    if (newHold > 5000) {
        server.send(400, "text/plain", "Trigger hold must be 0-5000 ms");
        return;
    }

    if (newMinOn > 15000) {
        server.send(400, "text/plain", "Minimum ON must be 0-15000 ms");
        return;
    }

    if (newBrightnessPercent < 1 || newBrightnessPercent > 100) {
        server.send(400, "text/plain", "Brightness must be 1-100%");
        return;
    }

    if (newFade > 3000) {
        server.send(400, "text/plain", "Fade must be 0-3000 ms");
        return;
    }

    if (bootMode != "armed" && bootMode != "standby") {
        server.send(400, "text/plain", "Invalid boot mode");
        return;
    }
    if (newOtaChannel != "stable" && newOtaChannel != "testing") {
        server.send(400, "text/plain", "OTA channel must be stable or testing");
        return;
    }
    if (newRideLogging != "enabled" && newRideLogging != "disabled") {
        server.send(400, "text/plain", "Ride logging must be enabled or disabled");
        return;
    }

    if (newWheeliePattern < 0 || newWheeliePattern > 4 ||
        newWarningPattern < 0 || newWarningPattern > 4) {
        server.send(400, "text/plain", "Invalid light pattern");
        return;
    }
    if (newWarningBrightnessPercent < 1 || newWarningBrightnessPercent > 100 ||
        newWarningAngle < 5.0f || newWarningAngle > 85.0f ||
        newWarningReset < 0.0f || newWarningReset >= newWarningAngle ||
        newWarningRate < 0.0f || newWarningRate > 250.0f) {
        server.send(400, "text/plain", "Invalid warning configuration");
        return;
    }

    settings.angleMode = angleMode == "adaptive" ? AngleMode::ADAPTIVE : AngleMode::ABSOLUTE;
    settings.adaptiveTimeConstantSec = newAdaptiveTau;
    settings.adaptiveFreezeRateDegSec = newFreezeRate;
    settings.triggerAngle = newTrigger;
    settings.resetAngle = newReset;
    settings.triggerHoldMs = newHold;
    settings.minimumOnTimeMs = newMinOn;
    settings.brightness = (uint8_t)map(newBrightnessPercent, 1, 100, 3, 255);
    settings.fadeMs = newFade;
    settings.bootArmed = bootMode == "armed";
    settings.wheeliePattern = static_cast<LightPattern>(newWheeliePattern);
    settings.warningPattern = static_cast<LightPattern>(newWarningPattern);
    settings.warningBrightness = (uint8_t)map(newWarningBrightnessPercent, 1, 100, 3, 255);
    settings.warningAngle = newWarningAngle;
    settings.warningResetAngle = newWarningReset;
    settings.warningPitchRateDegSec = newWarningRate;
    otaChannel = newOtaChannel;
    const bool wasRideLoggingEnabled = rideLoggingEnabled;
    rideLoggingEnabled = newRideLogging == "enabled";

    controllerState = ControllerState::NORMAL;
    forceOutputOff();
    resetAdaptiveBaselineToCurrent();
    saveSettings();
    if (wasRideLoggingEnabled && !rideLoggingEnabled) {
        finishRideSession();
    } else if (!wasRideLoggingEnabled && rideLoggingEnabled &&
               operatingMode == OperatingMode::ARMED) {
        startRideSession();
    }

    oled.clearDisplay();
    displayDirty = true;
    rotateWriteToken();
    server.send(200, "text/plain", "Settings saved");
}

void handleMode() {
    if (!requireWriteToken()) return;

    if (!server.hasArg("mode")) {
        server.send(400, "text/plain", "Missing mode");
        return;
    }

    String mode = server.arg("mode");

    if (mode == "armed") {
        if (!imuHealthy) {
            server.send(409, "text/plain", "Cannot arm: IMU fault");
            return;
        }
        setOperatingMode(OperatingMode::ARMED);
        server.send(200, "text/plain", "Controller ARMED");
    } else if (mode == "standby") {
        setOperatingMode(OperatingMode::STANDBY);
        server.send(200, "text/plain", "Controller STANDBY");
    } else {
        server.send(400, "text/plain", "Invalid mode");
    }
}

void handleCalibration() {
    if (!requireWriteToken()) return;

    OperatingMode previousMode = operatingMode;
    setOperatingMode(OperatingMode::STANDBY);

    bool success = calibrateMPU();

    if (success && previousMode == OperatingMode::ARMED) {
        setOperatingMode(OperatingMode::ARMED);
    } else {
        setOperatingMode(OperatingMode::STANDBY);
    }

    server.send(
        success ? 200 : 500,
        "text/plain",
        success ? "Calibration complete" : "Calibration failed — controller left in STANDBY"
    );
}

void handleManualOutput() {
    if (!requireWriteToken()) return;

    if (operatingMode != OperatingMode::STANDBY) {
        server.send(409, "text/plain", "Manual output is only allowed in STANDBY");
        return;
    }

    if (!server.hasArg("level")) {
        server.send(400, "text/plain", "Missing output level");
        return;
    }

    int level = server.arg("level").toInt();

    if (level < 0 || level > 100) {
        server.send(400, "text/plain", "Output level must be 0-100%");
        return;
    }

    controllerState = ControllerState::NORMAL;

    if (level == 0) {
        manualTestActive = false;
        setOutputTarget(0);
        server.send(200, "text/plain", "Manual output OFF");
    } else {
        manualTestActive = true;
        manualTestStartMs = millis();
        uint8_t value = (uint8_t)map(level, 1, 100, 3, 255);
        setOutputTarget(value);
        server.send(200, "text/plain", String("Manual output ") + level + "% for up to 10 seconds");
    }

    displayDirty = true;
}

void handlePeakReset() {
    if (!requireWriteToken()) return;

    if (!server.hasArg("kind")) {
        server.send(400, "text/plain", "Missing peak type");
        return;
    }

    String kind = server.arg("kind");
    if (kind == "angle") {
        highestAngle = max(0.0f, currentTriggerPitch);
        server.send(200, "text/plain", "Highest angle reset");
    } else if (kind == "g") {
        highestGLoad = currentGLoad;
        server.send(200, "text/plain", "Highest +G reset");
    } else {
        server.send(400, "text/plain", "Invalid peak type");
    }
}

void handleWiFiPassword() {
    if (!requireWriteToken()) return;

    const bool generatePassword = server.hasArg("generate") && server.arg("generate") == "1";
    if (!generatePassword && !server.hasArg("password")) {
        server.send(400, "text/plain", "Missing password");
        return;
    }

    String newPassword = generatePassword ? generateUniqueWiFiPassword() : server.arg("password");
    if (newPassword.length() < 8 || newPassword.length() > 63) {
        server.send(400, "text/plain", "Wi-Fi password must be 8-63 characters");
        return;
    }

    if (newPassword == wifiApPassword) {
        server.send(200, "text/plain", "Wi-Fi password unchanged");
        return;
    }

    wifiApPassword = newPassword;
    saveSettings();
    rotateWriteToken();

    if (accessPointEnabled) {
        scheduleAccessPointRestart(1200);
        server.send(200, "text/plain", generatePassword ?
            String("Generated device password: ") + newPassword +
                "\nCopy it now. The access point is restarting." :
            "Password saved. Access point restarting — reconnect with the new password.");
    } else {
        server.send(200, "text/plain", generatePassword ?
            String("Generated device password: ") + newPassword +
                "\nCopy it now. It will be used when the AP is enabled." :
            "Password saved. It will be used next time the AP is enabled.");
    }
}

void resetFirmwareUploadState() {
    if (firmwareUpload.hashInitialized) {
        mbedtls_sha256_free(&firmwareUpload.hashContext);
    }
    if (firmwareUpload.updateStarted && Update.isRunning()) {
        Update.abort();
    }
    firmwareUpload = FirmwareUploadState{};
}

void failFirmwareUpload(const String& reason) {
    if (firmwareUpload.failed) return;
    firmwareUpload.failed = true;
    firmwareUpload.error = reason;
    if (firmwareUpload.updateStarted && Update.isRunning()) {
        Update.abort();
    }
    firmwareUpload.updateStarted = false;
    if (firmwareUpload.hashInitialized) {
        mbedtls_sha256_free(&firmwareUpload.hashContext);
        firmwareUpload.hashInitialized = false;
    }
    Serial.printf("OTA rejected: %s\n", reason.c_str());
}

String firmwareManifestValue(const char* key) {
    const String manifest(firmwareUpload.manifest);
    const String prefix = String(key) + "=";
    int start = manifest.indexOf(prefix);
    while (start >= 0 && start > 0 && manifest[start - 1] != '\n') {
        start = manifest.indexOf(prefix, start + 1);
    }
    if (start < 0) return "";
    start += prefix.length();
    int end = manifest.indexOf('\n', start);
    if (end < 0) return "";
    return manifest.substring(start, end);
}

bool isLowerHexSha256(const String& value) {
    if (value.length() != 64) return false;
    for (size_t index = 0; index < value.length(); ++index) {
        const char character = value[index];
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) return false;
    }
    return true;
}

String digestToHex(const uint8_t digest[32]) {
    static constexpr char hexadecimal[] = "0123456789abcdef";
    char output[65];
    for (size_t index = 0; index < 32; ++index) {
        output[index * 2] = hexadecimal[digest[index] >> 4];
        output[index * 2 + 1] = hexadecimal[digest[index] & 0x0F];
    }
    output[64] = '\0';
    return String(output);
}

bool verifyFirmwareManifestSignature() {
    uint8_t digest[32];
    mbedtls_sha256_context manifestHash;
    mbedtls_sha256_init(&manifestHash);
    if (mbedtls_sha256_starts(&manifestHash, 0) != 0 ||
        mbedtls_sha256_update(
            &manifestHash,
            reinterpret_cast<const uint8_t*>(firmwareUpload.manifest),
            firmwareUpload.manifestExpected) != 0 ||
        mbedtls_sha256_finish(&manifestHash, digest) != 0) {
        mbedtls_sha256_free(&manifestHash);
        return false;
    }
    mbedtls_sha256_free(&manifestHash);

    mbedtls_pk_context publicKey;
    mbedtls_pk_init(&publicKey);
    const int parseResult = mbedtls_pk_parse_public_key(
        &publicKey,
        reinterpret_cast<const uint8_t*>(FIRMWARE_SIGNING_PUBLIC_KEY_PEM),
        sizeof(FIRMWARE_SIGNING_PUBLIC_KEY_PEM));
    const int verifyResult = parseResult == 0 ? mbedtls_pk_verify(
        &publicKey,
        MBEDTLS_MD_SHA256,
        digest,
        sizeof(digest),
        firmwareUpload.signature,
        firmwareUpload.signatureExpected) : parseResult;
    mbedtls_pk_free(&publicKey);
    return verifyResult == 0;
}

bool prepareFirmwarePayload() {
    firmwareUpload.manifest[firmwareUpload.manifestExpected] = '\0';
    if (!verifyFirmwareManifestSignature()) {
        failFirmwareUpload("Manifest signature is invalid");
        return false;
    }

    const String format = firmwareManifestValue("format");
    const String board = firmwareManifestValue("board");
    const String chip = firmwareManifestValue("chip");
    firmwareUpload.packageVersion = firmwareManifestValue("version");
    const String channel = firmwareManifestValue("channel");
    firmwareUpload.packageCommit = firmwareManifestValue("commit");
    firmwareUpload.packageBuilt = firmwareManifestValue("built");
    const String size = firmwareManifestValue("size");
    firmwareUpload.expectedSha256 = firmwareManifestValue("sha256");

    if (format != "1") {
        failFirmwareUpload("Unsupported package format");
        return false;
    }
    if (!isFirmwareMetadataCompatible(
            board.c_str(), channel.c_str(), TARGET_BOARD_ID, otaChannel.c_str()) ||
        chip != TARGET_CHIP_ID) {
        failFirmwareUpload(
            String("Package requires board ") + board + " / " + chip +
            " on " + channel + " channel; this device is " + TARGET_BOARD_ID +
            " / " + TARGET_CHIP_ID + " on " + otaChannel);
        return false;
    }
    if ((uint32_t)size.toInt() != firmwareUpload.firmwareExpected ||
        !isLowerHexSha256(firmwareUpload.expectedSha256) ||
        firmwareUpload.packageVersion.length() == 0 ||
        firmwareUpload.packageCommit.length() == 0 ||
        firmwareUpload.packageBuilt.length() == 0) {
        failFirmwareUpload("Signed manifest metadata is incomplete or inconsistent");
        return false;
    }
    if (!Update.begin(firmwareUpload.firmwareExpected)) {
        Update.printError(Serial);
        failFirmwareUpload("Unable to reserve the OTA partition");
        return false;
    }
    firmwareUpload.updateStarted = true;
    mbedtls_sha256_init(&firmwareUpload.hashContext);
    firmwareUpload.hashInitialized = true;
    if (mbedtls_sha256_starts(&firmwareUpload.hashContext, 0) != 0) {
        failFirmwareUpload("Unable to initialize firmware SHA-256");
        return false;
    }
    return true;
}

void parseFirmwarePackageHeader() {
    if (!hasFirmwarePackageMagic(
            firmwareUpload.header, firmwareUpload.headerReceived)) {
        failFirmwareUpload("Unsigned .bin files are not accepted; choose a signed .wctrl package");
        return;
    }
    firmwareUpload.manifestExpected = readLittleEndian16(firmwareUpload.header + 8);
    firmwareUpload.signatureExpected = readLittleEndian16(firmwareUpload.header + 10);
    firmwareUpload.firmwareExpected = readLittleEndian32(firmwareUpload.header + 12);
    if (firmwareUpload.manifestExpected == 0 ||
        firmwareUpload.manifestExpected > FIRMWARE_MANIFEST_MAX_SIZE ||
        firmwareUpload.signatureExpected == 0 ||
        firmwareUpload.signatureExpected > FIRMWARE_SIGNATURE_MAX_SIZE ||
        firmwareUpload.firmwareExpected == 0) {
        failFirmwareUpload("Signed package header is invalid");
    }
}

void processFirmwarePackageBytes(uint8_t* data, size_t size) {
    size_t offset = 0;
    while (offset < size && !firmwareUpload.failed) {
        if (firmwareUpload.headerReceived < FIRMWARE_PACKAGE_HEADER_SIZE) {
            const size_t count = min(
                size - offset,
                FIRMWARE_PACKAGE_HEADER_SIZE - firmwareUpload.headerReceived);
            memcpy(firmwareUpload.header + firmwareUpload.headerReceived, data + offset, count);
            firmwareUpload.headerReceived += count;
            offset += count;
            if (firmwareUpload.headerReceived == FIRMWARE_PACKAGE_HEADER_SIZE) {
                parseFirmwarePackageHeader();
            }
            continue;
        }
        if (firmwareUpload.manifestReceived < firmwareUpload.manifestExpected) {
            const size_t count = min(
                size - offset,
                firmwareUpload.manifestExpected - firmwareUpload.manifestReceived);
            memcpy(firmwareUpload.manifest + firmwareUpload.manifestReceived, data + offset, count);
            firmwareUpload.manifestReceived += count;
            offset += count;
            continue;
        }
        if (firmwareUpload.signatureReceived < firmwareUpload.signatureExpected) {
            const size_t count = min(
                size - offset,
                firmwareUpload.signatureExpected - firmwareUpload.signatureReceived);
            memcpy(firmwareUpload.signature + firmwareUpload.signatureReceived, data + offset, count);
            firmwareUpload.signatureReceived += count;
            offset += count;
            if (firmwareUpload.signatureReceived == firmwareUpload.signatureExpected &&
                !prepareFirmwarePayload()) return;
            continue;
        }
        if (firmwareUpload.firmwareReceived < firmwareUpload.firmwareExpected) {
            const size_t count = min(
                size - offset,
                (size_t)(firmwareUpload.firmwareExpected - firmwareUpload.firmwareReceived));
            if (mbedtls_sha256_update(&firmwareUpload.hashContext, data + offset, count) != 0 ||
                Update.write(data + offset, count) != count) {
                Update.printError(Serial);
                failFirmwareUpload("Firmware write failed");
                return;
            }
            firmwareUpload.firmwareReceived += count;
            offset += count;
            continue;
        }
        failFirmwareUpload("Signed package contains unexpected trailing data");
    }
}

void finishFirmwarePackage() {
    if (firmwareUpload.failed) return;
    if (!firmwareUpload.updateStarted ||
        firmwareUpload.firmwareReceived != firmwareUpload.firmwareExpected) {
        failFirmwareUpload("Signed package ended before the complete firmware image arrived");
        return;
    }
    uint8_t digest[32];
    if (mbedtls_sha256_finish(&firmwareUpload.hashContext, digest) != 0) {
        failFirmwareUpload("Unable to finish firmware SHA-256");
        return;
    }
    mbedtls_sha256_free(&firmwareUpload.hashContext);
    firmwareUpload.hashInitialized = false;
    if (digestToHex(digest) != firmwareUpload.expectedSha256) {
        failFirmwareUpload("Firmware SHA-256 does not match the signed manifest");
        return;
    }
    if (!Update.end(false)) {
        Update.printError(Serial);
        failFirmwareUpload("Firmware image validation failed");
        return;
    }
    firmwareUpload.updateStarted = false;
    firmwareUpload.complete = true;
    Serial.printf(
        "Signed OTA verified: %s %s commit %s (%u bytes)\n",
        otaChannel.c_str(), firmwareUpload.packageVersion.c_str(),
        firmwareUpload.packageCommit.c_str(), firmwareUpload.firmwareReceived);
}

void handleFirmwareUpdateFinished() {
    const bool success = firmwareUpload.authorized && firmwareUpload.complete &&
        !firmwareUpload.failed;
    const int status = firmwareUpload.rateLimited ? 429 :
        (!firmwareUpload.authorized ? 403 : (success ? 200 : 400));
    const String response = success ?
        String("Signed firmware ") + firmwareUpload.packageVersion +
            " verified. Rebooting..." :
        (firmwareUpload.error.length() > 0 ? firmwareUpload.error : "Firmware update failed");
    if (firmwareUpload.rateLimited) server.sendHeader("Retry-After", "30");
    server.sendHeader("Connection", "close");
    server.send(status, "text/plain", response);
    if (success) {
        delay(400);
        ESP.restart();
    }
}

void handleFirmwareUpload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        resetFirmwareUploadState();
        firmwareUpload.rateLimited = !consumeWriteRequestLimit();
        firmwareUpload.authorized = !firmwareUpload.rateLimited && validWriteToken();
        if (!firmwareUpload.authorized) {
            firmwareUpload.error = firmwareUpload.rateLimited ?
                "Too many write requests — retry in 30 seconds" : "Invalid write token";
            return;
        }
        Serial.printf("Signed OTA upload: %s\n", upload.filename.c_str());
        setOperatingMode(OperatingMode::STANDBY);
        forceOutputOff();
    } else if (!firmwareUpload.authorized) {
        return;
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        processFirmwarePackageBytes(upload.buf, upload.currentSize);
    } else if (upload.status == UPLOAD_FILE_END) {
        finishFirmwarePackage();
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        failFirmwareUpload("Firmware upload aborted");
    }
}

void handleOrientationWizard() {
    if (!requireWriteToken()) return;

    setOperatingMode(OperatingMode::STANDBY);
    forceOutputOff();
    const bool orientationSuccess = runOrientationWizard();
    bool calibrationSuccess = false;

    if (orientationSuccess) {
        for (uint8_t seconds = INITIAL_CALIBRATION_DELAY_SECONDS; seconds > 0; --seconds) {
            char countdown[17];
            snprintf(countdown, sizeof(countdown), "Starting in %u...", seconds);
            drawCalibrationScreen(countdown);
            delay(1000);
        }
        calibrationSuccess = calibrateMPU();
    }

    setOperatingMode(OperatingMode::STANDBY);
    lastMicros = micros();
    if (orientationSuccess) server.sendHeader("X-Write-Token", writeToken);
    server.send(
        orientationSuccess && calibrationSuccess ? 200 : 500,
        "text/plain",
        !orientationSuccess ? "Orientation not detected — previous setup preserved" :
        (calibrationSuccess ? "Orientation saved and calibrated; controller left in STANDBY" :
         "Orientation saved, but calibration failed — controller left in STANDBY")
    );
}

void handleFirmwareRollback() {
    if (!requireWriteToken()) return;

    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* alternate = esp_ota_get_next_update_partition(running);
    if (alternate == nullptr || alternate == running) {
        server.send(409, "text/plain", "No alternate OTA partition is available");
        return;
    }

    esp_app_desc_t description;
    if (esp_ota_get_partition_description(alternate, &description) != ESP_OK) {
        server.send(409, "text/plain", "No valid previous firmware image is available");
        return;
    }

    if (esp_ota_set_boot_partition(alternate) != ESP_OK) {
        server.send(500, "text/plain", "Unable to select the previous firmware image");
        return;
    }

    setOperatingMode(OperatingMode::STANDBY);
    forceOutputOff();
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", String("Returning to firmware ") + description.version + ". Rebooting...");
    delay(400);
    ESP.restart();
}

// =====================================================
// NETWORK
// =====================================================

void registerWebRoutes() {
    if (webRoutesRegistered) {
        return;
    }

    server.on("/", HTTP_GET, handleRoot);
    server.on("/settings", HTTP_GET, handleSettingsPage);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/rides", HTTP_GET, handleRideList);
    server.on("/api/ride/report", HTTP_GET, handleRideReport);
    server.on("/api/ride/csv", HTTP_GET, handleRideCsv);
    server.on("/api/settings", HTTP_POST, handleSettings);
    server.on("/api/mode", HTTP_POST, handleMode);
    server.on("/api/calibrate", HTTP_POST, handleCalibration);
    server.on("/api/orientation", HTTP_POST, handleOrientationWizard);
    server.on("/api/output", HTTP_POST, handleManualOutput);
    server.on("/api/peak/reset", HTTP_POST, handlePeakReset);
    server.on("/api/wifi", HTTP_POST, handleWiFiPassword);
    server.on("/api/update", HTTP_POST, handleFirmwareUpdateFinished, handleFirmwareUpload);
    server.on("/api/rollback", HTTP_POST, handleFirmwareRollback);

    // Common captive-portal probes must redirect to our actual local origin.
    // Serving dashboard HTML at a Microsoft probe URL can cause Windows to
    // continue into its MSN fallback rather than remain on the controller.
    server.on("/generate_204", HTTP_GET, handleCaptivePortalRedirect);
    server.on("/hotspot-detect.html", HTTP_GET, handleCaptivePortalRedirect);
    server.on("/library/test/success.html", HTTP_GET, handleCaptivePortalRedirect);
    server.on("/connecttest.txt", HTTP_GET, handleCaptivePortalRedirect);
    server.on("/ncsi.txt", HTTP_GET, handleCaptivePortalRedirect);
    server.on("/redirect", HTTP_GET, handleCaptivePortalRedirect);
    server.on("/fwlink", HTTP_GET, handleCaptivePortalRedirect);

    server.onNotFound([]() {
        server.sendHeader("Location", "http://192.168.4.1/", true);
        server.send(302, "text/plain", "");
    });

    webRoutesRegistered = true;
}

void startAccessPoint() {
    if (accessPointEnabled) {
        return;
    }

    Serial.println("Starting Wi-Fi AP...");

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
    WiFi.softAPsetHostname(MDNS_HOSTNAME);

    wifiHealthy = WiFi.softAP(wifiApSsid.c_str(), wifiApPassword.c_str());

    if (!wifiHealthy) {
        accessPointEnabled = false;
        Serial.println("ERROR: Wi-Fi AP failed");
        displayDirty = true;
        return;
    }

    accessPointEnabled = true;
    delay(100);

    // Captive DNS resolves ordinary hostnames to the controller even on
    // clients that do not cooperate with .local/mDNS.
    dnsHealthy = dnsServer.start(DNS_PORT, "*", AP_IP);

    // mDNS remains the preferred friendly wheelie.local name. Give the AP
    // interface a moment and retry startup because some ESP32-S3/core
    // combinations are timing-sensitive immediately after softAP().
    mdnsHealthy = false;
    for (uint8_t attempt = 0; attempt < 3 && !mdnsHealthy; ++attempt) {
        mdnsHealthy = MDNS.begin(MDNS_HOSTNAME);
        if (!mdnsHealthy) delay(120);
    }
    if (mdnsHealthy) {
        MDNS.addService("http", "tcp", 80);
    }

    server.begin();

    Serial.printf("SSID: %s\n", wifiApSsid.c_str());
    Serial.printf("IP: %s\n", WiFi.softAPIP().toString().c_str());
    Serial.printf("mDNS: %s\n", mdnsHealthy ? "http://wheelie.local" : "unavailable");
    Serial.printf("Captive DNS: %s\n", dnsHealthy ? "ON" : "FAILED");
    Serial.println("HTTP server started");

    displayDirty = true;
}

void stopAccessPoint() {
    if (!accessPointEnabled) {
        return;
    }

    // Stop accepting HTTP/DNS clients before dropping the AP.
    server.stop();
    dnsServer.stop();
    dnsHealthy = false;

    if (mdnsHealthy) {
        MDNS.end();
    }

    mdnsHealthy = false;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);

    wifiHealthy = false;
    accessPointEnabled = false;

    Serial.println("Wi-Fi AP -> OFF");
    displayDirty = true;
}

void scheduleAccessPointRestart(unsigned long delayMs) {
    accessPointRestartPending = true;
    accessPointRestartAtMs = millis() + delayMs;
}

void updateAccessPointRestart() {
    if (!accessPointRestartPending) return;
    if ((long)(millis() - accessPointRestartAtMs) < 0) return;

    accessPointRestartPending = false;
    stopAccessPoint();
    delay(75);
    startAccessPoint();
}

void toggleAccessPointFromButton() {
    if (accessPointEnabled) {
        stopAccessPoint();
    } else {
        startAccessPoint();
        Serial.println(accessPointEnabled ? "Wi-Fi AP -> ON" : "Wi-Fi AP failed to start");
    }

    oled.clearDisplay();
    displayDirty = true;
}

void initializeNetwork() {
    registerWebRoutes();
    startAccessPoint();
}

// =====================================================
// IMU HEALTH / MAIN SENSOR UPDATE
// =====================================================

bool updateIMUAndPitch(float& dt) {
    MPUData data;

    if (!readMPU(data)) {
        imuFailureCount++;
        imuRecoveryCount = 0;

        if (imuFailureCount >= IMU_FAILURE_LIMIT) {
            if (imuHealthy) {
                Serial.println("ERROR: MPU6050 communication lost");
            }
            imuHealthy = false;
            controllerState = ControllerState::NORMAL;
            forceOutputOff();
            displayDirty = true;
        }
        return false;
    }

    imuFailureCount = 0;

    if (!imuHealthy) {
        imuRecoveryCount++;
        if (imuRecoveryCount >= IMU_RECOVERY_LIMIT) {
            imuHealthy = true;
            imuRecoveryCount = IMU_RECOVERY_LIMIT;
            Serial.println("MPU6050 communication restored");
            displayDirty = true;
        }
    } else {
        imuRecoveryCount = IMU_RECOVERY_LIMIT;
    }

    unsigned long nowMicros = micros();
    dt = (nowMicros - lastMicros) / 1000000.0f;
    lastMicros = nowMicros;

    if (dt <= 0.0f || dt > 0.1f) {
        dt = 0.01f;
    }

    // Normalize the accelerometer so the calibrated stationary magnitude
    // is 1.00 g even if this particular MPU clone read ~1.12 g at rest.
    currentAccelX = data.ax * accelScaleCorrection;
    currentAccelY = data.ay * accelScaleCorrection;
    currentAccelZ = data.az * accelScaleCorrection;

    // Slowly estimate the gravity vector in sensor coordinates. This tracks
    // ordinary changes in road/bike attitude, while a launch or impact moves
    // much faster than the estimator and therefore remains in the residual.
    float gravityAlpha = dt / (GRAVITY_TRACK_TIME_SEC + dt);
    gravityAlpha = constrain(gravityAlpha, 0.0f, 1.0f);
    gravityEstimateX += (currentAccelX - gravityEstimateX) * gravityAlpha;
    gravityEstimateY += (currentAccelY - gravityEstimateY) * gravityAlpha;
    gravityEstimateZ += (currentAccelZ - gravityEstimateZ) * gravityAlpha;

    float linearX = currentAccelX - gravityEstimateX;
    float linearY = currentAccelY - gravityEstimateY;
    float linearZ = currentAccelZ - gravityEstimateZ;
    float instantaneousLinearG = sqrtf(
        linearX * linearX +
        linearY * linearY +
        linearZ * linearZ
    );

    float gAlpha = dt / (GLOAD_SMOOTHING_TIME_SEC + dt);
    gAlpha = constrain(gAlpha, 0.0f, 1.0f);
    currentGLoad += (instantaneousLinearG - currentGLoad) * gAlpha;
    if (currentGLoad < GLOAD_DEADBAND_G) currentGLoad = 0.0f;

    float accelPitch = calculateAccelAngleRelative(rotationAxis, data);
    float gyroRate = getSelectedGyroRate(data) - gyroAxisBias;
    float accelRoll = calculateAccelAngleRelative(rollAxis, data);
    float rollRate = getGyroRateForAxis(data, rollAxis) - rollGyroBias;

    currentGyroRate = PITCH_SIGN * gyroRate;
    currentRollRate = rollRate;

    pitch =
        FILTER_ALPHA * (pitch + gyroRate * dt) +
        (1.0f - FILTER_ALPHA) * accelPitch;
    roll =
        FILTER_ALPHA * (roll + rollRate * dt) +
        (1.0f - FILTER_ALPHA) * accelRoll;

    currentAbsolutePitch = PITCH_SIGN * (pitch - pitchZero);
    currentRollAngle = roll - rollZero;

    if (currentGLoad > highestGLoad) highestGLoad = currentGLoad;
    return true;
}

// =====================================================
// SERIAL STATUS
// =====================================================

void updateSerialStatus() {
    unsigned long now = millis();
    if ((now - lastSerialUpdate) < 250) return;
    lastSerialUpdate = now;

    Serial.print("Raw:");
    Serial.print(currentAbsolutePitch, 1);
    Serial.print(" Base:");
    Serial.print(adaptiveBaseline, 1);
    Serial.print(" Trigger:");
    Serial.print(currentTriggerPitch, 1);
    Serial.print(" Gyro:");
    Serial.print(currentGyroRate, 1);
    Serial.print(" +G:");
    Serial.print(currentGLoad, 2);
    Serial.print(" Mode:");
    Serial.print(getAngleModeName());
    Serial.print(" ");
    Serial.print(getModeName());
    Serial.print(" State:");
    Serial.print(getStateName());
    Serial.print(" PWM:");
    Serial.print(outputBrightness);
    Serial.print(" AP:");
    Serial.print(accessPointEnabled ? "ON" : "OFF");
    Serial.print(" Clients:");
    Serial.println(accessPointEnabled ? WiFi.softAPgetStationNum() : 0);
}

// =====================================================
// SETUP
// =====================================================

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("Motorcycle Wheelie Controller");
    Serial.print("Firmware ");
    Serial.println(FIRMWARE_VERSION);
    Serial.printf("Build %s at %s (%s channel)\n", BUILD_COMMIT, BUILD_DATE, RELEASE_CHANNEL);
    Serial.printf("Target %s / %s\n", TARGET_BOARD_ID, TARGET_CHIP_ID);
    Serial.println("Dynamic +G + Peaks + Wi-Fi Recovery + Robust Discovery");
    Serial.println("--------------------------------");

    // Hardware-safe output state before anything else.
    pinMode(OUTPUT1_PWM_PIN, OUTPUT);
    digitalWrite(OUTPUT1_PWM_PIN, LOW);

    pinMode(USER_BUTTON_PIN, INPUT_PULLUP);

    loadSettings();
    Serial.printf("Accepted OTA channel: %s\n", otaChannel.c_str());
    writeToken = makeWriteToken();
    initializeRideLogging();

    // OLED initializes the shared I2C bus. Do not add Wire.begin().
    oled.begin();
    oled.setPowerSave(0);
    oled.setFlipMode(1);
    oled.setFont(u8x8_font_chroma48medium8_r);
    oled.clearDisplay();
    Wire.setClock(400000);

    drawBootScreen();
    delay(600);

    initializePWM();

    // MPU6050 configuration.
    writeMPURegister(PWR_MGMT_1, 0x00);
    delay(100);
    writeMPURegister(CONFIG_REG, 0x03);   // ~44/42 Hz internal DLPF
    writeMPURegister(ACCEL_CONFIG, ACCEL_RANGE_CONFIG_VALUE); // +/-4g
    writeMPURegister(GYRO_CONFIG, 0x00);  // ±250 deg/s

    if (!orientationConfigured) {
        Serial.println("No saved mounting orientation; starting first-setup wizard.");
        if (!runOrientationWizard()) {
            Serial.println("Using safe default axes until the wizard is completed in Settings.");
        }
    } else {
        Serial.printf("Saved orientation: vertical=%s roll=%s pitch=%s\n",
                      getAxisName(verticalAxis), getAxisName(rollAxis),
                      getAxisName(rotationAxis));
    }

    Serial.printf("Initial calibration begins in %u seconds; level the bike now.\n",
                  INITIAL_CALIBRATION_DELAY_SECONDS);
    for (uint8_t seconds = INITIAL_CALIBRATION_DELAY_SECONDS; seconds > 0; --seconds) {
        char countdown[17];
        snprintf(countdown, sizeof(countdown), "Starting in %u...", seconds);
        drawCalibrationScreen(countdown);
        delay(1000);
    }

    bool calibrated = calibrateMPU();
    lastMicros = micros();

    // A failed calibration always leaves the controller safe.
    if (calibrated && settings.bootArmed) {
        operatingMode = OperatingMode::ARMED;
        resetAdaptiveBaselineToCurrent();
        startRideSession();
    } else {
        operatingMode = OperatingMode::STANDBY;
        forceOutputOff();
    }

    initializeNetwork();

    oled.clearDisplay();
    displayDirty = true;

    Serial.println();
    Serial.println("CONTROLLER READY");
    Serial.printf("Angle mode: %s\n", getAngleModeName());
    Serial.printf("Boot mode: %s\n", getModeName());
    Serial.printf("Wi-Fi: %s\n", wifiApSsid.c_str());
    Serial.println("Try: http://wheelie.local");
    Serial.println("Fallback: http://192.168.4.1");
    Serial.println("Button: 1x page, 2x angle mode, 3x Wi-Fi AP, hold ARM/STANDBY, 30s reset Wi-Fi password");
    // If rollback support is enabled in the bootloader, reaching the end of
    // setup confirms that the newly selected OTA image initialized safely.
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println();
}

// =====================================================
// LOOP
// =====================================================

void loop() {
    if (accessPointEnabled) {
        dnsServer.processNextRequest();
        server.handleClient();
    }
    updateAccessPointRestart();
    updateButton();
    updateManualOutputTest();
    updateOutputFade();

    float dt = 0.01f;
    bool gotSensorSample = updateIMUAndPitch(dt);

    if (gotSensorSample) {
        updateAngleProcessing(dt);
        updateController(currentTriggerPitch);
    } else if (!imuHealthy) {
        controllerState = ControllerState::NORMAL;
        forceOutputOff();
    }
    updateRideLogging();

    updateDisplay();
    updateSerialStatus();

    delay(3);
}
