#include "WaterfallAnalyzer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cella {
namespace waterfall {

namespace {

constexpr float PI = 3.14159265358979323846f;
constexpr float MAGNITUDE_EPSILON = 1e-8f;

float windowCoefficient(WindowFunction window, int index, int size) {
    const float phase = 2.f * PI * static_cast<float>(index) / static_cast<float>(size - 1);
    switch (window) {
        case WindowFunction::HANN:
            return 0.5f - 0.5f * std::cos(phase);
        case WindowFunction::BLACKMAN_HARRIS:
            return 0.35875f - 0.48829f * std::cos(phase) + 0.14128f * std::cos(2.f * phase) -
                   0.01168f * std::cos(3.f * phase);
        case WindowFunction::FLAT_TOP:
            return 0.21557895f - 0.41663158f * std::cos(phase) + 0.277263158f * std::cos(2.f * phase) -
                   0.083578947f * std::cos(3.f * phase) + 0.006947368f * std::cos(4.f * phase);
        default:
            return 1.f;
    }
}

}  // namespace

struct WaterfallAnalyzer::Kernel {
    explicit Kernel(int length)
        : size(length),
          fft(length),
          capture(static_cast<size_t>(length), 0.f),
          work(static_cast<size_t>(length), 0.f),
          fftOutput(static_cast<size_t>(length), 0.f),
          magnitudes(static_cast<size_t>(length / 2 + 1), 0.f) {
        for (int windowIndex = 0; windowIndex < static_cast<int>(WindowFunction::COUNT); ++windowIndex) {
            windows[windowIndex].resize(static_cast<size_t>(size));
            float sum = 0.f;
            for (int i = 0; i < size; ++i) {
                const float coefficient = windowCoefficient(static_cast<WindowFunction>(windowIndex), i, size);
                windows[windowIndex][static_cast<size_t>(i)] = coefficient;
                sum += coefficient;
            }
            coherentGains[windowIndex] = sum / static_cast<float>(size);
        }
        cellBinLow.fill(1);
        cellBinHigh.fill(1);
    }

    int size;
    rack::dsp::RealFFT fft;
    std::vector<float> capture;
    std::vector<float> work;
    std::vector<float> fftOutput;
    std::vector<float> magnitudes;
    std::array<std::vector<float>, static_cast<int>(WindowFunction::COUNT)> windows;
    std::array<float, static_cast<int>(WindowFunction::COUNT)> coherentGains = {};
    std::array<int, NUM_FREQUENCY_CELLS> cellBinLow;
    std::array<int, NUM_FREQUENCY_CELLS> cellBinHigh;
    std::array<float, NUM_FREQUENCY_CELLS> spectrum = {};
    int writePosition = 0;
    int filled = 0;
    int samplesSinceAnalysis = 0;
    int hopSize = 1;
    bool analyzed = false;

    void reset(float sampleRate, FftOverlap overlap, FrequencyBinScale frequencyBins) {
        std::fill(capture.begin(), capture.end(), 0.f);
        std::fill(work.begin(), work.end(), 0.f);
        std::fill(fftOutput.begin(), fftOutput.end(), 0.f);
        std::fill(magnitudes.begin(), magnitudes.end(), 0.f);
        spectrum.fill(INTERNAL_FLOOR_DB);
        writePosition = 0;
        filled = 0;
        samplesSinceAnalysis = 0;
        analyzed = false;
        hopSize = effectiveFftHopSize(size, sampleRate, overlap);
        updateCellMapping(sampleRate, frequencyBins);
    }

    void updateCellMapping(float sampleRate, FrequencyBinScale frequencyBins) {
        const float maximumFrequency = displayMaximumFrequency(sampleRate);
        const float binWidth = sampleRate / static_cast<float>(size);
        const int nyquistBin = size / 2;
        const int maximumBin =
            clampValue(static_cast<int>(std::floor(maximumFrequency / binWidth)), 1, nyquistBin);
        for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell) {
            const float lowFraction = static_cast<float>(cell) / NUM_FREQUENCY_CELLS;
            const float highFraction = static_cast<float>(cell + 1) / NUM_FREQUENCY_CELLS;
            const float lowFrequency =
                frequencyHzForCoordinate(lowFraction, maximumFrequency, frequencyBins);
            const float highFrequency =
                frequencyHzForCoordinate(highFraction, maximumFrequency, frequencyBins);
            int lowBin = static_cast<int>(std::ceil(lowFrequency / binWidth));
            int highBin = static_cast<int>(std::floor(highFrequency / binWidth));
            lowBin = clampValue(lowBin, 1, maximumBin);
            highBin = clampValue(highBin, 1, maximumBin);
            if (highBin < lowBin) {
                const float centerFrequency = frequencyHzForCoordinate(
                    0.5f * (lowFraction + highFraction), maximumFrequency, frequencyBins);
                lowBin = highBin =
                    clampValue(static_cast<int>(std::lround(centerFrequency / binWidth)), 1, maximumBin);
            }
            cellBinLow[static_cast<size_t>(cell)] = lowBin;
            cellBinHigh[static_cast<size_t>(cell)] = highBin;
        }
    }

