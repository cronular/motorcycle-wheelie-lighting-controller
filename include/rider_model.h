#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "controller_core.h"

constexpr uint32_t RIDER_PROFILE_MAGIC = 0x31504652UL; // RFP1
constexpr uint16_t RIDER_PROFILE_VERSION = 1;
constexpr uint32_t EVENT_HISTORY_MAGIC = 0x31545645UL; // EVT1
constexpr uint16_t EVENT_HISTORY_VERSION = 1;
constexpr uint8_t EVENT_HISTORY_CAPACITY = 12;
constexpr bool RIDER_MODEL_DEFAULT_ENABLED = false;

enum class ModelEventOutcome : uint8_t {
    CANCELLED = 0,
    DETECTED = 1,
    MISSED = 2,
};

enum class ModelEventLabel : uint8_t {
    UNLABELED = 0,
    CORRECT = 1,
    FALSE_TRIGGER = 2,
    MISSED = 3,
};

struct RiderProfile {
    uint32_t magic = RIDER_PROFILE_MAGIC;
    uint16_t version = RIDER_PROFILE_VERSION;
    uint16_t size = sizeof(RiderProfile);
    uint32_t stableSamples = 0;
    uint32_t labeledCorrect = 0;
    uint32_t labeledFalse = 0;
    uint32_t labeledMissed = 0;
    float gyroMean = 0.0f;
    float gyroM2 = 0.0f;
    float accelResidualMean = 0.0f;
    float accelResidualM2 = 0.0f;
    float correctPitchRiseMean = 0.0f;
    float correctPeakRateMean = 0.0f;
    float falsePitchRiseMean = 0.0f;
    float falsePeakRateMean = 0.0f;
};

struct ModelEventFeatures {
    uint32_t id = 0;
    uint32_t rideSessionId = 0;
    uint32_t startedMs = 0;
    uint32_t durationMs = 0;
    uint16_t sampleCount = 0;
    uint16_t aboveTriggerSamples = 0;
    ModelEventOutcome outcome = ModelEventOutcome::CANCELLED;
    ModelEventLabel label = ModelEventLabel::UNLABELED;
    float startPitch = 0.0f;
    float endPitch = 0.0f;
    float peakPitch = 0.0f;
    float pitchRise = 0.0f;
    float peakPitchRate = 0.0f;
    float rmsPitchRate = 0.0f;
    float integratedPositiveRate = 0.0f;
    float peakG = 0.0f;
    float rmsG = 0.0f;
    float peakAbsRoll = 0.0f;
    float frozenFraction = 0.0f;
    float shadowScore = 0.0f;
};

struct ModelEventAccumulator {
    bool active = false;
    uint32_t id = 0;
    uint32_t startedMs = 0;
    uint32_t lastSampleMs = 0;
    uint32_t sumFrozen = 0;
    uint32_t aboveTriggerSamples = 0;
    uint32_t sampleCount = 0;
    float startPitch = 0.0f;
    float lastPitch = 0.0f;
    float peakPitch = -1000.0f;
    float peakPitchRate = 0.0f;
    float sumPitchRateSq = 0.0f;
    float integratedPositiveRate = 0.0f;
    float peakG = 0.0f;
    float sumGSq = 0.0f;
    float peakAbsRoll = 0.0f;
};

struct ModelEventHistory {
    uint32_t magic = EVENT_HISTORY_MAGIC;
    uint16_t version = EVENT_HISTORY_VERSION;
    uint16_t size = sizeof(ModelEventHistory);
    uint32_t nextId = 1;
    uint8_t count = 0;
    uint8_t head = 0;
    uint8_t reserved[2] = {};
    ModelEventFeatures events[EVENT_HISTORY_CAPACITY] = {};
};

static_assert(sizeof(RiderProfile) <= 256, "Rider profile must remain compact");
static_assert(sizeof(ModelEventHistory) <= 2048, "Event history must remain NVS-safe");

inline bool isRiderProfileValid(const RiderProfile& profile) {
    return profile.magic == RIDER_PROFILE_MAGIC &&
        profile.version == RIDER_PROFILE_VERSION &&
        profile.size == sizeof(RiderProfile);
}

