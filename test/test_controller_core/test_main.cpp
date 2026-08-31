#ifdef ARDUINO
#include <Arduino.h>
#endif
#include <unity.h>
#include "controller_core.h"
#include "ride_log_format.h"
#include "rider_model.h"

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

void test_adaptive_baseline_reports_specific_freeze_reasons() {
    ControllerSettings s;
    s.angleMode = AngleMode::ADAPTIVE;
    float baseline = 0.0f;
    bool frozen = false;
    AdaptiveFreezeReason reason = AdaptiveFreezeReason::NONE;

    processTriggerPitch(5.0f, 0.0f, 0.01f, false, true, s,
        baseline, frozen, &reason);
    TEST_ASSERT_EQUAL_INT((int)AdaptiveFreezeReason::IMU_UNHEALTHY, (int)reason);
    processTriggerPitch(5.0f, 0.0f, 0.01f, true, false, s,
        baseline, frozen, &reason);
    TEST_ASSERT_EQUAL_INT((int)AdaptiveFreezeReason::CONTROLLER_ACTIVE, (int)reason);
    processTriggerPitch(5.0f, 9.0f, 0.01f, true, true, s,
        baseline, frozen, &reason);
    TEST_ASSERT_EQUAL_INT((int)AdaptiveFreezeReason::MOTION, (int)reason);
    processTriggerPitch(5.0f, 0.0f, 0.01f, true, true, s,
        baseline, frozen, &reason, 0.0f);
    TEST_ASSERT_EQUAL_INT((int)AdaptiveFreezeReason::ACCELERATION, (int)reason);
}

