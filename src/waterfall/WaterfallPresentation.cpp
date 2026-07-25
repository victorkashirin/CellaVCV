#include "WaterfallPresentation.hpp"

#include <algorithm>
#include <cmath>

namespace cella {
namespace waterfall {

namespace {
constexpr float POWER_EPSILON = 1e-16f;

float dbToPower(float db) {
    return std::pow(10.f, clampValue(db, INTERNAL_FLOOR_DB, INTERNAL_CEILING_DB) * 0.1f);
}

float powerToDb(float power) {
    return clampValue(10.f * std::log10(std::max(power, POWER_EPSILON)), INTERNAL_FLOOR_DB,
                      INTERNAL_CEILING_DB);
}
}  // namespace

void FrequencySmoothingKernel::configure(FrequencySmoothing smoothing, float sampleRate) {
    const float width = frequencySmoothingOctaves(smoothing);
    const float cellOctaves =
        std::log2(std::max(sampleRate * 0.5f, MIN_FREQUENCY_HZ * 1.01f) / MIN_FREQUENCY_HZ) /
        NUM_FREQUENCY_CELLS;
    for (int center = 0; center < NUM_FREQUENCY_CELLS; ++center) {
        TapRange& range = ranges[static_cast<size_t>(center)];
        range.weights.clear();
        if (!(width > 0.f)) {
            range.first = center;
            range.weights.push_back(1.f);
            continue;
        }
        // Cell centers are uniformly spaced in log-frequency. Treat the
        // selected fractional octave as the Gaussian FWHM.
        const float sigmaCells = std::max(width / (2.35482f * cellOctaves), 0.35f);
        const int radius = std::max(1, static_cast<int>(std::ceil(3.f * sigmaCells)));
        range.first = std::max(0, center - radius);
        const int last = std::min(NUM_FREQUENCY_CELLS - 1, center + radius);
        float sum = 0.f;
        for (int cell = range.first; cell <= last; ++cell) {
            const float distance = static_cast<float>(cell - center);
            const float weight = std::exp(-0.5f * distance * distance / (sigmaCells * sigmaCells));
            range.weights.push_back(weight);
            sum += weight;
        }
        for (size_t i = 0; i < range.weights.size(); ++i) range.weights[i] /= std::max(sum, 1e-12f);
    }
}

void FrequencySmoothingKernel::apply(const SpectrumRow& input, SpectrumRow& output) const {
    output = input;
    for (int center = 0; center < NUM_FREQUENCY_CELLS; ++center) {
        const TapRange& range = ranges[static_cast<size_t>(center)];
        float power = 0.f;
        for (size_t tap = 0; tap < range.weights.size(); ++tap) {
            const int cell = range.first + static_cast<int>(tap);
            power += range.weights[tap] * dbToPower(dequantizeDb(input.dbTenths[static_cast<size_t>(cell)]));
        }
        output.dbTenths[static_cast<size_t>(center)] = quantizeDb(powerToDb(power));
    }
}

void TemporalPowerSmoother::configure(TemporalSmoothing smoothing) {
    mode = smoothing;
    reset();
}

void TemporalPowerSmoother::reset() {
    state.fill(0.f);
    previous = SpectrumRow();
    initialized = false;
}

void TemporalPowerSmoother::process(const SpectrumRow& input, SpectrumRow& output) {
    output = input;
    float attack = 0.f;
    float release = 0.f;
    temporalTimeConstants(mode, attack, release);
    if (mode == TemporalSmoothing::OFF) return;

    double delta = 0.0;
    if (initialized && input.configGeneration == previous.configGeneration &&
        input.sampleRate == previous.sampleRate && input.rowEndSample > previous.rowEndSample) {
        delta = static_cast<double>(input.rowEndSample - previous.rowEndSample) / input.sampleRate;
    }
    const double expected = input.displayRowsPerSecond > 0
                                ? 1.0 / static_cast<double>(input.displayRowsPerSecond)
                                : 0.0;
    if (!initialized || !(delta > 0.0) || (expected > 0.0 && delta > std::max(2.5 * expected, 0.2))) {
        for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell)
            state[static_cast<size_t>(cell)] =
                dbToPower(dequantizeDb(input.dbTenths[static_cast<size_t>(cell)]));
    } else {
        for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell) {
            const float target = dbToPower(dequantizeDb(input.dbTenths[static_cast<size_t>(cell)]));
            float& value = state[static_cast<size_t>(cell)];
            const float timeConstant = target > value ? attack : release;
            const float alpha = 1.f - std::exp(-static_cast<float>(delta) / std::max(timeConstant, 1e-6f));
            value += alpha * (target - value);
        }
    }
    for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell)
        output.dbTenths[static_cast<size_t>(cell)] = quantizeDb(powerToDb(state[static_cast<size_t>(cell)]));
    previous = input;
    initialized = true;
}

float interpolateNoOvershoot(float left, float right, float fraction) {
    return left + clampValue(fraction, 0.f, 1.f) * (right - left);
}

}  // namespace waterfall
}  // namespace cella
