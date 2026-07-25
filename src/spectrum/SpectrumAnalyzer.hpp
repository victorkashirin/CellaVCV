#pragma once

#include "SpectrumTypes.hpp"

#include <rack.hpp>

#include <array>
#include <memory>
#include <vector>

namespace cella {
namespace spectrum {

class SpectrumAnalyzer {
  public:
    SpectrumAnalyzer();
    ~SpectrumAnalyzer();

    SpectrumAnalyzer(const SpectrumAnalyzer&) = delete;
    SpectrumAnalyzer& operator=(const SpectrumAnalyzer&) = delete;

    void configure(const SpectrumConfig& config);

    /**
     * Processes one already-selected, already-normalized peak sample.
     * Returns true when one sample-clock display row has been completed.
     */
    bool processSample(float normalizedSample, SpectrumRow& outputRow);
    bool processSample(float normalizedSample, uint64_t timelineSample, SpectrumRow& outputRow);

    /**
     * Discards delayed Advanced rows at a Clear or Freeze boundary. Future
     * reassigned contributions at or before the boundary are rejected.
     */
    void discardPending(uint64_t timelineSample);

    const SpectrumConfig& getConfig() const { return config; }
    uint64_t getProcessedSamples() const { return processedSamples; }

  private:
    struct Kernel;
    std::array<std::unique_ptr<Kernel>, static_cast<int>(FftSize::COUNT)> kernels;
    SpectrumConfig config;
    Kernel* activeKernel = nullptr;
    std::array<float, NUM_FREQUENCY_CELLS> latestSpectrum = {};
    std::array<float, NUM_FREQUENCY_CELLS> rowMaximum = {};
    uint64_t processedSamples = 0;
    uint64_t latestSpectrumSample = 0;
    uint64_t currentTimelineSample = 0;
    uint64_t nextRowSample = 0;
    uint64_t rowPeriodSamples = 1;
    bool haveSpectrum = false;
    bool rowHasSpectrum = false;
    uint64_t advancedBoundarySample = 0;

    void resetRowClock();
    void finishSpectrum(uint64_t timelineSample);
    void publishRow(SpectrumRow& outputRow);
    void finishAdvancedSpectrum(uint64_t timelineSample);
    bool publishAdvancedRow(uint64_t timelineSample, SpectrumRow& outputRow);
};

}  // namespace spectrum
}  // namespace cella
