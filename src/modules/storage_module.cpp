#include "firmware_api.h"
#include "modules/storage_module.h"

namespace firmware {

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
    riderModelEnabled = preferences.getBool("ridermodel", RIDER_MODEL_DEFAULT_ENABLED);

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
    preferences.putBool("ridermodel", riderModelEnabled);

    preferences.end();
    Serial.println("Settings saved to NVS");
}

// =====================================================
// MPU6050 LOW-LEVEL FUNCTIONS
// =====================================================

void loadRiderModel() {
    preferences.begin("wheelie", true);
    if (preferences.getBytesLength("rprofile") == sizeof(RiderProfile)) {
        preferences.getBytes("rprofile", &riderProfile, sizeof(riderProfile));
    }
    if (preferences.getBytesLength("evthist") == sizeof(ModelEventHistory)) {
        preferences.getBytes("evthist", &modelEventHistory, sizeof(modelEventHistory));
    }
    preferences.end();

    if (!isRiderProfileValid(riderProfile)) {
        memset(&riderProfile, 0, sizeof(riderProfile));
        riderProfile.magic = RIDER_PROFILE_MAGIC;
        riderProfile.version = RIDER_PROFILE_VERSION;
        riderProfile.size = sizeof(RiderProfile);
    }
    if (!isModelEventHistoryValid(modelEventHistory)) {
        memset(&modelEventHistory, 0, sizeof(modelEventHistory));
        modelEventHistory.magic = EVENT_HISTORY_MAGIC;
        modelEventHistory.version = EVENT_HISTORY_VERSION;
        modelEventHistory.size = sizeof(ModelEventHistory);
        modelEventHistory.nextId = 1;
    }
    effectiveAdaptiveFreezeRate = calculateNoiseAwareFreezeRate(
        settings.adaptiveFreezeRateDegSec, calibrationGyroNoiseRms);
    modelPreviousControllerState = controllerState;
    Serial.printf(
        "Rider model ready: %lu stable samples, %lu labels, %u recent events\n",
        (unsigned long)riderProfile.stableSamples,
        (unsigned long)(riderProfile.labeledCorrect + riderProfile.labeledFalse +
            riderProfile.labeledMissed),
        modelEventHistory.count);
}

void saveRiderModel() {
    if (!riderModelDirty) return;
    preferences.begin("wheelie", false);
    preferences.putBytes("rprofile", &riderProfile, sizeof(riderProfile));
    preferences.putBytes("evthist", &modelEventHistory, sizeof(modelEventHistory));
    preferences.end();
    riderModelDirty = false;
    modelLastProfileSaveMs = millis();
}

float recommendedRiderFreezeRate() {
    return calculateNoiseAwareFreezeRate(
        settings.adaptiveFreezeRateDegSec,
        fmaxf(calibrationGyroNoiseRms, riderProfileGyroRms(riderProfile)));
}

void addCurrentModelSample(ModelEventAccumulator& accumulator, uint32_t now) {
    addModelEventSample(
        accumulator, now, currentTriggerPitch, currentFilteredGyroRate,
        currentGLoad, currentRollAngle, adaptiveBaselineFrozen,
        settings.triggerAngle);
}

void beginCapturedModelEvent(uint32_t now) {
    if (modelEventAccumulator.active) return;
    const uint16_t oldest = (uint16_t)(
        (modelPreEventHead + MODEL_PRE_EVENT_SAMPLES - modelPreEventCount) %
        MODEL_PRE_EVENT_SAMPLES);
    const uint32_t startedMs = modelPreEventCount > 0
        ? modelPreEventRing[oldest].timeMs : now;
    beginModelEvent(modelEventAccumulator, modelEventHistory.nextId, startedMs);
    for (uint16_t logical = 0; logical < modelPreEventCount; ++logical) {
        const ModelRawSample& sample = modelPreEventRing[
            (oldest + logical) % MODEL_PRE_EVENT_SAMPLES];
        addModelEventSample(
            modelEventAccumulator, sample.timeMs, sample.pitch, sample.pitchRate,
            sample.gLoad, sample.roll, sample.baselineFrozen,
            settings.triggerAngle);
    }
    modelPendingOutcome = ModelEventOutcome::CANCELLED;
    modelFinishPending = false;
}

