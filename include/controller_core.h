#pragma once

#include <math.h>
#include <stdint.h>

enum class AngleMode : uint8_t { ABSOLUTE = 0, ADAPTIVE = 1 };
enum class LightPattern : uint8_t { OFF = 0, SOLID = 1, SLOW_PULSE = 2, FAST_PULSE = 3, STROBE = 4 };

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

// Hardware-independent helpers live here so the ESP32 firmware and desktop
// regression suite exercise the same behavior. Keep this header free of
// Arduino/ESP32 dependencies.
inline float controllerClamp(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
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
