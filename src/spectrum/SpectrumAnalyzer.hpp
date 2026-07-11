#pragma once

#include "SpectrumTypes.hpp"

#include <rack.hpp>

#include <array>
#include <vector>

namespace cella {
namespace spectrum {

class SpectrumAnalyzer {
  public:
    SpectrumAnalyzer();

    // Returns true only when a complete FFT frame has been analyzed.
    bool process(float leftVoltage, float rightVoltage, bool leftConnected, bool rightConnected, float sampleRate,
                 float fallSeconds);

    const SpectrumFrame& getFrame() const { return frame; }

  private:
    rack::dsp::RealFFT fft{SpectrumConfig::FFT_SIZE};
    std::vector<float> window;
    std::array<std::vector<float>, SpectrumConfig::NUM_AUDIO_CHANNELS> captures;
    std::vector<float> fftOutput;
    std::vector<float> magnitudes;
    std::array<std::vector<float>, SpectrumConfig::NUM_AUDIO_CHANNELS> channelMagnitudes;
    std::array<bool, SpectrumConfig::NUM_AUDIO_CHANNELS> frameChannelActive = {};
    SpectrumFrame frame;
    int writePos = 0;

    void analyze(float sampleRate, float fallSeconds);
    void performFFT();
    void writeFFTBinMagnitudes(std::vector<float>& outputMagnitudes);
    std::array<float, SpectrumConfig::NUM_BANDS + 1> getFrequencyEdges(float sampleRate) const;
    void updateBandLevels(const std::vector<float>& inputMagnitudes,
                          const std::array<float, SpectrumConfig::NUM_BANDS + 1>& edges, float fallDecay,
                          float sampleRate, std::array<float, SpectrumConfig::NUM_BANDS>& bandLevels) const;
    float calculateBandMagnitude(const std::vector<float>& inputMagnitudes, float fLo, float fHi,
                                 float sampleRate) const;
};

}  // namespace spectrum
}  // namespace cella
