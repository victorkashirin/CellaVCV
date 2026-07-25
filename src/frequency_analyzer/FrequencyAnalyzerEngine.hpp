#pragma once

#include "FrequencyAnalyzerTypes.hpp"

#include <rack.hpp>

#include <array>
#include <vector>

namespace cella {
namespace frequency_analyzer {

class FrequencyAnalyzerEngine {
  public:
    FrequencyAnalyzerEngine();

    // Returns true only when a complete FFT frame has been analyzed.
    bool process(float leftVoltage, float rightVoltage, bool leftConnected, bool rightConnected, float sampleRate,
                 float fallSeconds);

    const FrequencyAnalyzerFrame& getFrame() const { return frame; }

  private:
    rack::dsp::RealFFT fft{FrequencyAnalyzerConfig::FFT_SIZE};
    std::vector<float> window;
    std::array<std::vector<float>, FrequencyAnalyzerConfig::NUM_AUDIO_CHANNELS> captures;
    std::vector<float> fftOutput;
    std::vector<float> magnitudes;
    std::array<std::vector<float>, FrequencyAnalyzerConfig::NUM_AUDIO_CHANNELS> channelMagnitudes;
    std::array<bool, FrequencyAnalyzerConfig::NUM_AUDIO_CHANNELS> frameChannelActive = {};
    FrequencyAnalyzerFrame frame;
    int writePos = 0;

    void analyze(float sampleRate, float fallSeconds);
    void performFFT();
    void writeFFTBinMagnitudes(std::vector<float>& outputMagnitudes);
    std::array<float, FrequencyAnalyzerConfig::NUM_BANDS + 1> getFrequencyEdges(float sampleRate) const;
    void updateBandLevels(const std::vector<float>& inputMagnitudes,
                          const std::array<float, FrequencyAnalyzerConfig::NUM_BANDS + 1>& edges, float fallDecay,
                          float sampleRate, std::array<float, FrequencyAnalyzerConfig::NUM_BANDS>& bandLevels) const;
    float calculateBandMagnitude(const std::vector<float>& inputMagnitudes, float fLo, float fHi,
                                 float sampleRate) const;
};

}  // namespace frequency_analyzer
}  // namespace cella