inline bool isModelEventHistoryValid(const ModelEventHistory& history) {
    return history.magic == EVENT_HISTORY_MAGIC &&
        history.version == EVENT_HISTORY_VERSION &&
        history.size == sizeof(ModelEventHistory) &&
        history.count <= EVENT_HISTORY_CAPACITY &&
        history.head < EVENT_HISTORY_CAPACITY &&
        history.nextId > 0;
}

inline void updateRunningDistribution(
    uint32_t count,
    float sample,
    float& mean,
    float& m2
) {
    const float delta = sample - mean;
    mean += delta / (float)count;
    m2 += delta * (sample - mean);
}

inline void updateStableRiderProfile(
    RiderProfile& profile,
    float gyroRate,
    float accelerationResidualG
) {
    if (!isfinite(gyroRate) || !isfinite(accelerationResidualG)) return;
    if (profile.stableSamples == UINT32_MAX) return;
    profile.stableSamples++;
    updateRunningDistribution(
        profile.stableSamples, gyroRate, profile.gyroMean, profile.gyroM2);
    updateRunningDistribution(
        profile.stableSamples, fabsf(accelerationResidualG),
        profile.accelResidualMean, profile.accelResidualM2);
}

inline float riderProfileGyroRms(const RiderProfile& profile) {
    if (profile.stableSamples < 2) return 0.0f;
    return sqrtf(fmaxf(profile.gyroM2 / (profile.stableSamples - 1), 0.0f));
}

inline float riderProfileAccelResidualRms(const RiderProfile& profile) {
    if (profile.stableSamples < 2) return 0.0f;
    return sqrtf(fmaxf(
        profile.accelResidualM2 / (profile.stableSamples - 1), 0.0f));
}

inline void beginModelEvent(
    ModelEventAccumulator& accumulator,
    uint32_t id,
    uint32_t nowMs
) {
    accumulator = ModelEventAccumulator{};
    accumulator.active = true;
    accumulator.id = id;
    accumulator.startedMs = nowMs;
    accumulator.lastSampleMs = nowMs;
}

inline void addModelEventSample(
    ModelEventAccumulator& accumulator,
    uint32_t nowMs,
    float pitch,
    float pitchRate,
    float gLoad,
    float roll,
    bool baselineFrozen,
    float triggerAngle
) {
    if (!accumulator.active || !isfinite(pitch) || !isfinite(pitchRate) ||
        !isfinite(gLoad) || !isfinite(roll)) return;

    const float dt = accumulator.sampleCount == 0 ? 0.0f :
        (float)(uint32_t)(nowMs - accumulator.lastSampleMs) / 1000.0f;
    if (accumulator.sampleCount == 0) accumulator.startPitch = pitch;
    accumulator.lastPitch = pitch;
    accumulator.lastSampleMs = nowMs;
    accumulator.sampleCount++;
    if (pitch >= triggerAngle) accumulator.aboveTriggerSamples++;
    if (baselineFrozen) accumulator.sumFrozen++;
    accumulator.peakPitch = fmaxf(accumulator.peakPitch, pitch);
    accumulator.peakPitchRate = fmaxf(accumulator.peakPitchRate, pitchRate);
    accumulator.sumPitchRateSq += pitchRate * pitchRate;
    accumulator.integratedPositiveRate += fmaxf(pitchRate, 0.0f) *
        controllerClamp(dt, 0.0f, 0.1f);
    accumulator.peakG = fmaxf(accumulator.peakG, gLoad);
    accumulator.sumGSq += gLoad * gLoad;
    accumulator.peakAbsRoll = fmaxf(accumulator.peakAbsRoll, fabsf(roll));
}

inline float calculateShadowEventScore(const ModelEventFeatures& event) {
    // Transparent seed model. It stays advisory until coefficients are
    // replaced by a versioned model trained from rider-confirmed exports.
    float z = -3.0f;
    z += 0.10f * controllerClamp(event.pitchRise, 0.0f, 45.0f);
    z += 0.025f * controllerClamp(event.peakPitchRate, 0.0f, 120.0f);
    z += 0.018f * controllerClamp(event.integratedPositiveRate, 0.0f, 80.0f);
    z += 0.80f * controllerClamp(
        event.sampleCount == 0 ? 0.0f :
            (float)event.aboveTriggerSamples / event.sampleCount,
        0.0f, 1.0f);
    z -= 0.55f * controllerClamp(event.peakG, 0.0f, 4.0f);
    z -= 0.35f * controllerClamp(event.frozenFraction, 0.0f, 1.0f);
    return 1.0f / (1.0f + expf(-z));
}

