#pragma once

#include <array>
#include <cstdint>

namespace cella {
namespace spectrum {

struct SpectrumConfig {
    static constexpr int FFT_SIZE = 2048;
    static constexpr int SPEC_BINS = FFT_SIZE / 2 + 1;
    static constexpr int NUM_BANDS = 12;
    static constexpr int NUM_AUDIO_CHANNELS = 2;
    static constexpr float INPUT_GAIN = 0.1f;
    static constexpr float MIN_DELAY_TIME = 0.001f;
    static constexpr float NOISE_FLOOR_DB = -120.0f;
    static constexpr float DENORMAL_THRESHOLD = 1e-6f;

    static const std::array<float, NUM_BANDS> BAND_CENTERS;
};

struct SpectrumFrame {
    std::array<float, SpectrumConfig::NUM_BANDS> levels;
    std::array<std::array<float, SpectrumConfig::NUM_BANDS>, SpectrumConfig::NUM_AUDIO_CHANNELS> channelLevels;
    std::array<float, SpectrumConfig::NUM_BANDS> peaks;
    std::array<std::array<float, SpectrumConfig::NUM_BANDS>, SpectrumConfig::NUM_AUDIO_CHANNELS> channelPeaks;
    std::uint64_t sequence = 0;
    float sampleRate = 0.0f;

    SpectrumFrame() {
        levels.fill(SpectrumConfig::NOISE_FLOOR_DB);
        peaks.fill(0.0f);
        for (int channel = 0; channel < SpectrumConfig::NUM_AUDIO_CHANNELS; ++channel) {
            channelLevels[channel].fill(SpectrumConfig::NOISE_FLOOR_DB);
            channelPeaks[channel].fill(0.0f);
        }
    }
};

}  // namespace spectrum
}  // namespace cella
