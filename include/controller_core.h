#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum class AngleMode : uint8_t { ABSOLUTE = 0, ADAPTIVE = 1 };
enum class LightPattern : uint8_t { OFF = 0, SOLID = 1, SLOW_PULSE = 2, FAST_PULSE = 3, STROBE = 4 };
enum class RotationAxis : uint8_t { X = 0, Y = 1, Z = 2 };

struct OrientationResult {
    bool valid = false;
    RotationAxis verticalAxis = RotationAxis::Z;
    RotationAxis rollAxis = RotationAxis::X;
    RotationAxis pitchAxis = RotationAxis::Y;
    float rollMotionDegSec = 0.0f;
    float confidence = 0.0f;
};

struct ControllerSettings {
    float triggerAngle = 20.0f;
    float resetAngle = 10.0f;
    uint32_t triggerHoldMs = 150;
    uint32_t minimumOnTimeMs = 1000;
    uint8_t brightness = 255;
    uint32_t fadeMs = 200;
    bool bootArmed = false;
    AngleMode angleMode = AngleMode::ABSOLUTE;
    float adaptiveTimeConstantSec = 4.0f;
    float adaptiveFreezeRateDegSec = 8.0f;
    float warningAngle = 45.0f;
    float warningResetAngle = 40.0f;
    float warningPitchRateDegSec = 45.0f;
    LightPattern wheeliePattern = LightPattern::SOLID;
    LightPattern warningPattern = LightPattern::STROBE;
    uint8_t warningBrightness = 255;
};

enum class ControllerState : uint8_t { NORMAL, TRIGGER_PENDING, WHEELIE };

constexpr uint16_t FIRMWARE_PACKAGE_HEADER_SIZE = 16;
constexpr uint8_t FIRMWARE_PACKAGE_MAGIC[8] = {'W', 'C', 'T', 'R', 'L', '1', '\r', '\n'};
constexpr uint16_t FIRMWARE_MANIFEST_MAX_SIZE = 768;
constexpr uint16_t FIRMWARE_SIGNATURE_MAX_SIZE = 80;

struct WriteRateLimitBucket {
    uint32_t clientKey = 0;
    uint32_t windowStartedMs = 0;
    uint32_t blockedUntilMs = 0;
    uint32_t lastSeenMs = 0;
    uint8_t requestCount = 0;
    bool occupied = false;
};

enum class WriteRateLimitDecision : uint8_t { ALLOW, BLOCK };

