#include "firmware_api.h"
#include "modules/operating_mode_module.h"

namespace firmware {

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

const char* getAdaptiveFreezeReasonName() {
    switch (adaptiveFreezeReason) {
        case AdaptiveFreezeReason::NONE: return "TRACKING";
        case AdaptiveFreezeReason::IMU_UNHEALTHY: return "HOLD: IMU";
        case AdaptiveFreezeReason::CONTROLLER_ACTIVE: return "HOLD: STATE";
        case AdaptiveFreezeReason::MOTION: return "HOLD: MOTION";
        case AdaptiveFreezeReason::ACCELERATION: return "HOLD: ACCEL";
    }
    return "HOLD";
}

const char* getModelOutcomeName(ModelEventOutcome outcome) {
    switch (outcome) {
        case ModelEventOutcome::CANCELLED: return "cancelled";
        case ModelEventOutcome::DETECTED: return "detected";
        case ModelEventOutcome::MISSED: return "missed";
    }
    return "unknown";
}

const char* getModelLabelName(ModelEventLabel label) {
    switch (label) {
        case ModelEventLabel::UNLABELED: return "unlabeled";
        case ModelEventLabel::CORRECT: return "correct";
        case ModelEventLabel::FALSE_TRIGGER: return "false";
        case ModelEventLabel::MISSED: return "missed";
    }
    return "unlabeled";
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

void setOperatingMode(OperatingMode mode) {
    const OperatingMode previousMode = operatingMode;
    if (previousMode == OperatingMode::ARMED && mode != OperatingMode::ARMED) {
        finishRideSession();
        saveRiderModel();
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
        adaptiveFreezeReason = AdaptiveFreezeReason::NONE;
    } else {
        settings.angleMode = AngleMode::ABSOLUTE;
        adaptiveBaseline = 0.0f;
        currentTriggerPitch = currentAbsolutePitch;
        adaptiveBaselineFrozen = false;
        adaptiveFreezeReason = AdaptiveFreezeReason::NONE;
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

void updateHighAngleWarning(float triggerPitch) {
    highAngleWarningActive = stepHighAngleWarning(
        highAngleWarningActive, triggerPitch, currentFilteredGyroRate, settings);
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

    const uint8_t requestedBrightness = calculateRequestedBrightness(
        highAngleWarningActive, outputOn,
        settings.warningPattern, settings.warningBrightness,
        settings.wheeliePattern, settings.brightness, millis());
    setOutputTarget(requestedBrightness, true);
    if (controllerState != previousState) displayDirty = true;
}

// =====================================================
// WRITE-TOKEN / WEB SECURITY HELPER
// =====================================================

namespace {
void beginOperatingMode() {
    pinMode(USER_BUTTON_PIN, INPUT_PULLUP);
}

void tickOperatingMode() {
    updateButton();
}

const FirmwareModule MODULE = {
    "operating-mode", beginOperatingMode, tickOperatingMode, 10, 20
};
}

const FirmwareModule& operatingModeModule() { return MODULE; }

} // namespace firmware
