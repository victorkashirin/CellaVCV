#include "FrequencyAnalyzerEngine.hpp"

#include <algorithm>
#include <cmath>

namespace cella {
namespace frequency_analyzer {

constexpr int FrequencyAnalyzerConfig::FFT_SIZE;
constexpr int FrequencyAnalyzerConfig::SPEC_BINS;
constexpr int FrequencyAnalyzerConfig::NUM_BANDS;
constexpr int FrequencyAnalyzerConfig::NUM_AUDIO_CHANNELS;
constexpr float FrequencyAnalyzerConfig::INPUT_GAIN;
constexpr float FrequencyAnalyzerConfig::MIN_DELAY_TIME;
constexpr float FrequencyAnalyzerConfig::NOISE_FLOOR_DB;
constexpr float FrequencyAnalyzerConfig::DENORMAL_THRESHOLD;
const std::array<float, FrequencyAnalyzerConfig::NUM_BANDS> FrequencyAnalyzerConfig::BAND_CENTERS = {
    {25.f, 40.f, 63.f, 100.f, 160.f, 250.f, 500.f, 1000.f, 2000.f, 4000.f, 8000.f, 16000.f}};

template <typename T>
static T clampValue(T value, T low, T high) {
    return value < low ? low : (value > high ? high : value);
}

FrequencyAnalyzerEngine::FrequencyAnalyzerEngine() {
    window.resize(FrequencyAnalyzerConfig::FFT_SIZE);
    fftOutput.resize(FrequencyAnalyzerConfig::FFT_SIZE);
    magnitudes.resize(FrequencyAnalyzerConfig::SPEC_BINS);
    for (int channel = 0; channel < FrequencyAnalyzerConfig::NUM_AUDIO_CHANNELS; ++channel) {
        captures[channel].resize(FrequencyAnalyzerConfig::FFT_SIZE);
        channelMagnitudes[channel].resize(FrequencyAnalyzerConfig::SPEC_BINS);
    }

    for (int i = 0; i < FrequencyAnalyzerConfig::FFT_SIZE; ++i) {
        window[i] = 0.5f * (1.f - std::cos(2.f * float(M_PI) * i / (FrequencyAnalyzerConfig::FFT_SIZE - 1)));
    }
}

bool FrequencyAnalyzerEngine::process(float leftVoltage, float rightVoltage, bool leftConnected, bool rightConnected,
                               float sampleRate, float fallSeconds) {
    const float windowValue = window[writePos] * FrequencyAnalyzerConfig::INPUT_GAIN;
    captures[0][writePos] = leftConnected ? leftVoltage * windowValue : 0.f;
    captures[1][writePos] = rightConnected ? rightVoltage * windowValue : 0.f;
    frameChannelActive[0] = frameChannelActive[0] || leftConnected;
    frameChannelActive[1] = frameChannelActive[1] || rightConnected;

    if (++writePos < FrequencyAnalyzerConfig::FFT_SIZE) {
        return false;
    }

    writePos = 0;
    analyze(sampleRate, fallSeconds);
    frameChannelActive.fill(false);
    return true;
}

void FrequencyAnalyzerEngine::analyze(float sampleRate, float fallSeconds) {
    performFFT();
    const std::array<float, FrequencyAnalyzerConfig::NUM_BANDS + 1> edges = getFrequencyEdges(sampleRate);
    const float delay = std::max(FrequencyAnalyzerConfig::MIN_DELAY_TIME, fallSeconds);
    const float deltaTime = static_cast<float>(FrequencyAnalyzerConfig::FFT_SIZE) / sampleRate;
    const float fallDecay = std::exp(-deltaTime / delay);

    updateBandLevels(magnitudes, edges, fallDecay, sampleRate, frame.levels);
    for (int channel = 0; channel < FrequencyAnalyzerConfig::NUM_AUDIO_CHANNELS; ++channel) {
        updateBandLevels(channelMagnitudes[channel], edges, fallDecay, sampleRate, frame.channelLevels[channel]);
    }
    ++frame.sequence;
    frame.sampleRate = sampleRate;
}

void FrequencyAnalyzerEngine::performFFT() {
    std::fill(magnitudes.begin(), magnitudes.end(), 0.f);
    for (int channel = 0; channel < FrequencyAnalyzerConfig::NUM_AUDIO_CHANNELS; ++channel) {
        std::fill(channelMagnitudes[channel].begin(), channelMagnitudes[channel].end(), 0.f);
    }

    int activeChannels = 0;
    for (int channel = 0; channel < FrequencyAnalyzerConfig::NUM_AUDIO_CHANNELS; ++channel) {
        if (!frameChannelActive[channel]) {
            continue;
        }

        ++activeChannels;
        fft.rfft(captures[channel].data(), fftOutput.data());
        fft.scale(fftOutput.data());
        writeFFTBinMagnitudes(channelMagnitudes[channel]);
        for (int k = 0; k < FrequencyAnalyzerConfig::SPEC_BINS; ++k) {
            const float channelMagnitude = channelMagnitudes[channel][k];
            magnitudes[k] += channelMagnitude * channelMagnitude;
        }
    }

    if (activeChannels == 0) {
        return;
    }

    const float channelScale = 1.f / activeChannels;
    for (float& magnitude : magnitudes) {
        magnitude = std::sqrt(magnitude * channelScale);
    }
}

void FrequencyAnalyzerEngine::writeFFTBinMagnitudes(std::vector<float>& outputMagnitudes) {
    // Rack's real FFT packs DC at [0], Nyquist at [1], and complex bins thereafter.
    outputMagnitudes[0] = std::fabs(fftOutput[0]);
    outputMagnitudes[FrequencyAnalyzerConfig::FFT_SIZE / 2] = std::fabs(fftOutput[1]);
    for (int k = 1; k < FrequencyAnalyzerConfig::FFT_SIZE / 2; ++k) {
        const float real = fftOutput[2 * k];
        const float imaginary = fftOutput[2 * k + 1];
        outputMagnitudes[k] = std::sqrt(real * real + imaginary * imaginary);
    }
}

std::array<float, FrequencyAnalyzerConfig::NUM_BANDS + 1> FrequencyAnalyzerEngine::getFrequencyEdges(float sampleRate) const {
    std::array<float, FrequencyAnalyzerConfig::NUM_BANDS + 1> edges;
    edges[0] = std::sqrt(FrequencyAnalyzerConfig::BAND_CENTERS[0] * (FrequencyAnalyzerConfig::BAND_CENTERS[0] / 2.f));
    for (int i = 1; i < FrequencyAnalyzerConfig::NUM_BANDS; ++i) {
        edges[i] = std::sqrt(FrequencyAnalyzerConfig::BAND_CENTERS[i - 1] * FrequencyAnalyzerConfig::BAND_CENTERS[i]);
    }
    edges[FrequencyAnalyzerConfig::NUM_BANDS] = sampleRate * 0.5f;
    return edges;
}

void FrequencyAnalyzerEngine::updateBandLevels(
    const std::vector<float>& inputMagnitudes,
    const std::array<float, FrequencyAnalyzerConfig::NUM_BANDS + 1>& edges,
    float fallDecay,
    float sampleRate,
    std::array<float, FrequencyAnalyzerConfig::NUM_BANDS>& bandLevels) const {
    for (int band = 0; band < FrequencyAnalyzerConfig::NUM_BANDS; ++band) {
        const float average = calculateBandMagnitude(inputMagnitudes, edges[band], edges[band + 1], sampleRate);
        const float newDb = 20.f * std::log10(average + FrequencyAnalyzerConfig::DENORMAL_THRESHOLD);
        const float currentDb = bandLevels[band];
        const float updatedDb = newDb >= currentDb ? newDb : currentDb * fallDecay + newDb * (1.f - fallDecay);
        bandLevels[band] = std::max(updatedDb, FrequencyAnalyzerConfig::NOISE_FLOOR_DB);
    }
}

float FrequencyAnalyzerEngine::calculateBandMagnitude(const std::vector<float>& inputMagnitudes, float fLo, float fHi,
                                               float sampleRate) const {
    const int binLo = clampValue<int>(static_cast<int>(std::floor(fLo * FrequencyAnalyzerConfig::FFT_SIZE / sampleRate)), 0,
                                      FrequencyAnalyzerConfig::SPEC_BINS - 1);
    int binHi = clampValue<int>(static_cast<int>(std::ceil(fHi * FrequencyAnalyzerConfig::FFT_SIZE / sampleRate)), binLo + 1,
                                FrequencyAnalyzerConfig::SPEC_BINS);
    if (fHi >= sampleRate * 0.5f) {
        binHi = FrequencyAnalyzerConfig::SPEC_BINS;
    }

    float sum = 0.f;
    for (int bin = binLo; bin < binHi; ++bin) {
        sum += inputMagnitudes[bin];
    }
    return sum / (binHi - binLo);
}

}  // namespace frequency_analyzer
}  // namespace cella
