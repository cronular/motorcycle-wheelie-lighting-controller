#include "firmware_api.h"
#include "modules/sensor_module.h"

namespace firmware {

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
    currentFilteredGyroRate = 0.0f;
    currentRollAngle = 0.0f;
    currentRollRate = 0.0f;
    currentAccelerationTrust = 1.0f;
    currentAccelerationMagnitude = 1.0f;
    effectiveAdaptiveFreezeRate = calculateNoiseAwareFreezeRate(
        settings.adaptiveFreezeRateDegSec, calibrationGyroNoiseRms);
    adaptiveBaselineFrozen = false;
    adaptiveFreezeReason = AdaptiveFreezeReason::NONE;

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
    adaptiveFreezeReason = AdaptiveFreezeReason::NONE;
}

void updateAngleProcessing(float dt) {
    // Adaptive mode follows terrain only during ordinary, stable riding.
    // The portable helper is also exercised by the desktop test suite.
    const bool controllerAllowsTracking =
        operatingMode == OperatingMode::ARMED &&
        controllerState == ControllerState::NORMAL;
    currentTriggerPitch = processTriggerPitch(
        currentAbsolutePitch, currentFilteredGyroRate, dt, imuHealthy,
        controllerAllowsTracking, settings, adaptiveBaseline,
        adaptiveBaselineFrozen, &adaptiveFreezeReason,
        currentAccelerationTrust, effectiveAdaptiveFreezeRate);
    if (currentTriggerPitch > highestAngle) highestAngle = currentTriggerPitch;
}

// =====================================================
// RIDER PROFILE / SHADOW EVENT MODEL
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
    currentAccelerationMagnitude = sqrtf(
        currentAccelX * currentAccelX +
        currentAccelY * currentAccelY +
        currentAccelZ * currentAccelZ);
    currentAccelerationTrust = calculateAccelerationTrust(currentAccelerationMagnitude);

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
    currentFilteredGyroRate = updateLowPass(
        currentFilteredGyroRate, currentGyroRate, dt,
        GYRO_RATE_FILTER_TIME_CONSTANT_SEC);
    currentRollRate = rollRate;

    pitch = updateAdaptiveComplementaryAngle(
        pitch, gyroRate, accelPitch, dt, currentAccelerationTrust,
        ATTITUDE_FILTER_TIME_CONSTANT_SEC);
    roll = updateAdaptiveComplementaryAngle(
        roll, rollRate, accelRoll, dt, currentAccelerationTrust,
        ATTITUDE_FILTER_TIME_CONSTANT_SEC);

    currentAbsolutePitch = PITCH_SIGN * (pitch - pitchZero);
    currentRollAngle = roll - rollZero;

    if (currentGLoad > highestGLoad) highestGLoad = currentGLoad;
    return true;
}

// =====================================================
// SERIAL STATUS
// =====================================================

namespace {
void beginSensor() {
    writeMPURegister(PWR_MGMT_1, 0x00);
    delay(100);
    writeMPURegister(CONFIG_REG, 0x03);
    writeMPURegister(ACCEL_CONFIG, ACCEL_RANGE_CONFIG_VALUE);
    writeMPURegister(GYRO_CONFIG, 0x00);

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

    const bool calibrated = calibrateMPU();
    lastMicros = micros();
    if (calibrated && settings.bootArmed) {
        operatingMode = OperatingMode::ARMED;
        resetAdaptiveBaselineToCurrent();
        startRideSession();
    } else {
        operatingMode = OperatingMode::STANDBY;
        forceOutputOff();
    }
}

void tickSensor() {
    float dt = 0.01f;
    const bool gotSensorSample = updateIMUAndPitch(dt);
    if (gotSensorSample) {
        updateAngleProcessing(dt);
        updateController(currentTriggerPitch);
    } else if (!imuHealthy) {
        controllerState = ControllerState::NORMAL;
        forceOutputOff();
    }
}

const FirmwareModule MODULE = {"sensor", beginSensor, tickSensor, 50, 40};
}

const FirmwareModule& sensorModule() { return MODULE; }

} // namespace firmware