    bool process(float sample, WindowFunction selectedWindow) {
        capture[static_cast<size_t>(writePosition)] = std::isfinite(sample) ? sample : 0.f;
        writePosition = (writePosition + 1) % size;
        filled = std::min(filled + 1, size);
        ++samplesSinceAnalysis;
        if (filled < size || (analyzed && samplesSinceAnalysis < hopSize)) return false;

        analyze(selectedWindow);
        samplesSinceAnalysis = 0;
        analyzed = true;
        return true;
    }

    void analyze(WindowFunction selectedWindow) {
        const int windowIndex =
            clampValue(static_cast<int>(selectedWindow), 0, static_cast<int>(WindowFunction::COUNT) - 1);
        const std::vector<float>& selected = windows[static_cast<size_t>(windowIndex)];
        for (int i = 0; i < size; ++i) {
            const int captureIndex = (writePosition + i) % size;
            work[static_cast<size_t>(i)] =
                capture[static_cast<size_t>(captureIndex)] * selected[static_cast<size_t>(i)];
        }

        fft.rfft(work.data(), fftOutput.data());
        fft.scale(fftOutput.data());

        // rfft.scale() supplies 1/N. A real sine has half its peak amplitude
        // in each side of the spectrum, so ordinary positive bins receive the
        // one-sided factor 2. Dividing by coherent gain removes window loss.
        const float inverseCoherentGain = 1.f / std::max(coherentGains[static_cast<size_t>(windowIndex)], 1e-8f);
        magnitudes[0] = std::fabs(fftOutput[0]) * inverseCoherentGain;
        magnitudes[static_cast<size_t>(size / 2)] = std::fabs(fftOutput[1]) * inverseCoherentGain;
        for (int bin = 1; bin < size / 2; ++bin) {
            const float real = fftOutput[static_cast<size_t>(2 * bin)];
            const float imaginary = fftOutput[static_cast<size_t>(2 * bin + 1)];
            magnitudes[static_cast<size_t>(bin)] =
                2.f * std::sqrt(real * real + imaginary * imaginary) * inverseCoherentGain;
        }

        for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell) {
            float maximum = 0.f;
            const int low = cellBinLow[static_cast<size_t>(cell)];
            const int high = cellBinHigh[static_cast<size_t>(cell)];
            for (int bin = low; bin <= high; ++bin) {
                maximum = std::max(maximum, magnitudes[static_cast<size_t>(bin)]);
            }
            const float db = 20.f * std::log10(std::max(maximum, MAGNITUDE_EPSILON));
            spectrum[static_cast<size_t>(cell)] =
                clampValue(std::isfinite(db) ? db : INTERNAL_FLOOR_DB, INTERNAL_FLOOR_DB, INTERNAL_CEILING_DB);
        }
    }
};

WaterfallAnalyzer::WaterfallAnalyzer() {
    for (int i = 0; i < static_cast<int>(FftSize::COUNT); ++i) {
        kernels[static_cast<size_t>(i)].reset(new Kernel(FFT_SIZES[static_cast<size_t>(i)]));
    }
    latestSpectrum.fill(INTERNAL_FLOOR_DB);
    rowMaximum.fill(INTERNAL_FLOOR_DB);
    configure(config);
}

WaterfallAnalyzer::~WaterfallAnalyzer() = default;

