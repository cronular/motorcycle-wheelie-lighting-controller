#ifdef ARDUINO
#include <Arduino.h>
#endif
#include <unity.h>
#include "controller_core.h"

void setUp() {}
void tearDown() {}

void test_trigger_hold() {
    ControllerSettings s; ControllerState state = ControllerState::NORMAL; unsigned long trigger = 0, wheelie = 0;
    TEST_ASSERT_FALSE(stepController(state, 20.0f, 1000, s, trigger, wheelie));
    TEST_ASSERT_EQUAL_INT((int)ControllerState::TRIGGER_PENDING, (int)state);
    TEST_ASSERT_FALSE(stepController(state, 25.0f, 1149, s, trigger, wheelie));
    TEST_ASSERT_TRUE(stepController(state, 25.0f, 1150, s, trigger, wheelie));
}

void test_pending_cancellation() {
    ControllerSettings s; ControllerState state = ControllerState::NORMAL; unsigned long trigger = 0, wheelie = 0;
    stepController(state, 30.0f, 50, s, trigger, wheelie);
    TEST_ASSERT_FALSE(stepController(state, 19.9f, 100, s, trigger, wheelie));
    TEST_ASSERT_EQUAL_INT((int)ControllerState::NORMAL, (int)state);
}

void test_minimum_on_and_hysteresis() {
    ControllerSettings s; s.triggerHoldMs = 0; s.minimumOnTimeMs = 1000;
    ControllerState state = ControllerState::NORMAL; unsigned long trigger = 0, wheelie = 0;
    stepController(state, 25.0f, 100, s, trigger, wheelie);
    TEST_ASSERT_TRUE(stepController(state, 25.0f, 100, s, trigger, wheelie));
    TEST_ASSERT_TRUE(stepController(state, 0.0f, 1099, s, trigger, wheelie));
    TEST_ASSERT_TRUE(stepController(state, 10.1f, 1100, s, trigger, wheelie));
    TEST_ASSERT_FALSE(stepController(state, 10.0f, 1101, s, trigger, wheelie));
}

void test_millis_rollover() {
    ControllerSettings s; s.triggerHoldMs = 32;
    ControllerState state = ControllerState::NORMAL; unsigned long trigger = 0, wheelie = 0;
    stepController(state, 25.0f, UINT32_MAX - 15, s, trigger, wheelie);
    TEST_ASSERT_FALSE(stepController(state, 25.0f, 15, s, trigger, wheelie));
    TEST_ASSERT_TRUE(stepController(state, 25.0f, 16, s, trigger, wheelie));
}

void test_invalid_settings() {
    ControllerSettings s; s.triggerAngle = NAN; s.resetAngle = INFINITY;
    s.triggerHoldMs = 5001; s.minimumOnTimeMs = 15001; s.brightness = 0; s.fadeMs = 3001;
    s.angleMode = static_cast<AngleMode>(99); s.adaptiveTimeConstantSec = NAN; s.adaptiveFreezeRateDegSec = -INFINITY;
    validateControllerSettings(s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, s.triggerAngle);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.0f, s.resetAngle);
    TEST_ASSERT_EQUAL_UINT32(150, s.triggerHoldMs); TEST_ASSERT_EQUAL_UINT32(1000, s.minimumOnTimeMs);
    TEST_ASSERT_EQUAL_UINT8(255, s.brightness); TEST_ASSERT_EQUAL_UINT32(200, s.fadeMs);
    TEST_ASSERT_EQUAL_INT((int)AngleMode::ABSOLUTE, (int)s.angleMode);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.0f, s.adaptiveTimeConstantSec);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 8.0f, s.adaptiveFreezeRateDegSec);
}

void test_warning_and_pattern_settings_validation() {
    ControllerSettings s;
    s.warningAngle = NAN;
    s.warningResetAngle = INFINITY;
    s.warningPitchRateDegSec = 251.0f;
    s.wheeliePattern = static_cast<LightPattern>(99);
    s.warningPattern = static_cast<LightPattern>(99);
    s.warningBrightness = 0;
    validateControllerSettings(s);

    TEST_ASSERT_FLOAT_WITHIN(0.001f, 45.0f, s.warningAngle);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 40.0f, s.warningResetAngle);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 45.0f, s.warningPitchRateDegSec);
    TEST_ASSERT_EQUAL_INT((int)LightPattern::SOLID, (int)s.wheeliePattern);
    TEST_ASSERT_EQUAL_INT((int)LightPattern::STROBE, (int)s.warningPattern);
    TEST_ASSERT_EQUAL_UINT8(255, s.warningBrightness);
}

void test_absolute_angle_processing() {
    ControllerSettings s;
    float baseline = 12.0f;
    bool frozen = true;
    float result = processTriggerPitch(
        17.5f, 0.0f, 0.01f, true, true, s, baseline, frozen);

    TEST_ASSERT_FLOAT_WITHIN(0.001f, 17.5f, result);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, baseline);
    TEST_ASSERT_FALSE(frozen);
}

void test_adaptive_baseline_tracks_slow_terrain() {
    ControllerSettings s;
    s.angleMode = AngleMode::ADAPTIVE;
    s.adaptiveTimeConstantSec = 1.0f;
    float baseline = 0.0f;
    bool frozen = false;
    float triggerPitch = 0.0f;

    for (int i = 0; i < 500; ++i) {
        triggerPitch = processTriggerPitch(
            10.0f, 0.5f, 0.01f, true, true, s, baseline, frozen);
    }

    TEST_ASSERT_FALSE(frozen);
    TEST_ASSERT_FLOAT_WITHIN(0.08f, 10.0f, baseline);
    TEST_ASSERT_FLOAT_WITHIN(0.08f, 0.0f, triggerPitch);
}

