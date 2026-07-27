#include "SpectrumAnalyzer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cella {
namespace spectrum {

namespace {

constexpr float PI = 3.14159265358979323846f;
constexpr float MAGNITUDE_EPSILON = 1e-8f;
// Calibrated magnitude corresponding to the fixed -140 dBFS stability floor.
constexpr float REASSIGNMENT_MIN_MAGNITUDE = 1e-7f;

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

float windowDerivative(WindowFunction window, int index, int size) {
    const float phaseStep = 2.f * PI / static_cast<float>(size - 1);
    const float phase = phaseStep * static_cast<float>(index);
    switch (window) {
        case WindowFunction::HANN:
            return 0.5f * phaseStep * std::sin(phase);
        case WindowFunction::BLACKMAN_HARRIS:
            return phaseStep *
                   (0.48829f * std::sin(phase) - 2.f * 0.14128f * std::sin(2.f * phase) +
                    3.f * 0.01168f * std::sin(3.f * phase));
        case WindowFunction::FLAT_TOP:
            return phaseStep *
                   (0.41663158f * std::sin(phase) - 2.f * 0.277263158f * std::sin(2.f * phase) +
                    3.f * 0.083578947f * std::sin(3.f * phase) -
                    4.f * 0.006947368f * std::sin(4.f * phase));
        default:
            return 0.f;
    }
}

}  // namespace

struct SpectrumAnalyzer::Kernel {
    struct AdvancedBin {
        float timeCorrectionSamples = 0.f;
        float frequencyHz = 0.f;
        float power = 0.f;
        bool valid = false;
    };

    struct PendingRow {
        std::array<float, NUM_FREQUENCY_CELLS> power = {};
        uint64_t targetRowEndSample = 0;
        uint64_t sourceAnalysisSample = 0;
        bool valid = false;
    };

    explicit Kernel(int length)
        : size(length),
          fft(length),
          capture(static_cast<size_t>(length), 0.f),
          work(static_cast<size_t>(length), 0.f),
          fftOutput(static_cast<size_t>(length), 0.f),
          timeFftOutput(static_cast<size_t>(length), 0.f),
          derivativeFftOutput(static_cast<size_t>(length), 0.f),
          advancedBins(static_cast<size_t>(length / 2 + 1)),
          pendingRows(static_cast<size_t>(length / 16 + 8)),
          magnitudes(static_cast<size_t>(length / 2 + 1), 0.f) {
        for (int windowIndex = 0; windowIndex < static_cast<int>(WindowFunction::COUNT); ++windowIndex) {
            windows[windowIndex].resize(static_cast<size_t>(size));
            timeWeightedWindows[windowIndex].resize(static_cast<size_t>(size));
            derivativeWindows[windowIndex].resize(static_cast<size_t>(size));
            float sum = 0.f;
            float squaredSum = 0.f;
            const float center = 0.5f * static_cast<float>(size - 1);
            for (int i = 0; i < size; ++i) {
                const float coefficient = windowCoefficient(static_cast<WindowFunction>(windowIndex), i, size);
                windows[windowIndex][static_cast<size_t>(i)] = coefficient;
                timeWeightedWindows[windowIndex][static_cast<size_t>(i)] =
                    (static_cast<float>(i) - center) * coefficient;
                derivativeWindows[windowIndex][static_cast<size_t>(i)] =
                    windowDerivative(static_cast<WindowFunction>(windowIndex), i, size);
                sum += coefficient;
                squaredSum += coefficient * coefficient;
            }
            coherentGains[windowIndex] = sum / static_cast<float>(size);
            equivalentNoiseBandwidths[windowIndex] =
                static_cast<float>(size) * squaredSum / std::max(sum * sum, 1e-16f);
        }
        cellBinLow.fill(1);
        cellBinHigh.fill(1);
    }