inline ModelEventFeatures finishModelEvent(
    ModelEventAccumulator& accumulator,
    uint32_t nowMs,
    ModelEventOutcome outcome
) {
    ModelEventFeatures event;
    event.id = accumulator.id;
    event.startedMs = accumulator.startedMs;
    event.durationMs = (uint32_t)(nowMs - accumulator.startedMs);
    event.sampleCount = (uint16_t)(accumulator.sampleCount > UINT16_MAX
        ? UINT16_MAX : accumulator.sampleCount);
    event.aboveTriggerSamples = (uint16_t)(accumulator.aboveTriggerSamples > UINT16_MAX
        ? UINT16_MAX : accumulator.aboveTriggerSamples);
    event.outcome = outcome;
    event.startPitch = accumulator.startPitch;
    event.endPitch = accumulator.lastPitch;
    event.peakPitch = accumulator.sampleCount == 0 ? 0.0f : accumulator.peakPitch;
    event.pitchRise = event.peakPitch - event.startPitch;
    event.peakPitchRate = accumulator.peakPitchRate;
    if (accumulator.sampleCount > 0) {
        event.rmsPitchRate = sqrtf(accumulator.sumPitchRateSq / accumulator.sampleCount);
        event.rmsG = sqrtf(accumulator.sumGSq / accumulator.sampleCount);
        event.frozenFraction = (float)accumulator.sumFrozen / accumulator.sampleCount;
    }
    event.integratedPositiveRate = accumulator.integratedPositiveRate;
    event.peakG = accumulator.peakG;
    event.peakAbsRoll = accumulator.peakAbsRoll;
    event.shadowScore = calculateShadowEventScore(event);
    accumulator.active = false;
    return event;
}

inline void appendModelEvent(ModelEventHistory& history, const ModelEventFeatures& event) {
    history.events[history.head] = event;
    history.head = (uint8_t)((history.head + 1) % EVENT_HISTORY_CAPACITY);
    if (history.count < EVENT_HISTORY_CAPACITY) history.count++;
    if (event.id >= history.nextId) history.nextId = event.id + 1;
    if (history.nextId == 0) history.nextId = 1;
}

inline ModelEventFeatures* findModelEvent(ModelEventHistory& history, uint32_t id) {
    for (uint8_t index = 0; index < history.count; ++index) {
        if (history.events[index].id == id) return &history.events[index];
    }
    return nullptr;
}

inline void updateRiderProfileFromLabel(
    RiderProfile& profile,
    const ModelEventFeatures& event,
    ModelEventLabel label
) {
    if (label == ModelEventLabel::CORRECT) {
        profile.labeledCorrect++;
        const float weight = 1.0f / profile.labeledCorrect;
        profile.correctPitchRiseMean += (event.pitchRise - profile.correctPitchRiseMean) * weight;
        profile.correctPeakRateMean += (event.peakPitchRate - profile.correctPeakRateMean) * weight;
    } else if (label == ModelEventLabel::FALSE_TRIGGER) {
        profile.labeledFalse++;
        const float weight = 1.0f / profile.labeledFalse;
        profile.falsePitchRiseMean += (event.pitchRise - profile.falsePitchRiseMean) * weight;
        profile.falsePeakRateMean += (event.peakPitchRate - profile.falsePeakRateMean) * weight;
    } else if (label == ModelEventLabel::MISSED) {
        profile.labeledMissed++;
    }
}

inline float riderModelConfidence(const RiderProfile& profile) {
    const uint32_t labels = profile.labeledCorrect +
        profile.labeledFalse + profile.labeledMissed;
    const float stableConfidence = controllerClamp(profile.stableSamples / 3000.0f, 0.0f, 1.0f);
    const float labelConfidence = controllerClamp(labels / 10.0f, 0.0f, 1.0f);
    return stableConfidence * labelConfidence;
}