void test_time_based_attitude_filter_rejects_external_acceleration() {
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.98f, timeConstantGyroWeight(0.49f, 0.01f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, calculateAccelerationTrust(1.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, calculateAccelerationTrust(1.2f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f,
        updateAdaptiveComplementaryAngle(0.0f, 100.0f, 40.0f, 0.01f, 0.0f));
    TEST_ASSERT_TRUE(
        updateAdaptiveComplementaryAngle(0.0f, 0.0f, 40.0f, 0.01f, 1.0f) > 0.0f);
}

void test_noise_aware_freeze_rate_is_bounded() {
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 8.0f, calculateNoiseAwareFreezeRate(8.0f, 1.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.0f, calculateNoiseAwareFreezeRate(8.0f, 4.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, calculateNoiseAwareFreezeRate(8.0f, 20.0f));
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

void test_warning_off_does_not_suppress_wheelie_output() {
    TEST_ASSERT_EQUAL_UINT8(180, calculateRequestedBrightness(
        true, true, LightPattern::OFF, 255,
        LightPattern::SOLID, 180, 100));
    TEST_ASSERT_EQUAL_UINT8(220, calculateRequestedBrightness(
        true, true, LightPattern::SOLID, 220,
        LightPattern::SOLID, 180, 100));
    TEST_ASSERT_EQUAL_UINT8(0, calculateRequestedBrightness(
        true, false, LightPattern::OFF, 255,
        LightPattern::SOLID, 180, 100));
}

void test_rider_profile_and_shadow_event_are_bounded() {
    TEST_ASSERT_FALSE(RIDER_MODEL_DEFAULT_ENABLED);
    RiderProfile profile;
    updateStableRiderProfile(profile, -1.0f, 0.01f);
    updateStableRiderProfile(profile, 1.0f, 0.03f);
    TEST_ASSERT_EQUAL_UINT32(2, profile.stableSamples);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.4142f, riderProfileGyroRms(profile));

    ModelEventAccumulator accumulator;
    beginModelEvent(accumulator, 7, 1000);
    addModelEventSample(accumulator, 1000, 0.0f, 0.0f, 0.0f, 2.0f, false, 20.0f);
    addModelEventSample(accumulator, 1020, 25.0f, 60.0f, 0.4f, 4.0f, true, 20.0f);
    ModelEventFeatures event = finishModelEvent(
        accumulator, 1040, ModelEventOutcome::DETECTED);
    TEST_ASSERT_EQUAL_UINT32(7, event.id);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, event.pitchRise);
    TEST_ASSERT_TRUE(event.shadowScore > 0.0f && event.shadowScore < 1.0f);

    ModelEventHistory history;
    for (uint32_t id = 1; id <= EVENT_HISTORY_CAPACITY + 2; ++id) {
        event.id = id;
        appendModelEvent(history, event);
    }
    TEST_ASSERT_EQUAL_UINT8(EVENT_HISTORY_CAPACITY, history.count);
    TEST_ASSERT_EQUAL_UINT32(EVENT_HISTORY_CAPACITY + 3, history.nextId);
    TEST_ASSERT_NOT_NULL(findModelEvent(history, EVENT_HISTORY_CAPACITY + 2));
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

void test_orientation_detects_z_vertical_x_roll_y_pitch() {
    const OrientationResult result = detectOrientationAxes(
        0.03f, -0.02f, 1.01f, 12.0f, 1.5f, 0.8f);
    TEST_ASSERT_TRUE(result.valid);
    TEST_ASSERT_EQUAL_INT((int)RotationAxis::Z, (int)result.verticalAxis);
    TEST_ASSERT_EQUAL_INT((int)RotationAxis::X, (int)result.rollAxis);
    TEST_ASSERT_EQUAL_INT((int)RotationAxis::Y, (int)result.pitchAxis);
}

void test_orientation_detects_y_vertical_z_roll_x_pitch() {
    const OrientationResult result = detectOrientationAxes(
        0.04f, -0.98f, 0.08f, 2.0f, 0.6f, 9.0f);
    TEST_ASSERT_TRUE(result.valid);
    TEST_ASSERT_EQUAL_INT((int)RotationAxis::Y, (int)result.verticalAxis);
    TEST_ASSERT_EQUAL_INT((int)RotationAxis::Z, (int)result.rollAxis);
    TEST_ASSERT_EQUAL_INT((int)RotationAxis::X, (int)result.pitchAxis);
}

void test_orientation_rejects_insufficient_lean_motion() {
    const OrientationResult result = detectOrientationAxes(
        0.0f, 0.0f, 1.0f, 1.5f, 0.4f, 0.2f);
    TEST_ASSERT_FALSE(result.valid);
}

void test_orientation_rejects_ambiguous_motion() {
    const OrientationResult result = detectOrientationAxes(
        0.0f, 1.0f, 0.0f, 6.0f, 0.2f, 5.5f);
    TEST_ASSERT_FALSE(result.valid);
}

void test_relative_rotation_around_x_is_signed() {
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 30.0f, calculateRelativeRotationDegrees(
        RotationAxis::X, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 0.8660254f));
    TEST_ASSERT_FLOAT_WITHIN(0.05f, -30.0f, calculateRelativeRotationDegrees(
        RotationAxis::X, 0.0f, 0.0f, 1.0f, 0.0f, -0.5f, 0.8660254f));
}

void test_relative_rotation_around_y_is_signed() {
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 30.0f, calculateRelativeRotationDegrees(
        RotationAxis::Y, 0.0f, 0.0f, 1.0f, -0.5f, 0.0f, 0.8660254f));
    TEST_ASSERT_FLOAT_WITHIN(0.05f, -30.0f, calculateRelativeRotationDegrees(
        RotationAxis::Y, 0.0f, 0.0f, 1.0f, 0.5f, 0.0f, 0.8660254f));
}

void test_relative_rotation_handles_y_vertical_z_roll() {
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 30.0f, calculateRelativeRotationDegrees(
        RotationAxis::Z, 0.0f, 1.0f, 0.0f, 0.5f, 0.8660254f, 0.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.05f, -30.0f, calculateRelativeRotationDegrees(
        RotationAxis::Z, 0.0f, 1.0f, 0.0f, -0.5f, 0.8660254f, 0.0f));
}

void test_firmware_package_header_helpers() {
    uint8_t header[FIRMWARE_PACKAGE_HEADER_SIZE] = {
        'W', 'C', 'T', 'R', 'L', '1', '\r', '\n',
        0x34, 0x12, 0x48, 0x00, 0x78, 0x56, 0x34, 0x12
    };
    TEST_ASSERT_TRUE(hasFirmwarePackageMagic(header, sizeof(header)));
    TEST_ASSERT_EQUAL_UINT16(0x1234, readLittleEndian16(header + 8));
    TEST_ASSERT_EQUAL_UINT16(0x0048, readLittleEndian16(header + 10));
    TEST_ASSERT_EQUAL_UINT32(0x12345678, readLittleEndian32(header + 12));
    header[0] = 'X';
    TEST_ASSERT_FALSE(hasFirmwarePackageMagic(header, sizeof(header)));
}

void test_firmware_metadata_requires_board_and_channel() {
    TEST_ASSERT_TRUE(isFirmwareMetadataCompatible(
        "seeed_xiao_esp32s3", "testing", "seeed_xiao_esp32s3", "testing"));
    TEST_ASSERT_FALSE(isFirmwareMetadataCompatible(
        "esp32dev", "testing", "seeed_xiao_esp32s3", "testing"));
    TEST_ASSERT_FALSE(isFirmwareMetadataCompatible(
        "seeed_xiao_esp32s3", "stable", "seeed_xiao_esp32s3", "testing"));
}

void test_write_rate_limit_blocks_burst() {
    WriteRateLimitBucket buckets[2];
    for (uint8_t request = 0; request < 3; ++request) {
        TEST_ASSERT_EQUAL_INT((int)WriteRateLimitDecision::ALLOW, (int)checkWriteRateLimit(
            buckets, 2, 0x01020304, 1000 + request, 3, 10000, 30000));
    }
    TEST_ASSERT_EQUAL_INT((int)WriteRateLimitDecision::BLOCK, (int)checkWriteRateLimit(
        buckets, 2, 0x01020304, 1004, 3, 10000, 30000));
}

void test_write_rate_limit_is_per_client() {
    WriteRateLimitBucket buckets[2];
    TEST_ASSERT_EQUAL_INT((int)WriteRateLimitDecision::ALLOW, (int)checkWriteRateLimit(
        buckets, 2, 1, 1000, 1, 10000, 30000));
    TEST_ASSERT_EQUAL_INT((int)WriteRateLimitDecision::BLOCK, (int)checkWriteRateLimit(
        buckets, 2, 1, 1001, 1, 10000, 30000));
    TEST_ASSERT_EQUAL_INT((int)WriteRateLimitDecision::ALLOW, (int)checkWriteRateLimit(
        buckets, 2, 2, 1002, 1, 10000, 30000));
}

void test_write_rate_limit_recovers_after_block() {
    WriteRateLimitBucket buckets[1];
    checkWriteRateLimit(buckets, 1, 7, 1000, 1, 10000, 30000);
    TEST_ASSERT_EQUAL_INT((int)WriteRateLimitDecision::BLOCK, (int)checkWriteRateLimit(
        buckets, 1, 7, 1001, 1, 10000, 30000));
    TEST_ASSERT_EQUAL_INT((int)WriteRateLimitDecision::ALLOW, (int)checkWriteRateLimit(
        buckets, 1, 7, 31002, 1, 10000, 30000));
}

void test_write_rate_limit_handles_millis_rollover() {
    WriteRateLimitBucket buckets[1];
    const uint32_t nearRollover = UINT32_MAX - 100;
    checkWriteRateLimit(buckets, 1, 9, nearRollover, 1, 10000, 30000);
    TEST_ASSERT_EQUAL_INT((int)WriteRateLimitDecision::BLOCK, (int)checkWriteRateLimit(
        buckets, 1, 9, nearRollover + 1, 1, 10000, 30000));
    TEST_ASSERT_EQUAL_INT((int)WriteRateLimitDecision::ALLOW, (int)checkWriteRateLimit(
        buckets, 1, 9, nearRollover + 30001, 1, 10000, 30000));
}

void test_ride_log_storage_budget() {
    TEST_ASSERT_EQUAL_UINT16(5, RIDE_LOG_SAMPLE_RATE_HZ);
    TEST_ASSERT_EQUAL_UINT32(90UL * 60UL * 1000UL, RIDE_LOG_MAX_DURATION_MS);
    TEST_ASSERT_EQUAL_UINT32(27000, RIDE_LOG_MAX_SAMPLES);
    TEST_ASSERT_EQUAL_UINT32(432192, RIDE_LOG_MAX_FILE_BYTES);
    TEST_ASSERT_TRUE(RIDE_LOG_MAX_SESSIONS * RIDE_LOG_MAX_FILE_BYTES <=
                     0x180000 - 256 * 1024);
}

void test_ride_log_compact_record_layout() {
    TEST_ASSERT_EQUAL_UINT32(192, sizeof(RideLogHeader));
    TEST_ASSERT_EQUAL_UINT32(16, sizeof(RideTelemetrySample));
    TEST_ASSERT_EQUAL_UINT8(3, RIDE_LOG_MAX_SESSIONS);
}

void test_ride_log_header_validation_and_opt_in_default() {
    TEST_ASSERT_FALSE(RIDE_LOG_DEFAULT_ENABLED);
    RideLogHeader header;
    TEST_ASSERT_FALSE(isRideLogHeaderValid(header));
    header.sampleSize = sizeof(RideTelemetrySample);
    header.sessionId = 1;
    TEST_ASSERT_TRUE(isRideLogHeaderValid(header));
    header.sampleRateHz = 10;
    TEST_ASSERT_FALSE(isRideLogHeaderValid(header));
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
    RUN_TEST(test_adaptive_baseline_reports_specific_freeze_reasons);
    RUN_TEST(test_time_based_attitude_filter_rejects_external_acceleration);
    RUN_TEST(test_noise_aware_freeze_rate_is_bounded);
    RUN_TEST(test_warning_angle_hysteresis);
    RUN_TEST(test_warning_pitch_rate_early_trigger_and_release);
    RUN_TEST(test_light_patterns);
    RUN_TEST(test_warning_off_does_not_suppress_wheelie_output);
    RUN_TEST(test_rider_profile_and_shadow_event_are_bounded);
    RUN_TEST(test_fade_interpolation);
    RUN_TEST(test_full_wheelie_profile);
    RUN_TEST(test_orientation_detects_z_vertical_x_roll_y_pitch);
    RUN_TEST(test_orientation_detects_y_vertical_z_roll_x_pitch);
    RUN_TEST(test_orientation_rejects_insufficient_lean_motion);
    RUN_TEST(test_orientation_rejects_ambiguous_motion);
    RUN_TEST(test_relative_rotation_around_x_is_signed);
    RUN_TEST(test_relative_rotation_around_y_is_signed);
    RUN_TEST(test_relative_rotation_handles_y_vertical_z_roll);
    RUN_TEST(test_firmware_package_header_helpers);
    RUN_TEST(test_firmware_metadata_requires_board_and_channel);
    RUN_TEST(test_write_rate_limit_blocks_burst);
    RUN_TEST(test_write_rate_limit_is_per_client);
    RUN_TEST(test_write_rate_limit_recovers_after_block);
    RUN_TEST(test_write_rate_limit_handles_millis_rollover);
    RUN_TEST(test_ride_log_storage_budget);
    RUN_TEST(test_ride_log_compact_record_layout);
    RUN_TEST(test_ride_log_header_validation_and_opt_in_default);
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