    int size;
    rack::dsp::RealFFT fft;
    std::vector<float> capture;
    std::vector<float> work;
    std::vector<float> fftOutput;
    std::vector<float> timeFftOutput;
    std::vector<float> derivativeFftOutput;
    std::vector<AdvancedBin> advancedBins;
    std::vector<PendingRow> pendingRows;
    std::vector<float> magnitudes;
    std::array<std::vector<float>, static_cast<int>(WindowFunction::COUNT)> windows;
    std::array<std::vector<float>, static_cast<int>(WindowFunction::COUNT)> timeWeightedWindows;
    std::array<std::vector<float>, static_cast<int>(WindowFunction::COUNT)> derivativeWindows;
    std::array<float, static_cast<int>(WindowFunction::COUNT)> coherentGains = {};
    std::array<float, static_cast<int>(WindowFunction::COUNT)> equivalentNoiseBandwidths = {};
    std::array<int, NUM_FREQUENCY_CELLS> cellBinLow;
    std::array<int, NUM_FREQUENCY_CELLS> cellBinHigh;
    std::array<float, NUM_FREQUENCY_CELLS> spectrum = {};
    int writePosition = 0;
    int filled = 0;
    int samplesSinceAnalysis = 0;
    int hopSize = 1;
    int pendingCapacity = 1;
    uint64_t pendingRowPeriod = 1;
    uint64_t nextAdvancedRowSample = 0;
    bool advancedGridInitialized = false;
    bool analyzed = false;

    void clearPending(uint64_t rowPeriod) {
        pendingRowPeriod = std::max<uint64_t>(rowPeriod, 1);
        const uint64_t required =
            (static_cast<uint64_t>(size) + pendingRowPeriod - 1) / pendingRowPeriod + 4;
        pendingCapacity = static_cast<int>(
            std::min<uint64_t>(std::max<uint64_t>(required, 1), pendingRows.size()));
        for (size_t i = 0; i < pendingRows.size(); ++i) {
            pendingRows[i].power.fill(0.f);
            pendingRows[i].targetRowEndSample = 0;
            pendingRows[i].sourceAnalysisSample = 0;
            pendingRows[i].valid = false;
        }
        nextAdvancedRowSample = 0;
        advancedGridInitialized = false;
    }