void WaterfallAnalyzer::configure(const WaterfallConfig& newConfig) {
    const WaterfallConfig oldConfig = config;
    const bool wasConfigured = activeKernel != NULL;
    config = newConfig;
    const int fftIndex = clampValue(static_cast<int>(config.fftSize), 0, static_cast<int>(FftSize::COUNT) - 1);
    config.fftSize = static_cast<FftSize>(fftIndex);
    config.window = static_cast<WindowFunction>(
        clampValue(static_cast<int>(config.window), 0, static_cast<int>(WindowFunction::COUNT) - 1));
    config.fftOverlap = static_cast<FftOverlap>(
        clampValue(static_cast<int>(config.fftOverlap), 0, static_cast<int>(FftOverlap::COUNT) - 1));
    config.quality =
        static_cast<Quality>(clampValue(static_cast<int>(config.quality), 0, static_cast<int>(Quality::COUNT) - 1));
    config.channelMode = static_cast<ChannelMode>(
        clampValue(static_cast<int>(config.channelMode), 0, static_cast<int>(ChannelMode::COUNT) - 1));
    config.frequencyBins = static_cast<FrequencyBinScale>(
        clampValue(static_cast<int>(config.frequencyBins), 0,
                   static_cast<int>(FrequencyBinScale::COUNT) - 1));
    config.polyChannel = clampValue(config.polyChannel, 0, 15);
    config.sampleRate = std::max(config.sampleRate, 1000.f);

    const bool analysisChanged =
        !wasConfigured || oldConfig.fftSize != config.fftSize || oldConfig.window != config.window ||
        oldConfig.fftOverlap != config.fftOverlap || oldConfig.channelMode != config.channelMode ||
        oldConfig.frequencyBins != config.frequencyBins || oldConfig.polyChannel != config.polyChannel ||
        oldConfig.sampleRate != config.sampleRate || oldConfig.generation != config.generation;
    activeKernel = kernels[static_cast<size_t>(fftIndex)].get();
    if (analysisChanged) {
        activeKernel->reset(config.sampleRate, config.fftOverlap, config.frequencyBins);
        processedSamples = 0;
        latestSpectrumSample = 0;
        haveSpectrum = false;
        rowHasSpectrum = false;
        latestSpectrum.fill(INTERNAL_FLOOR_DB);
        rowMaximum.fill(INTERNAL_FLOOR_DB);
    }
    resetRowClock();
}

void WaterfallAnalyzer::resetRowClock() {
    rowPeriodSamples = std::max<uint64_t>(
        1, static_cast<uint64_t>(std::llround(config.sampleRate / rowsPerSecond(config.quality))));
    nextRowSample = processedSamples + rowPeriodSamples;
}

bool WaterfallAnalyzer::processSample(float normalizedSample, SpectrumRow& outputRow) {
    return processSample(normalizedSample, processedSamples + 1, outputRow);
}

bool WaterfallAnalyzer::processSample(float normalizedSample, uint64_t timelineSample, SpectrumRow& outputRow) {
    ++processedSamples;
    currentTimelineSample = timelineSample;
    if (activeKernel->process(normalizedSample, config.window)) finishSpectrum(timelineSample);

    if (processedSamples < nextRowSample) return false;
    publishRow(outputRow);
    do {
        nextRowSample += rowPeriodSamples;
    } while (nextRowSample <= processedSamples);
    rowMaximum.fill(INTERNAL_FLOOR_DB);
    rowHasSpectrum = false;
    return true;
}

void WaterfallAnalyzer::finishSpectrum(uint64_t timelineSample) {
    latestSpectrum = activeKernel->spectrum;
    latestSpectrumSample = timelineSample;
    haveSpectrum = true;
    if (!rowHasSpectrum) {
        rowMaximum = latestSpectrum;
        rowHasSpectrum = true;
        return;
    }
    for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell) {
        rowMaximum[static_cast<size_t>(cell)] =
            std::max(rowMaximum[static_cast<size_t>(cell)], latestSpectrum[static_cast<size_t>(cell)]);
    }
}

void WaterfallAnalyzer::publishRow(SpectrumRow& outputRow) {
    const std::array<float, NUM_FREQUENCY_CELLS>& values =
        rowHasSpectrum ? rowMaximum : (haveSpectrum ? latestSpectrum : rowMaximum);
    for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell) {
        outputRow.dbTenths[static_cast<size_t>(cell)] = quantizeDb(values[static_cast<size_t>(cell)]);
    }
    outputRow.rowEndSample = currentTimelineSample;
    outputRow.sourceAnalysisSample = latestSpectrumSample;
    outputRow.sampleRate = config.sampleRate;
    outputRow.fftSize = static_cast<uint32_t>(activeKernel->size);
    outputRow.effectiveHopSize = static_cast<uint32_t>(activeKernel->hopSize);
    outputRow.displayRowsPerSecond = static_cast<uint32_t>(rowsPerSecond(config.quality));
    outputRow.configGeneration = config.generation;
}

}  // namespace waterfall
}  // namespace cella
