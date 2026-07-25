#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace cella {
namespace waterfall {

constexpr int NUM_FREQUENCY_CELLS = 512;
constexpr int ROW_QUEUE_SIZE = 64;
constexpr int MARKER_QUEUE_SIZE = 64;
constexpr int MAX_RETAINED_MARKERS = 128;
constexpr int TIME_LOOKUP_SIZE = 1024;
constexpr float MIN_FREQUENCY_HZ = 20.f;
constexpr float INTERNAL_FLOOR_DB = -160.f;
constexpr float INTERNAL_CEILING_DB = 24.f;
constexpr float VOLTAGE_TO_FULL_SCALE = 0.1f;
constexpr float MAX_SAFE_VOLTAGE = 100.f;

enum class ChannelMode : int { LEFT, RIGHT, MONO, MID, SIDE, COUNT };
enum class FftSize : int { FFT_1024, FFT_2048, FFT_4096, FFT_8192, FFT_16384, COUNT };
enum class WindowFunction : int { HANN, BLACKMAN_HARRIS, FLAT_TOP, COUNT };
enum class FftOverlap : int { NONE, PERCENT_25, PERCENT_50, PERCENT_75, PERCENT_87_5, PERCENT_93_75, COUNT };
enum class Quality : int { ECONOMY, NORMAL, HIGH, COUNT };
// Keep the first three values stable so patches saved by Waterfall Slice 1/2
// continue to select the same palette.
enum class Palette : int {
    HEAT,
    GRAY,
    VIRIDIS,
    AMBER,
    AMETHYST,
    APPLE,
    ARCTIC,
    BUBBLEGUM,
    CHROMA,
    CIVIDIS,
    COSMIC,
    CUBEHELIX,
    DUSK,
    ECLIPSE,
    EMBER,
    EMERALD,
    FALL,
    FLAMINGO,
    FREEZE,
    GEM,
    GHOSTLIGHT,
    GOTHIC,
    HORIZON,
    INFERNO,
    JUNGLE,
    LAVENDER,
    LILAC,
    MAGMA,
    MAKO,
    NEON,
    NUCLEAR,
    OCEAN,
    PEPPER,
    PLASMA,
    RAINFOREST,
    ROCKET,
    SAPPHIRE,
    SAVANNA,
    SEPIA,
    SUNBURST,
    SWAMP,
    TORCH,
    TOXIC,
    TREE,
    TROPICAL,
    TURBO,
    VOLTAGE,
    COUNT
};
enum class PeakHold : int { OFF, DECAY, INFINITE, COUNT };
enum class FlowDirection : int { UP, DOWN, LEFT, RIGHT, COUNT };
enum class RenderingStyle : int { PRECISE, SMOOTH, COUNT };
enum class LiveTraceMode : int { OFF, LINE, LINE_FILL, COUNT };
enum class FrequencyScaleMode : int { HZ, OCTAVES, MUSICAL, COMBINED, COUNT };
enum class FrequencySmoothing : int {
    NONE,
    OCTAVE_1_48,
    OCTAVE_1_24,
    OCTAVE_1_12,
    OCTAVE_1_6,
    OCTAVE_1_3,
    COUNT
};
enum class TemporalSmoothing : int { OFF, FAST, MEDIUM, SLOW, COUNT };
enum class HistoryDuration : int { SECONDS_2, SECONDS_4, SECONDS_8, SECONDS_16, SECONDS_30, COUNT };

constexpr std::array<int, static_cast<int>(FftSize::COUNT)> FFT_SIZES = {{1024, 2048, 4096, 8192, 16384}};
constexpr std::array<int, static_cast<int>(FftOverlap::COUNT)> FFT_HOP_SIXTEENTHS = {{16, 12, 8, 4, 2, 1}};
constexpr std::array<int, static_cast<int>(Quality::COUNT)> ROW_RATES = {{15, 30, 60}};
constexpr std::array<int, static_cast<int>(HistoryDuration::COUNT)> HISTORY_DURATIONS = {{2, 4, 8, 16, 30}};
constexpr float MAX_FFT_ANALYSES_PER_SECOND = 240.f;

template <typename T>
inline T clampValue(T value, T low, T high) {
    return value < low ? low : (value > high ? high : value);
}

inline float sanitizeVoltage(float voltage) {
    if (!std::isfinite(voltage)) return 0.f;
    return clampValue(voltage, -MAX_SAFE_VOLTAGE, MAX_SAFE_VOLTAGE);
}

/**
 * Applies the channel definitions before the fixed 10 V peak = 0 dBFS scale.
 * The R input is normalled from L when R is not connected.
 */
inline float mixInputVoltages(float leftVoltage, float rightVoltage, bool leftConnected, bool rightConnected,
                              ChannelMode mode) {
    const float left = leftConnected ? sanitizeVoltage(leftVoltage) : 0.f;
    const float right = rightConnected ? sanitizeVoltage(rightVoltage) : (leftConnected ? left : 0.f);

    switch (mode) {
        case ChannelMode::LEFT:
            return left;
        case ChannelMode::RIGHT:
            return right;
        case ChannelMode::MONO:
            if (leftConnected && rightConnected) return 0.5f * (left + right);
            if (leftConnected) return left;
            return rightConnected ? right : 0.f;
        case ChannelMode::MID:
            return 0.5f * (left + right);
        case ChannelMode::SIDE:
            return 0.5f * (left - right);
        default:
            return 0.f;
    }
}

inline int fftSizeSamples(FftSize size) {
    return FFT_SIZES[static_cast<int>(size)];
}

inline int requestedFftHopSize(int fftSize, FftOverlap overlap) {
    const int safeSize = std::max(fftSize, 1);
    const int overlapIndex =
        clampValue(static_cast<int>(overlap), 0, static_cast<int>(FftOverlap::COUNT) - 1);
    const int hopSixteenths = FFT_HOP_SIXTEENTHS[static_cast<size_t>(overlapIndex)];
    return std::max(1, (safeSize * hopSixteenths + 15) / 16);
}

inline int effectiveFftHopSize(int fftSize, float sampleRate, FftOverlap overlap) {
    const int safeSize = std::max(fftSize, 1);
    const float safeSampleRate =
        std::isfinite(sampleRate) ? std::max(sampleRate, 1000.f) : 48000.f;
    const int cadenceLimitedHop = std::max(
        1, static_cast<int>(std::ceil(static_cast<double>(safeSampleRate) /
                                     static_cast<double>(MAX_FFT_ANALYSES_PER_SECOND))));
    // A hop larger than the FFT would leave gaps in the analyzed signal. At
    // extreme sample rates, continuous coverage therefore takes precedence
    // over the soft analysis-cadence ceiling.
    return std::min(safeSize, std::max(requestedFftHopSize(safeSize, overlap), cadenceLimitedHop));
}

inline int rowsPerSecond(Quality quality) {
    return ROW_RATES[static_cast<int>(quality)];
}

inline int historyDurationSeconds(HistoryDuration duration) {
    return HISTORY_DURATIONS[static_cast<int>(duration)];
}

inline int historyRowCapacity(float seconds, int effectiveRowsPerSecond) {
    return std::max(4, static_cast<int>(std::ceil(std::max(seconds, 0.25f) *
                                                  std::max(effectiveRowsPerSecond, 1))) +
                           2);
}

inline float frequencySmoothingOctaves(FrequencySmoothing smoothing) {
    switch (smoothing) {
        case FrequencySmoothing::OCTAVE_1_48:
            return 1.f / 48.f;
        case FrequencySmoothing::OCTAVE_1_24:
            return 1.f / 24.f;
        case FrequencySmoothing::OCTAVE_1_12:
            return 1.f / 12.f;
        case FrequencySmoothing::OCTAVE_1_6:
            return 1.f / 6.f;
        case FrequencySmoothing::OCTAVE_1_3:
            return 1.f / 3.f;
        default:
            return 0.f;
    }
}

inline void temporalTimeConstants(TemporalSmoothing smoothing, float& attackSeconds, float& releaseSeconds) {
    switch (smoothing) {
        case TemporalSmoothing::FAST:
            attackSeconds = 0.025f;
            releaseSeconds = 0.250f;
            break;
        case TemporalSmoothing::MEDIUM:
            attackSeconds = 0.100f;
            releaseSeconds = 0.700f;
            break;
        case TemporalSmoothing::SLOW:
            attackSeconds = 0.300f;
            releaseSeconds = 1.500f;
            break;
        default:
            attackSeconds = releaseSeconds = 0.f;
            break;
    }
}

inline bool isVerticalFlow(FlowDirection flow) {
    return flow == FlowDirection::UP || flow == FlowDirection::DOWN;
}

struct LogicalPoint {
    LogicalPoint(float frequency = 0.f, float age = 0.f) : frequency(frequency), age(age) {}
    float frequency;
    float age;
};

/**
 * Converts bottom-origin normalized display coordinates to the common
 * frequency/age domain used by the shader and interaction layer.
 */
inline LogicalPoint logicalFromScreen(FlowDirection flow, float x, float y) {
    x = clampValue(x, 0.f, 1.f);
    y = clampValue(y, 0.f, 1.f);
    switch (flow) {
        case FlowDirection::UP:
            return {x, y};
        case FlowDirection::DOWN:
            return {x, 1.f - y};
        case FlowDirection::LEFT:
            return {y, 1.f - x};
        case FlowDirection::RIGHT:
            return {y, x};
        default:
            return {x, y};
    }
}

inline void screenFromLogical(FlowDirection flow, const LogicalPoint& logical, float& x, float& y) {
    const float frequency = clampValue(logical.frequency, 0.f, 1.f);
    const float age = clampValue(logical.age, 0.f, 1.f);
    switch (flow) {
        case FlowDirection::UP:
            x = frequency;
            y = age;
            break;
        case FlowDirection::DOWN:
            x = frequency;
            y = 1.f - age;
            break;
        case FlowDirection::LEFT:
            x = 1.f - age;
            y = frequency;
            break;
        case FlowDirection::RIGHT:
            x = age;
            y = frequency;
            break;
        default:
            x = frequency;
            y = age;
            break;
    }
}

struct WaterfallConfig {
    FftSize fftSize = FftSize::FFT_4096;
    WindowFunction window = WindowFunction::HANN;
    FftOverlap fftOverlap = FftOverlap::PERCENT_75;
    Quality quality = Quality::NORMAL;
    ChannelMode channelMode = ChannelMode::MONO;
    int polyChannel = 0;
    float sampleRate = 48000.f;
    uint64_t generation = 1;
};

struct SpectrumRow {
    std::array<int16_t, NUM_FREQUENCY_CELLS> dbTenths = {};
    uint64_t rowEndSample = 0;
    uint64_t sourceAnalysisSample = 0;
    float sampleRate = 0.f;
    uint32_t fftSize = 0;
    uint32_t effectiveHopSize = 0;
    uint32_t displayRowsPerSecond = 0;
    uint64_t configGeneration = 0;
};

struct MarkerEvent {
    uint64_t timelineSample = 0;
    float sampleRate = 0.f;
    uint64_t configGeneration = 0;
    uint32_t sequence = 0;
};

inline int16_t quantizeDb(float db) {
    const float finite = std::isfinite(db) ? db : INTERNAL_FLOOR_DB;
    return static_cast<int16_t>(
        std::lround(clampValue(finite, INTERNAL_FLOOR_DB, INTERNAL_CEILING_DB) * 10.f));
}

inline float dequantizeDb(int16_t dbTenths) {
    return static_cast<float>(dbTenths) * 0.1f;
}

inline unsigned char encodeDb(float db) {
    const float normalized =
        (clampValue(db, INTERNAL_FLOOR_DB, INTERNAL_CEILING_DB) - INTERNAL_FLOOR_DB) /
        (INTERNAL_CEILING_DB - INTERNAL_FLOOR_DB);
    return static_cast<unsigned char>(std::lround(normalized * 255.f));
}

inline uint16_t encodeDb16(float db) {
    const float normalized =
        (clampValue(db, INTERNAL_FLOOR_DB, INTERNAL_CEILING_DB) - INTERNAL_FLOOR_DB) /
        (INTERNAL_CEILING_DB - INTERNAL_FLOOR_DB);
    return static_cast<uint16_t>(std::lround(normalized * 65535.f));
}

}  // namespace waterfall
}  // namespace cella