    void reset(float sampleRate, FftOverlap overlap, FrequencyBinScale frequencyBins,
               uint64_t rowPeriod, bool preserveCapture = false) {
        if (!preserveCapture) {
            std::fill(capture.begin(), capture.end(), 0.f);
            writePosition = 0;
            filled = 0;
        }
        std::fill(work.begin(), work.end(), 0.f);
        std::fill(fftOutput.begin(), fftOutput.end(), 0.f);
        std::fill(timeFftOutput.begin(), timeFftOutput.end(), 0.f);
        std::fill(derivativeFftOutput.begin(), derivativeFftOutput.end(), 0.f);
        std::fill(magnitudes.begin(), magnitudes.end(), 0.f);
        std::fill(advancedBins.begin(), advancedBins.end(), AdvancedBin());
        spectrum.fill(INTERNAL_FLOOR_DB);
        samplesSinceAnalysis = 0;
        analyzed = false;
        hopSize = effectiveFftHopSize(size, sampleRate, overlap);
        updateCellMapping(sampleRate, frequencyBins);
        clearPending(rowPeriod);
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

    bool process(float sample, WindowFunction selectedWindow, AnalysisMode mode, float sampleRate) {
        capture[static_cast<size_t>(writePosition)] = std::isfinite(sample) ? sample : 0.f;
        writePosition = (writePosition + 1) % size;
        filled = std::min(filled + 1, size);
        ++samplesSinceAnalysis;
        if (filled < size || (analyzed && samplesSinceAnalysis < hopSize)) return false;

        if (mode == AnalysisMode::ADVANCED)
            analyzeAdvanced(selectedWindow, sampleRate);
        else
            analyzeClassic(selectedWindow);
        samplesSinceAnalysis = 0;
        analyzed = true;
        return true;
    }

    void prepareWork(const std::vector<float>& selected) {
        for (int i = 0; i < size; ++i) {
            const int captureIndex = (writePosition + i) % size;
            work[static_cast<size_t>(i)] =
                capture[static_cast<size_t>(captureIndex)] * selected[static_cast<size_t>(i)];
        }
    }

    void analyzeClassic(WindowFunction selectedWindow) {
        const int windowIndex =
            clampValue(static_cast<int>(selectedWindow), 0, static_cast<int>(WindowFunction::COUNT) - 1);
        const std::vector<float>& selected = windows[static_cast<size_t>(windowIndex)];
        prepareWork(selected);
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

    void analyzeAdvanced(WindowFunction selectedWindow, float sampleRate) {
        const int windowIndex =
            clampValue(static_cast<int>(selectedWindow), 0, static_cast<int>(WindowFunction::COUNT) - 1);
        prepareWork(windows[static_cast<size_t>(windowIndex)]);
        fft.rfft(work.data(), fftOutput.data());
        fft.scale(fftOutput.data());
        prepareWork(timeWeightedWindows[static_cast<size_t>(windowIndex)]);
        fft.rfft(work.data(), timeFftOutput.data());
        fft.scale(timeFftOutput.data());
        prepareWork(derivativeWindows[static_cast<size_t>(windowIndex)]);
        fft.rfft(work.data(), derivativeFftOutput.data());
        fft.scale(derivativeFftOutput.data());

        std::fill(advancedBins.begin(), advancedBins.end(), AdvancedBin());
        const float coherentGain =
            std::max(coherentGains[static_cast<size_t>(windowIndex)], 1e-8f);
        const float enbw =
            std::max(equivalentNoiseBandwidths[static_cast<size_t>(windowIndex)], 1e-8f);
        const float maximumFrequency = displayMaximumFrequency(sampleRate);
        const float nyquist = 0.5f * sampleRate;

        // This is the discrete time-frequency reassignment formulation of
        // Flandrin, Auger, et al. rack::dsp::RealFFT uses the sign convention
        // for which +Re(Xt/X) and -Im(Xd/X) move an isolated component toward
        // its event time and instantaneous frequency.
        for (int bin = 1; bin < size / 2; ++bin) {
            const size_t realIndex = static_cast<size_t>(2 * bin);
            const size_t imaginaryIndex = realIndex + 1;
            const float xr = fftOutput[realIndex];
            const float xi = fftOutput[imaginaryIndex];
            const float xtr = timeFftOutput[realIndex];
            const float xti = timeFftOutput[imaginaryIndex];
            const float xdr = derivativeFftOutput[realIndex];
            const float xdi = derivativeFftOutput[imaginaryIndex];
            if (!std::isfinite(xr) || !std::isfinite(xi) || !std::isfinite(xtr) ||
                !std::isfinite(xti) || !std::isfinite(xdr) || !std::isfinite(xdi))
                continue;

            const float denominator = xr * xr + xi * xi;
            if (!(denominator > 0.f) || !std::isfinite(denominator)) continue;
            const float calibratedMagnitude =
                2.f * std::sqrt(denominator) / coherentGain;
            if (!(calibratedMagnitude >= REASSIGNMENT_MIN_MAGNITUDE) ||
                !std::isfinite(calibratedMagnitude))
                continue;

            const float timeCorrection = (xtr * xr + xti * xi) / denominator;
            const float derivativeRatioImaginary = (xdi * xr - xdr * xi) / denominator;
            const float omegaK = 2.f * PI * static_cast<float>(bin) / static_cast<float>(size);
            const float omegaHat = omegaK - derivativeRatioImaginary;
            const float frequency = omegaHat * sampleRate / (2.f * PI);
            if (!std::isfinite(timeCorrection) || !std::isfinite(omegaHat) ||
                !std::isfinite(frequency) || std::fabs(timeCorrection) > 0.5f * size ||
                !(omegaHat > 0.f && omegaHat < PI) || !(frequency >= MIN_FREQUENCY_HZ) ||
                !(frequency <= maximumFrequency) || !(frequency < nyquist))
                continue;

            AdvancedBin& result = advancedBins[static_cast<size_t>(bin)];
            result.timeCorrectionSamples = timeCorrection;
            result.frequencyHz = frequency;
            // Reassignment accumulates linear power. Summing the squared
            // coherent-gain-calibrated one-sided bins yields ENBW times the
            // peak sine power, so division by ENBW keeps a 10 V peak sine at
            // 0 dBFS. hop/rowPeriod below supplies the fixed overlap/cadence
            // normalization.
            result.power = calibratedMagnitude * calibratedMagnitude / enbw;
            result.valid = std::isfinite(result.power) && result.power > 0.f;
        }
    }

    PendingRow* pendingRow(uint64_t targetRowEndSample) {
        if (pendingCapacity <= 0 || pendingRowPeriod == 0) return NULL;
        const uint64_t rowIndex = targetRowEndSample / pendingRowPeriod;
        PendingRow& row = pendingRows[static_cast<size_t>(rowIndex % static_cast<uint64_t>(pendingCapacity))];
        if (row.valid && row.targetRowEndSample != targetRowEndSample) return NULL;
        if (!row.valid) {
            row.power.fill(0.f);
            row.targetRowEndSample = targetRowEndSample;
            row.sourceAnalysisSample = 0;
            row.valid = true;
        }
        return &row;
    }

    PendingRow* findPendingRow(uint64_t targetRowEndSample) {
        if (pendingCapacity <= 0 || pendingRowPeriod == 0) return NULL;
        const uint64_t rowIndex = targetRowEndSample / pendingRowPeriod;
        PendingRow& row = pendingRows[static_cast<size_t>(rowIndex % static_cast<uint64_t>(pendingCapacity))];
        return row.valid && row.targetRowEndSample == targetRowEndSample ? &row : NULL;
    }
};

SpectrumAnalyzer::SpectrumAnalyzer() {
    for (int i = 0; i < static_cast<int>(FftSize::COUNT); ++i) {
        kernels[static_cast<size_t>(i)].reset(new Kernel(FFT_SIZES[static_cast<size_t>(i)]));
    }
    latestSpectrum.fill(INTERNAL_FLOOR_DB);
    rowMaximum.fill(INTERNAL_FLOOR_DB);
    configure(config);
}

SpectrumAnalyzer::~SpectrumAnalyzer() = default;

void SpectrumAnalyzer::configure(const SpectrumConfig& newConfig) {
    const SpectrumConfig oldConfig = config;
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
    config.analysisMode = static_cast<AnalysisMode>(
        clampValue(static_cast<int>(config.analysisMode), 0, static_cast<int>(AnalysisMode::COUNT) - 1));
    config.frequencyBins = static_cast<FrequencyBinScale>(
        clampValue(static_cast<int>(config.frequencyBins), 0,
                   static_cast<int>(FrequencyBinScale::COUNT) - 1));
    config.polyChannel = clampValue(config.polyChannel, 0, 15);
    config.sampleRate = std::max(config.sampleRate, 1000.f);
    rowPeriodSamples = std::max<uint64_t>(
        1, static_cast<uint64_t>(std::llround(config.sampleRate / rowsPerSecond(config.quality))));

    const bool analysisChanged =
        !wasConfigured || oldConfig.fftSize != config.fftSize || oldConfig.window != config.window ||
        oldConfig.fftOverlap != config.fftOverlap || oldConfig.channelMode != config.channelMode ||
        oldConfig.analysisMode != config.analysisMode ||
        oldConfig.frequencyBins != config.frequencyBins || oldConfig.polyChannel != config.polyChannel ||
        oldConfig.sampleRate != config.sampleRate || oldConfig.generation != config.generation;
    const bool qualityChanged = wasConfigured && oldConfig.quality != config.quality;
    activeKernel = kernels[static_cast<size_t>(fftIndex)].get();
    if (analysisChanged) {
        const bool captureCompatible =
            wasConfigured && oldConfig.fftSize == config.fftSize &&
            oldConfig.channelMode == config.channelMode &&
            oldConfig.polyChannel == config.polyChannel &&
            oldConfig.sampleRate == config.sampleRate;
        activeKernel->reset(config.sampleRate, config.fftOverlap, config.frequencyBins,
                            rowPeriodSamples, captureCompatible);
        processedSamples = 0;
        latestSpectrumSample = 0;
        haveSpectrum = false;
        rowHasSpectrum = false;
        latestSpectrum.fill(INTERNAL_FLOOR_DB);
        rowMaximum.fill(INTERNAL_FLOOR_DB);
        advancedBoundarySample = 0;
    } else if (qualityChanged && config.analysisMode == AnalysisMode::ADVANCED) {
        activeKernel->clearPending(rowPeriodSamples);
        advancedBoundarySample = currentTimelineSample;
    }
    resetRowClock();
}

void SpectrumAnalyzer::resetRowClock() {
    nextRowSample = processedSamples + rowPeriodSamples;
}

bool SpectrumAnalyzer::processSample(float normalizedSample, SpectrumRow& outputRow) {
    return processSample(normalizedSample, processedSamples + 1, outputRow);
}

bool SpectrumAnalyzer::processSample(float normalizedSample, uint64_t timelineSample, SpectrumRow& outputRow) {
    ++processedSamples;
    currentTimelineSample = timelineSample;
    if (!activeKernel->advancedGridInitialized && config.analysisMode == AnalysisMode::ADVANCED) {
        const uint64_t period = std::max<uint64_t>(activeKernel->pendingRowPeriod, 1);
        activeKernel->nextAdvancedRowSample =
            (timelineSample / period + 1) * period;
        activeKernel->advancedGridInitialized = true;
        advancedBoundarySample = std::max(advancedBoundarySample, timelineSample - (timelineSample > 0));
    }

    if (activeKernel->process(normalizedSample, config.window, config.analysisMode,
                              config.sampleRate)) {
        if (config.analysisMode == AnalysisMode::ADVANCED)
            finishAdvancedSpectrum(timelineSample);
        else
            finishSpectrum(timelineSample);
    }

    if (config.analysisMode == AnalysisMode::ADVANCED)
        return publishAdvancedRow(timelineSample, outputRow);

    if (processedSamples < nextRowSample) return false;
    publishRow(outputRow);
    do {
        nextRowSample += rowPeriodSamples;
    } while (nextRowSample <= processedSamples);
    rowMaximum.fill(INTERNAL_FLOOR_DB);
    rowHasSpectrum = false;
    return true;
}

void SpectrumAnalyzer::discardPending(uint64_t timelineSample) {
    if (!activeKernel || config.analysisMode != AnalysisMode::ADVANCED) return;
    activeKernel->clearPending(rowPeriodSamples);
    advancedBoundarySample = timelineSample;
    const uint64_t period = std::max<uint64_t>(rowPeriodSamples, 1);
    activeKernel->nextAdvancedRowSample =
        (timelineSample / period + 1) * period;
    activeKernel->advancedGridInitialized = true;
}

void SpectrumAnalyzer::finishSpectrum(uint64_t timelineSample) {
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

void SpectrumAnalyzer::publishRow(SpectrumRow& outputRow) {
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

void SpectrumAnalyzer::finishAdvancedSpectrum(uint64_t timelineSample) {
    const double frameCenter =
        static_cast<double>(timelineSample) - 0.5 * static_cast<double>(activeKernel->size - 1);
    const double period = static_cast<double>(std::max<uint64_t>(rowPeriodSamples, 1));
    const float cadenceNormalization =
        static_cast<float>(activeKernel->hopSize) / static_cast<float>(period);
    const float maximumFrequency = displayMaximumFrequency(config.sampleRate);

    for (int bin = 1; bin < activeKernel->size / 2; ++bin) {
        const Kernel::AdvancedBin& contribution =
            activeKernel->advancedBins[static_cast<size_t>(bin)];
        if (!contribution.valid) continue;
        const double reassignedTime =
            frameCenter + static_cast<double>(contribution.timeCorrectionSamples);
        if (!std::isfinite(reassignedTime) ||
            reassignedTime <= static_cast<double>(advancedBoundarySample))
            continue;

        const double timeCoordinate = reassignedTime / period;
        const double lowerIndexDouble = std::floor(timeCoordinate);
        if (!(lowerIndexDouble >= 0.0) ||
            lowerIndexDouble > static_cast<double>(std::numeric_limits<uint64_t>::max() /
                                                   std::max<uint64_t>(rowPeriodSamples, 1) - 1))
            continue;
        const uint64_t lowerIndex = static_cast<uint64_t>(lowerIndexDouble);
        const float upperTimeWeight =
            clampValue(static_cast<float>(timeCoordinate - lowerIndexDouble), 0.f, 1.f);
        const float lowerTimeWeight = 1.f - upperTimeWeight;

        const float coordinate =
            frequencyCoordinateForHz(contribution.frequencyHz, maximumFrequency,
                                     config.frequencyBins) *
            static_cast<float>(NUM_FREQUENCY_CELLS - 1);
        if (!std::isfinite(coordinate)) continue;
        const int lowerCell = clampValue(static_cast<int>(std::floor(coordinate)), 0,
                                         NUM_FREQUENCY_CELLS - 1);
        const int upperCell = std::min(lowerCell + 1, NUM_FREQUENCY_CELLS - 1);
        float upperFrequencyWeight = clampValue(coordinate - lowerCell, 0.f, 1.f);
        float lowerFrequencyWeight = 1.f - upperFrequencyWeight;
        if (lowerCell == upperCell) {
            lowerFrequencyWeight = 1.f;
            upperFrequencyWeight = 0.f;
        }

        const uint64_t rowTargets[2] = {
            lowerIndex * rowPeriodSamples,
            (lowerIndex + 1) * rowPeriodSamples,
        };
        const float timeWeights[2] = {lowerTimeWeight, upperTimeWeight};
        for (int timeSide = 0; timeSide < 2; ++timeSide) {
            if (!(timeWeights[timeSide] > 0.f) ||
                rowTargets[timeSide] <= advancedBoundarySample ||
                rowTargets[timeSide] < activeKernel->nextAdvancedRowSample)
                continue;
            Kernel::PendingRow* row = activeKernel->pendingRow(rowTargets[timeSide]);
            if (!row) continue;
            const float scaledPower =
                contribution.power * cadenceNormalization * timeWeights[timeSide];
            row->power[static_cast<size_t>(lowerCell)] +=
                scaledPower * lowerFrequencyWeight;
            row->power[static_cast<size_t>(upperCell)] +=
                scaledPower * upperFrequencyWeight;
            row->sourceAnalysisSample =
                std::max(row->sourceAnalysisSample, timelineSample);
        }
    }
}

bool SpectrumAnalyzer::publishAdvancedRow(uint64_t timelineSample, SpectrumRow& outputRow) {
    if (!activeKernel->advancedGridInitialized) return false;
    if (timelineSample <= static_cast<uint64_t>(activeKernel->size)) return false;
    const uint64_t finalizationLimit =
        timelineSample - static_cast<uint64_t>(activeKernel->size);
    if (!(activeKernel->nextAdvancedRowSample < finalizationLimit)) return false;

    Kernel::PendingRow* pending =
        activeKernel->findPendingRow(activeKernel->nextAdvancedRowSample);
    for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell) {
        const float power = pending ? pending->power[static_cast<size_t>(cell)] : 0.f;
        const float db = power > 0.f && std::isfinite(power)
                             ? 10.f * std::log10(power)
                             : INTERNAL_FLOOR_DB;
        outputRow.dbTenths[static_cast<size_t>(cell)] =
            quantizeDb(clampValue(std::isfinite(db) ? db : INTERNAL_FLOOR_DB,
                                  INTERNAL_FLOOR_DB, INTERNAL_CEILING_DB));
    }
    outputRow.rowEndSample = activeKernel->nextAdvancedRowSample;
    outputRow.sourceAnalysisSample =
        pending ? pending->sourceAnalysisSample : 0;
    outputRow.sampleRate = config.sampleRate;
    outputRow.fftSize = static_cast<uint32_t>(activeKernel->size);
    outputRow.effectiveHopSize = static_cast<uint32_t>(activeKernel->hopSize);
    outputRow.displayRowsPerSecond =
        static_cast<uint32_t>(rowsPerSecond(config.quality));
    outputRow.configGeneration = config.generation;

    if (pending) {
        pending->power.fill(0.f);
        pending->targetRowEndSample = 0;
        pending->sourceAnalysisSample = 0;
        pending->valid = false;
    }
    activeKernel->nextAdvancedRowSample += rowPeriodSamples;
    return true;
}

}  // namespace spectrum
}  // namespace cella
