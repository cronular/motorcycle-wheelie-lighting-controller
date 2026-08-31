#ifndef RIDE_LOG_FORMAT_H
#define RIDE_LOG_FORMAT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

constexpr char RIDE_LOG_MAGIC[4] = {'W', 'R', 'L', '1'};
constexpr uint16_t RIDE_LOG_FORMAT_VERSION = 1;
constexpr uint16_t RIDE_LOG_SAMPLE_RATE_HZ = 5;
constexpr uint32_t RIDE_LOG_SAMPLE_INTERVAL_MS = 1000 / RIDE_LOG_SAMPLE_RATE_HZ;
constexpr uint8_t RIDE_LOG_MAX_SESSIONS = 3;
constexpr uint32_t RIDE_LOG_MAX_DURATION_MS = 90UL * 60UL * 1000UL;
constexpr bool RIDE_LOG_DEFAULT_ENABLED = false;

enum RideHeaderFlags : uint8_t {
    RIDE_HEADER_COMPLETE = 1 << 0,
    RIDE_HEADER_RECOVERED = 1 << 1,
    RIDE_HEADER_CAPACITY_REACHED = 1 << 2,
};

enum RideSampleFlags : uint8_t {
    RIDE_SAMPLE_IMU_OK = 1 << 0,
    RIDE_SAMPLE_PENDING = 1 << 1,
    RIDE_SAMPLE_WHEELIE = 1 << 2,
    RIDE_SAMPLE_WARNING = 1 << 3,
    RIDE_SAMPLE_BASELINE_FROZEN = 1 << 4,
};

#pragma pack(push, 1)
struct RideLogHeader {
    char magic[4] = {'W', 'R', 'L', '1'};
    uint16_t formatVersion = RIDE_LOG_FORMAT_VERSION;
    uint16_t headerSize = sizeof(RideLogHeader);
    uint16_t sampleSize = 0;
    uint16_t sampleRateHz = RIDE_LOG_SAMPLE_RATE_HZ;
    uint32_t sessionId = 0;
    uint32_t startUptimeMs = 0;
    uint32_t durationMs = 0;
    uint32_t sampleCount = 0;
    uint32_t wheelieCount = 0;
    int16_t peakPitchCentiDeg = 0;
    int16_t peakRollCentiDeg = 0;
    int16_t peakGyroCentiDegSec = 0;
    uint16_t peakGMillig = 0;
    char firmware[24] = {};
    char commit[16] = {};
    char buildDate[24] = {};
    char channel[8] = {};
    char board[24] = {};
    uint8_t rotationAxis = 0;
    uint8_t rollAxis = 0;
    uint8_t verticalAxis = 0;
    uint8_t flags = 0;
    uint8_t sampleSha256[32] = {};
    uint8_t reserved[20] = {};
};

struct RideTelemetrySample {
    uint32_t elapsedMs = 0;
    int16_t pitchCentiDeg = 0;
    int16_t rawPitchCentiDeg = 0;
    int16_t rollCentiDeg = 0;
    int16_t gyroCentiDegSec = 0;
    int16_t gLoadMillig = 0;
    uint8_t outputPercent = 0;
    uint8_t flags = 0;
};
#pragma pack(pop)

constexpr size_t RIDE_LOG_MAX_SAMPLES = RIDE_LOG_MAX_DURATION_MS / RIDE_LOG_SAMPLE_INTERVAL_MS;
constexpr size_t RIDE_LOG_MAX_FILE_BYTES =
    sizeof(RideLogHeader) + RIDE_LOG_MAX_SAMPLES * sizeof(RideTelemetrySample);

inline bool isRideLogHeaderValid(const RideLogHeader& header) {
    return memcmp(header.magic, RIDE_LOG_MAGIC, sizeof(RIDE_LOG_MAGIC)) == 0 &&
           header.formatVersion == RIDE_LOG_FORMAT_VERSION &&
           header.headerSize == sizeof(RideLogHeader) &&
           header.sampleSize == sizeof(RideTelemetrySample) &&
           header.sampleRateHz == RIDE_LOG_SAMPLE_RATE_HZ &&
           header.sessionId > 0;
}

static_assert(sizeof(RideLogHeader) == 192, "Ride log header size changed");
static_assert(sizeof(RideTelemetrySample) == 16, "Ride sample must remain compact");
static_assert(RIDE_LOG_MAX_SAMPLES == 27000, "Ride capacity calculation changed");
static_assert(RIDE_LOG_MAX_FILE_BYTES == 432192, "Ride file budget changed");
static_assert(RIDE_LOG_MAX_SESSIONS * RIDE_LOG_MAX_FILE_BYTES <= 0x180000 - 256 * 1024,
              "Ride logs must leave at least 256 KiB free in the filesystem");

#endif