void test_adaptive_baseline_freezes_during_fast_pitch_or_wheelie() {
    ControllerSettings s;
    s.angleMode = AngleMode::ADAPTIVE;
    float baseline = 3.0f;
    bool frozen = false;

    float pitch = processTriggerPitch(
        25.0f, s.adaptiveFreezeRateDegSec, 0.1f,
        true, true, s, baseline, frozen);
    TEST_ASSERT_TRUE(frozen);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.0f, baseline);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 22.0f, pitch);

    frozen = false;
    pitch = processTriggerPitch(
        26.0f, 0.0f, 0.1f, true, false, s, baseline, frozen);
    TEST_ASSERT_TRUE(frozen);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.0f, baseline);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 23.0f, pitch);
}

void test_warning_angle_hysteresis() {
    ControllerSettings s;
    bool warning = false;
    warning = stepHighAngleWarning(warning, 45.0f, 0.0f, s);
    TEST_ASSERT_TRUE(warning);
    warning = stepHighAngleWarning(warning, 42.0f, 0.0f, s);
    TEST_ASSERT_TRUE(warning);
    warning = stepHighAngleWarning(warning, 40.0f, 0.0f, s);
    TEST_ASSERT_FALSE(warning);
}

void test_warning_pitch_rate_early_trigger_and_release() {
    ControllerSettings s;
    bool warning = stepHighAngleWarning(false, 20.0f, 45.0f, s);
    TEST_ASSERT_TRUE(warning);
    warning = stepHighAngleWarning(warning, 10.0f, 30.0f, s);
    TEST_ASSERT_TRUE(warning);
    warning = stepHighAngleWarning(warning, 10.0f, 22.0f, s);
    TEST_ASSERT_FALSE(warning);
}

void test_light_patterns() {
    TEST_ASSERT_EQUAL_UINT8(0, calculatePatternBrightness(LightPattern::OFF, 200, 0));
    TEST_ASSERT_EQUAL_UINT8(200, calculatePatternBrightness(LightPattern::SOLID, 200, 999));
    TEST_ASSERT_EQUAL_UINT8(200, calculatePatternBrightness(LightPattern::STROBE, 200, 94));
    TEST_ASSERT_EQUAL_UINT8(0, calculatePatternBrightness(LightPattern::STROBE, 200, 95));
    TEST_ASSERT_EQUAL_UINT8(36, calculatePatternBrightness(LightPattern::SLOW_PULSE, 200, 0));
    TEST_ASSERT_EQUAL_UINT8(200, calculatePatternBrightness(LightPattern::SLOW_PULSE, 200, 800));
}

void test_fade_interpolation() {
    TEST_ASSERT_EQUAL_UINT8(0, calculateFadeBrightness(0, 200, 0, 1000));
    TEST_ASSERT_EQUAL_UINT8(50, calculateFadeBrightness(0, 200, 250, 1000));
    TEST_ASSERT_EQUAL_UINT8(100, calculateFadeBrightness(200, 0, 500, 1000));
    TEST_ASSERT_EQUAL_UINT8(200, calculateFadeBrightness(0, 200, 1000, 1000));
    TEST_ASSERT_EQUAL_UINT8(200, calculateFadeBrightness(0, 200, 0, 0));
}

void test_full_wheelie_profile() {
    ControllerSettings s;
    s.triggerHoldMs = 150;
    s.minimumOnTimeMs = 500;
    ControllerState state = ControllerState::NORMAL;
    unsigned long triggerStart = 0;
    unsigned long wheelieStart = 0;

    TEST_ASSERT_FALSE(stepController(state, 0.0f, 0, s, triggerStart, wheelieStart));
    TEST_ASSERT_FALSE(stepController(state, 25.0f, 100, s, triggerStart, wheelieStart));
    TEST_ASSERT_FALSE(stepController(state, 30.0f, 249, s, triggerStart, wheelieStart));
    TEST_ASSERT_TRUE(stepController(state, 35.0f, 250, s, triggerStart, wheelieStart));
    TEST_ASSERT_TRUE(stepController(state, 5.0f, 749, s, triggerStart, wheelieStart));
    TEST_ASSERT_FALSE(stepController(state, 5.0f, 750, s, triggerStart, wheelieStart));
    TEST_ASSERT_EQUAL_INT((int)ControllerState::NORMAL, (int)state);
}

static void runAllTests() {
    RUN_TEST(test_trigger_hold);
    RUN_TEST(test_pending_cancellation);
    RUN_TEST(test_minimum_on_and_hysteresis);
    RUN_TEST(test_millis_rollover);
    RUN_TEST(test_invalid_settings);
    RUN_TEST(test_warning_and_pattern_settings_validation);
    RUN_TEST(test_absolute_angle_processing);
    RUN_TEST(test_adaptive_baseline_tracks_slow_terrain);
    RUN_TEST(test_adaptive_baseline_freezes_during_fast_pitch_or_wheelie);
    RUN_TEST(test_warning_angle_hysteresis);
    RUN_TEST(test_warning_pitch_rate_early_trigger_and_release);
    RUN_TEST(test_light_patterns);
    RUN_TEST(test_fade_interpolation);
    RUN_TEST(test_full_wheelie_profile);
}

#ifdef ARDUINO
void setup() {
    delay(2000);
    UNITY_BEGIN();
    runAllTests();
    UNITY_END();
}
void loop() {}
#else
int main(int, char**) {
    UNITY_BEGIN();
    runAllTests();
    return UNITY_END();
}
#endif
