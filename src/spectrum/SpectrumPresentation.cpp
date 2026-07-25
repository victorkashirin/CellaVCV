#include "SpectrumPresentation.hpp"

#include <algorithm>
#include <cmath>

namespace cella {
namespace spectrum {

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

void FrequencySmoothingKernel::configure(FrequencySmoothing smoothing, float sampleRate,
                                         FrequencyBinScale frequencyBins) {
    const float width = frequencySmoothingOctaves(smoothing);
    const float maximumFrequency = displayMaximumFrequency(sampleRate);
    std::array<float, NUM_FREQUENCY_CELLS> centerOctaves;
    for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell) {
        const float coordinate = (static_cast<float>(cell) + 0.5f) / NUM_FREQUENCY_CELLS;
        centerOctaves[static_cast<size_t>(cell)] =
            std::log2(frequencyHzForCoordinate(coordinate, maximumFrequency, frequencyBins));
    }
    for (int center = 0; center < NUM_FREQUENCY_CELLS; ++center) {
        TapRange& range = ranges[static_cast<size_t>(center)];
        range.weights.clear();
        if (!(width > 0.f)) {
            range.first = center;
            range.weights.push_back(1.f);
            continue;
        }
        // Treat the selected fractional octave as the Gaussian FWHM. Weight
        // in log-frequency even when the stored cells use a linear or Mel axis.
        const float sigmaOctaves = width / 2.35482f;
        const float cutoff = 3.f * sigmaOctaves;
        int first = center;
        int last = center;
        while (first > 0 &&
               centerOctaves[static_cast<size_t>(center)] -
                       centerOctaves[static_cast<size_t>(first - 1)] <=
                   cutoff)
            --first;
        while (last + 1 < NUM_FREQUENCY_CELLS &&
               centerOctaves[static_cast<size_t>(last + 1)] -
                       centerOctaves[static_cast<size_t>(center)] <=
                   cutoff)
            ++last;
        range.first = first;
        float sum = 0.f;
        for (int cell = first; cell <= last; ++cell) {
            const float distance = centerOctaves[static_cast<size_t>(cell)] -
                                   centerOctaves[static_cast<size_t>(center)];
            const float weight =
                std::exp(-0.5f * distance * distance / (sigmaOctaves * sigmaOctaves));
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

float interpolateNoOvershoot(float left, float right, float fraction) {
    return left + clampValue(fraction, 0.f, 1.f) * (right - left);
}

}  // namespace spectrum
}  // namespace cella
