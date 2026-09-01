#include "firmware_api.h"
#include "modules/output_module.h"

namespace firmware {

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

namespace {
void beginOutput() {
    initializePWM();
}

void tickOutput() {
    updateManualOutputTest();
    updateOutputFade();
}

const FirmwareModule MODULE = {"output", beginOutput, tickOutput, 40, 30};
}

const FirmwareModule& outputModule() { return MODULE; }

} // namespace firmware