void finalizeCapturedModelEvent(uint32_t now) {
    if (!modelEventAccumulator.active) return;
    ModelEventFeatures event = finishModelEvent(
        modelEventAccumulator, now, modelPendingOutcome);
    event.rideSessionId = rideSessionActive ? activeRideHeader.sessionId : 0;
    appendModelEvent(modelEventHistory, event);
    riderModelDirty = true;
    modelFinishPending = false;
    Serial.printf(
        "Shadow event #%lu: %s score=%.2f rise=%.1f rate=%.1f samples=%u\n",
        (unsigned long)event.id, getModelOutcomeName(event.outcome),
        event.shadowScore, event.pitchRise, event.peakPitchRate,
        event.sampleCount);
}

void updateRiderModel() {
    const uint32_t now = millis();
    if (!riderModelEnabled) {
        modelPreviousControllerState = controllerState;
        return;
    }
    if ((int32_t)(now - modelNextSampleMs) < 0) return;
    modelNextSampleMs = now + MODEL_SAMPLE_INTERVAL_MS;

    const bool transitionedToPending =
        modelPreviousControllerState == ControllerState::NORMAL &&
        controllerState == ControllerState::TRIGGER_PENDING;
    if (transitionedToPending) {
        if (modelEventAccumulator.active) finalizeCapturedModelEvent(now);
        beginCapturedModelEvent(now);
    }
    if (modelPreviousControllerState == ControllerState::TRIGGER_PENDING &&
        controllerState == ControllerState::WHEELIE) {
        modelPendingOutcome = ModelEventOutcome::DETECTED;
    }
    if (modelEventAccumulator.active &&
        modelPreviousControllerState != ControllerState::NORMAL &&
        controllerState == ControllerState::NORMAL) {
        modelFinishPending = true;
        modelFinishAtMs = now + MODEL_POST_EVENT_MS;
    }

    ModelRawSample& ringSample = modelPreEventRing[modelPreEventHead];
    ringSample.timeMs = now;
    ringSample.pitch = currentTriggerPitch;
    ringSample.pitchRate = currentFilteredGyroRate;
    ringSample.gLoad = currentGLoad;
    ringSample.roll = currentRollAngle;
    ringSample.baselineFrozen = adaptiveBaselineFrozen;
    modelPreEventHead = (uint16_t)((modelPreEventHead + 1) % MODEL_PRE_EVENT_SAMPLES);
    if (modelPreEventCount < MODEL_PRE_EVENT_SAMPLES) modelPreEventCount++;

    if (modelEventAccumulator.active) addCurrentModelSample(modelEventAccumulator, now);

    const bool stableForProfile = imuHealthy &&
        operatingMode == OperatingMode::ARMED &&
        controllerState == ControllerState::NORMAL &&
        currentAccelerationTrust >= 0.80f &&
        fabsf(currentFilteredGyroRate) < 2.0f;
    if (stableForProfile) {
        updateStableRiderProfile(
            riderProfile, currentFilteredGyroRate,
            fabsf(currentAccelerationMagnitude - 1.0f));
        riderModelDirty = true;
    }

    if (modelFinishPending && (int32_t)(now - modelFinishAtMs) >= 0) {
        finalizeCapturedModelEvent(now);
    }
    if (riderModelDirty &&
        (uint32_t)(now - modelLastProfileSaveMs) >= MODEL_PROFILE_SAVE_INTERVAL_MS) {
        saveRiderModel();
    }
    modelPreviousControllerState = controllerState;
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

namespace {
void beginStorage() {
    loadSettings();
    loadRiderModel();
    initializeRideLogging();
}

void tickStorage() {
    updateRiderModel();
    updateRideLogging();
}

const FirmwareModule MODULE = {"storage", beginStorage, tickStorage, 20, 50};
}

const FirmwareModule& storageModule() { return MODULE; }

} // namespace firmware