// Hardware-independent helpers live here so the ESP32 firmware and desktop
// regression suite exercise the same behavior. Keep this header free of
// Arduino/ESP32 dependencies.
inline float controllerClamp(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

inline uint16_t readLittleEndian16(const uint8_t* data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

inline uint32_t readLittleEndian32(const uint8_t* data) {
    return (uint32_t)data[0] |
        ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) |
        ((uint32_t)data[3] << 24);
}

inline bool hasFirmwarePackageMagic(const uint8_t* data, size_t size) {
    if (size < sizeof(FIRMWARE_PACKAGE_MAGIC)) return false;
    uint8_t difference = 0;
    for (size_t index = 0; index < sizeof(FIRMWARE_PACKAGE_MAGIC); ++index) {
        difference |= data[index] ^ FIRMWARE_PACKAGE_MAGIC[index];
    }
    return difference == 0;
}

inline bool isFirmwareMetadataCompatible(
    const char* packageBoard,
    const char* packageChannel,
    const char* expectedBoard,
    const char* selectedChannel
) {
    if (packageBoard == nullptr || packageChannel == nullptr ||
        expectedBoard == nullptr || selectedChannel == nullptr) return false;
    return strcmp(packageBoard, expectedBoard) == 0 &&
        strcmp(packageChannel, selectedChannel) == 0;
}

inline WriteRateLimitDecision checkWriteRateLimit(
    WriteRateLimitBucket* buckets,
    size_t bucketCount,
    uint32_t clientKey,
    uint32_t nowMs,
    uint8_t maximumRequests = 12,
    uint32_t windowMs = 10000,
    uint32_t blockMs = 30000
) {
    if (buckets == nullptr || bucketCount == 0) return WriteRateLimitDecision::BLOCK;

    size_t selected = bucketCount;
    size_t oldest = 0;
    for (size_t index = 0; index < bucketCount; ++index) {
        if (buckets[index].occupied && buckets[index].clientKey == clientKey) {
            selected = index;
            break;
        }
        if (!buckets[index].occupied) {
            selected = index;
            break;
        }
        if ((uint32_t)(nowMs - buckets[index].lastSeenMs) >
            (uint32_t)(nowMs - buckets[oldest].lastSeenMs)) oldest = index;
    }
    if (selected == bucketCount) selected = oldest;

    WriteRateLimitBucket& bucket = buckets[selected];
    if (!bucket.occupied || bucket.clientKey != clientKey) {
        bucket = WriteRateLimitBucket{};
        bucket.occupied = true;
        bucket.clientKey = clientKey;
        bucket.windowStartedMs = nowMs;
    }
    bucket.lastSeenMs = nowMs;

    if (bucket.blockedUntilMs != 0 &&
        (int32_t)(bucket.blockedUntilMs - nowMs) > 0) {
        return WriteRateLimitDecision::BLOCK;
    }
    bucket.blockedUntilMs = 0;

    if ((uint32_t)(nowMs - bucket.windowStartedMs) >= windowMs) {
        bucket.windowStartedMs = nowMs;
        bucket.requestCount = 0;
    }
    if (bucket.requestCount >= maximumRequests) {
        bucket.blockedUntilMs = nowMs + blockMs;
        return WriteRateLimitDecision::BLOCK;
    }
    bucket.requestCount++;
    return WriteRateLimitDecision::ALLOW;
}

inline bool isRotationAxisValid(RotationAxis axis) {
    return axis == RotationAxis::X || axis == RotationAxis::Y || axis == RotationAxis::Z;
}

inline RotationAxis remainingRotationAxis(RotationAxis first, RotationAxis second) {
    for (uint8_t value = 0; value < 3; ++value) {
        const RotationAxis candidate = static_cast<RotationAxis>(value);
        if (candidate != first && candidate != second) return candidate;
    }
    return RotationAxis::Y;
}

inline float calculateRelativeRotationDegrees(
    RotationAxis axis,
    float referenceX,
    float referenceY,
    float referenceZ,
    float currentX,
    float currentY,
    float currentZ
) {
    float crossAlongAxis = 0.0f;
    float projectedDot = 0.0f;
    switch (axis) {
        case RotationAxis::X:
            crossAlongAxis = currentY * referenceZ - currentZ * referenceY;
            projectedDot = currentY * referenceY + currentZ * referenceZ;
            break;
        case RotationAxis::Y:
            crossAlongAxis = currentZ * referenceX - currentX * referenceZ;
            projectedDot = currentX * referenceX + currentZ * referenceZ;
            break;
        case RotationAxis::Z:
            crossAlongAxis = currentX * referenceY - currentY * referenceX;
            projectedDot = currentX * referenceX + currentY * referenceY;
            break;
    }
    return atan2f(crossAlongAxis, projectedDot) * 57.29577951308232f;
}

// The upright gravity vector identifies the sensor axis that points mostly
// vertically. Side-to-side motion then identifies roll from the two remaining
// gyro axes; the last orthogonal axis is pitch.
inline OrientationResult detectOrientationAxes(
    float uprightAx,
    float uprightAy,
    float uprightAz,
    float motionXDegSec,
    float motionYDegSec,
    float motionZDegSec
) {
    OrientationResult result;
    const float acceleration[3] = {
        fabsf(uprightAx), fabsf(uprightAy), fabsf(uprightAz)
    };
    const float motion[3] = {
        fabsf(motionXDegSec), fabsf(motionYDegSec), fabsf(motionZDegSec)
    };
    const float gravityMagnitude = sqrtf(
        uprightAx * uprightAx + uprightAy * uprightAy + uprightAz * uprightAz
    );
    if (!isfinite(gravityMagnitude) || gravityMagnitude < 0.65f || gravityMagnitude > 1.35f) {
        return result;
    }

    uint8_t vertical = 0;
    if (acceleration[1] > acceleration[vertical]) vertical = 1;
    if (acceleration[2] > acceleration[vertical]) vertical = 2;

    uint8_t firstHorizontal = vertical == 0 ? 1 : 0;
    uint8_t secondHorizontal = vertical == 2 ? 1 : 2;
    if (firstHorizontal == vertical) firstHorizontal = 0;
    if (secondHorizontal == vertical || secondHorizontal == firstHorizontal) {
        secondHorizontal = 3 - vertical - firstHorizontal;
    }

    const uint8_t roll = motion[firstHorizontal] >= motion[secondHorizontal]
        ? firstHorizontal : secondHorizontal;
    const uint8_t other = roll == firstHorizontal ? secondHorizontal : firstHorizontal;
    const float strongestMotion = motion[roll];
    const float competingMotion = motion[other];
    const float confidence = strongestMotion / fmaxf(competingMotion, 0.1f);

    result.verticalAxis = static_cast<RotationAxis>(vertical);
    result.rollAxis = static_cast<RotationAxis>(roll);
    result.pitchAxis = remainingRotationAxis(result.verticalAxis, result.rollAxis);
    result.rollMotionDegSec = strongestMotion;
    result.confidence = confidence;
    result.valid = isfinite(strongestMotion) && isfinite(confidence) &&
        strongestMotion >= 2.0f && confidence >= 1.25f;
    return result;
}

inline void validateControllerSettings(ControllerSettings& s) {
    if (!isfinite(s.triggerAngle) || s.triggerAngle < 5.0f || s.triggerAngle > 70.0f) s.triggerAngle = 20.0f;
    if (!isfinite(s.resetAngle) || s.resetAngle < 0.0f || s.resetAngle >= s.triggerAngle) s.resetAngle = 10.0f;
    if (s.triggerHoldMs > 5000) s.triggerHoldMs = 150;
    if (s.minimumOnTimeMs > 15000) s.minimumOnTimeMs = 1000;
    if (s.brightness == 0) s.brightness = 255;
    if (s.fadeMs > 3000) s.fadeMs = 200;
    if (s.angleMode != AngleMode::ABSOLUTE && s.angleMode != AngleMode::ADAPTIVE) s.angleMode = AngleMode::ABSOLUTE;
    if (!isfinite(s.adaptiveTimeConstantSec) || s.adaptiveTimeConstantSec < 0.5f || s.adaptiveTimeConstantSec > 60.0f) s.adaptiveTimeConstantSec = 4.0f;
    if (!isfinite(s.adaptiveFreezeRateDegSec) || s.adaptiveFreezeRateDegSec < 1.0f || s.adaptiveFreezeRateDegSec > 100.0f) s.adaptiveFreezeRateDegSec = 8.0f;
    if (!isfinite(s.warningAngle) || s.warningAngle < 5.0f || s.warningAngle > 85.0f) s.warningAngle = 45.0f;
    if (!isfinite(s.warningResetAngle) || s.warningResetAngle < 0.0f || s.warningResetAngle >= s.warningAngle) s.warningResetAngle = fmaxf(0.0f, s.warningAngle - 5.0f);
    if (!isfinite(s.warningPitchRateDegSec) || s.warningPitchRateDegSec < 0.0f || s.warningPitchRateDegSec > 250.0f) s.warningPitchRateDegSec = 45.0f;
    if (s.wheeliePattern > LightPattern::STROBE) s.wheeliePattern = LightPattern::SOLID;
    if (s.warningPattern > LightPattern::STROBE) s.warningPattern = LightPattern::STROBE;
    if (s.warningBrightness == 0) s.warningBrightness = 255;
}

// Pure transition logic; the caller applies PWM/fading from the result.
inline bool stepController(ControllerState& state, float pitch, unsigned long now,
                           const ControllerSettings& s, unsigned long& triggerStart,
                           unsigned long& wheelieStart) {
    switch (state) {
        case ControllerState::NORMAL:
            if (pitch >= s.triggerAngle) { triggerStart = now; state = ControllerState::TRIGGER_PENDING; }
            return false;
        case ControllerState::TRIGGER_PENDING:
            if (pitch < s.triggerAngle) { state = ControllerState::NORMAL; return false; }
            if ((unsigned long)(now - triggerStart) >= s.triggerHoldMs) {
                wheelieStart = now; state = ControllerState::WHEELIE; return true;
            }
            return false;
        case ControllerState::WHEELIE:
            if ((unsigned long)(now - wheelieStart) >= s.minimumOnTimeMs && pitch <= s.resetAngle) {
                state = ControllerState::NORMAL; return false;
            }
            return true;
    }
    state = ControllerState::NORMAL;
    return false;
}

inline float processTriggerPitch(
    float absolutePitch,
    float gyroRate,
    float dt,
    bool imuHealthy,
    bool controllerAllowsTracking,
    const ControllerSettings& settings,
    float& adaptiveBaseline,
    bool& adaptiveBaselineFrozen
) {
    if (settings.angleMode == AngleMode::ABSOLUTE) {
        adaptiveBaseline = 0.0f;
        adaptiveBaselineFrozen = false;
        return absolutePitch;
    }

    const bool gyroAllowsTracking =
        fabsf(gyroRate) < settings.adaptiveFreezeRateDegSec;
    const bool canTrack =
        imuHealthy && controllerAllowsTracking && gyroAllowsTracking;

    adaptiveBaselineFrozen = !canTrack;
    if (canTrack) {
        const float safeDt = dt > 0.0f ? dt : 0.0f;
        const float tau = fmaxf(settings.adaptiveTimeConstantSec, 0.5f);
        const float alpha = controllerClamp(safeDt / (tau + safeDt), 0.0f, 1.0f);
        adaptiveBaseline += (absolutePitch - adaptiveBaseline) * alpha;
    }

    return absolutePitch - adaptiveBaseline;
}

inline bool stepHighAngleWarning(
    bool active,
    float triggerPitch,
    float gyroRate,
    const ControllerSettings& settings
) {
    const bool rateEnabled = settings.warningPitchRateDegSec > 0.0f;
    const bool rateWarning = rateEnabled &&
        gyroRate >= settings.warningPitchRateDegSec &&
        triggerPitch >= settings.triggerAngle;

    if (!active) {
        return triggerPitch >= settings.warningAngle || rateWarning;
    }

    const float releaseRate =
        rateEnabled ? settings.warningPitchRateDegSec * 0.5f : 0.0f;
    if (triggerPitch <= settings.warningResetAngle &&
        (!rateEnabled || gyroRate < releaseRate)) {
        return false;
    }
    return true;
}

inline uint8_t calculatePatternBrightness(
    LightPattern pattern,
    uint8_t brightness,
    unsigned long now
) {
    switch (pattern) {
        case LightPattern::OFF:
            return 0;
        case LightPattern::SOLID:
            return brightness;
        case LightPattern::SLOW_PULSE:
        case LightPattern::FAST_PULSE: {
            const float period =
                pattern == LightPattern::SLOW_PULSE ? 1600.0f : 650.0f;
            const float phase =
                (float)(now % (unsigned long)period) / period;
            const float intensity = 0.18f + 0.82f *
                (0.5f - 0.5f * cosf(phase * 2.0f * 3.14159265358979323846f));
            return (uint8_t)roundf(brightness * intensity);
        }
        case LightPattern::STROBE:
            return (now % 320UL) < 95UL ? brightness : 0;
    }
    return 0;
}

inline uint8_t calculateFadeBrightness(
    uint8_t start,
    uint8_t target,
    unsigned long elapsed,
    uint32_t duration
) {
    if (duration == 0 || elapsed >= duration) {
        return target;
    }
    const float progress = (float)elapsed / (float)duration;
    const float value = start + ((float)target - start) * progress;
    return (uint8_t)roundf(controllerClamp(value, 0.0f, 255.0f));
}
