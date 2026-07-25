#pragma once

#include "WaterfallTypes.hpp"

#include <array>
#include <vector>

namespace cella {
namespace waterfall {

class FrequencySmoothingKernel {
  public:
    void configure(FrequencySmoothing smoothing, float sampleRate = 48000.f,
                   FrequencyBinScale frequencyBins = FrequencyBinScale::LOGARITHMIC);
    void apply(const SpectrumRow& input, SpectrumRow& output) const;

  private:
    struct TapRange {
        int first = 0;
        std::vector<float> weights;
    };
    std::array<TapRange, NUM_FREQUENCY_CELLS> ranges;
};

float interpolateNoOvershoot(float left, float right, float fraction);

}  // namespace waterfall
}  // namespace cella
