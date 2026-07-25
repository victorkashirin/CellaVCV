#pragma once

#include "WaterfallTypes.hpp"

#include <array>
#include <vector>

namespace cella {
namespace waterfall {

class FrequencySmoothingKernel {
  public:
    void configure(FrequencySmoothing smoothing, float sampleRate = 48000.f);
    void apply(const SpectrumRow& input, SpectrumRow& output) const;

  private:
    struct TapRange {
        int first = 0;
        std::vector<float> weights;
    };
    std::array<TapRange, NUM_FREQUENCY_CELLS> ranges;
};

class TemporalPowerSmoother {
  public:
    void configure(TemporalSmoothing smoothing);
    void reset();
    void process(const SpectrumRow& input, SpectrumRow& output);

  private:
    TemporalSmoothing mode = TemporalSmoothing::OFF;
    std::array<float, NUM_FREQUENCY_CELLS> state = {};
    SpectrumRow previous;
    bool initialized = false;
};

float interpolateNoOvershoot(float left, float right, float fraction);

}  // namespace waterfall
}  // namespace cella
