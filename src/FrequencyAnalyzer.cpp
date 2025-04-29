#include "plugin.hpp"

struct VFDFreqAnalyzer : Module {
    enum ParamIds {
        NUM_BANDS_PARAM,
        FALL_DELAY_PARAM,
        PEAK_FALL_DELAY_PARAM,
        GAIN_PARAM,
        RESPONSIVENESS_PARAM,
        LOG_BINS_PARAM,
        PARAMS_LEN
    };
    enum InputIds {
        AUDIO_INPUT,
        INPUTS_LEN
    };
    enum OutputIds {
        OUTPUTS_LEN
    };
    enum LightIds {
        LIGHTS_LEN
    };

    const std::vector<size_t> WINDOW_SIZES = {256, 512, 1024, 2048, 4096, 8192};

    std::vector<float> sampleBuffer;  // Circular buffer for incoming audio
    size_t windowSize = 1024;
    size_t hopSize = 512;
    size_t bufferWritePos = 0;
    size_t samplesSinceLastFFT = 0;
    bool isBufferInitiallyFilled = false;

    // --- FFT related buffers (using aligned memory) ---
    dsp::RealFFT* fft = nullptr;  // Pointer to the FFT object
    float* fftInput = nullptr;    // Aligned buffer for windowed input samples
    float* fftOutput = nullptr;   // Aligned buffer for FFT results

    // --- Band analysis ---
    std::vector<float> bandLevels;
    std::vector<float> bandPeaks;
    float sampleRate = 44100.f;
    bool logarithmicBins = false;

    VFDFreqAnalyzer() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(NUM_BANDS_PARAM, 1.f, 25.f, 16.f, "Number of Bands");
        configParam(FALL_DELAY_PARAM, 0.1f, 2.f, 0.5f, "Fall Delay", "s");
        configParam(PEAK_FALL_DELAY_PARAM, 0.1f, 2.f, 1.f, "Peak Fall Delay", "s");
        configParam(GAIN_PARAM, 0.f, 2.f, 0.003f, "Gain");
        // Ensure default responsiveness index is valid
        int defaultResponsivenessIndex = 2;  // Corresponds to 1024
        if (defaultResponsivenessIndex >= (int)WINDOW_SIZES.size()) {
            defaultResponsivenessIndex = WINDOW_SIZES.size() - 1;
        }
        configParam(RESPONSIVENESS_PARAM, 0.f, (float)WINDOW_SIZES.size() - 1, (float)defaultResponsivenessIndex, "FFT Window Size", "", 0.f, 1.f);

        configParam(LOG_BINS_PARAM, 0.f, 1.f, 0.f, "Bins", {"Linear", "Logarithmic"});
        configInput(AUDIO_INPUT, "Audio");

        // Initial setup using helper functions
        updateWindowSize(WINDOW_SIZES[defaultResponsivenessIndex]);  // Default to 1024
        updateNumBands(16);                                          // Default bands
    }

    ~VFDFreqAnalyzer() {
        // --- Free aligned memory ---
        if (fftInput) {
            pffft_aligned_free(fftInput);
            fftInput = nullptr;
        }
        if (fftOutput) {
            pffft_aligned_free(fftOutput);
            fftOutput = nullptr;
        }
        // --- Delete the FFT object ---
        if (fft) {
            delete fft;
            fft = nullptr;
        }
    }

    // --- Helper function to handle window size changes ---
    void updateWindowSize(size_t newWindowSize) {
        // Only update if the size actually changes or if FFT object is not yet created
        if (windowSize == newWindowSize && fft != nullptr) return;

        windowSize = newWindowSize;
        hopSize = windowSize / 2;

        // --- Resize circular buffer (can remain std::vector) ---
        sampleBuffer.resize(windowSize, 0.f);
        std::fill(sampleBuffer.begin(), sampleBuffer.end(), 0.f);  // Clear buffer

        // --- Free existing aligned buffers ---
        if (fftInput) pffft_aligned_free(fftInput);
        if (fftOutput) pffft_aligned_free(fftOutput);

        // --- Allocate new aligned buffers ---
        // Use pffft_aligned_malloc as required/recommended by dsp::RealFFT
        fftInput = static_cast<float*>(pffft_aligned_malloc(windowSize * sizeof(float)));
        fftOutput = static_cast<float*>(pffft_aligned_malloc(windowSize * sizeof(float)));

        // Basic check for allocation success
        if (!fftInput || !fftOutput) {
            WARN("Failed to allocate aligned memory for FFT buffers. Size: %zu", windowSize);
            // Handle error appropriately, maybe disable processing?
            if (fftInput) pffft_aligned_free(fftInput);
            if (fftOutput) pffft_aligned_free(fftOutput);
            fftInput = nullptr;
            fftOutput = nullptr;
            if (fft) delete fft;
            fft = nullptr;
            windowSize = 0;  // Indicate an invalid state
            return;
        }

        // Clear new buffers (optional but good practice)
        std::memset(fftInput, 0, windowSize * sizeof(float));
        std::memset(fftOutput, 0, windowSize * sizeof(float));

        // --- Recreate FFT object ---
        if (fft) delete fft;
        fft = new dsp::RealFFT(windowSize);

        // --- Reset buffer state ---
        bufferWritePos = 0;
        samplesSinceLastFFT = 0;
        isBufferInitiallyFilled = false;

        // rack::INFO("FFT Analyzer: Window Size set to %zu, Hop Size %zu", windowSize, hopSize);
    }

    // --- Helper function to handle band changes ---
    void updateNumBands(size_t newBands) {
        if (bandLevels.size() == newBands) return;
        bandLevels.resize(newBands, 0.f);
        bandPeaks.resize(newBands, 0.f);
        // rack::INFO("FFT Analyzer: Number of bands set to %zu", newBands);
    }

    void onSampleRateChange() override {
        // Recalculate sampleRate dependent things if needed,
        // Here, sampleRate is used within processFFT, so just updating the member is sufficient.
        sampleRate = APP->engine->getSampleRate();
    }

    void process(const ProcessArgs& args) override {
        // Ensure sample rate is up-to-date (can change without calling onSampleRateChange sometimes)
        if (sampleRate != args.sampleRate) {
            sampleRate = args.sampleRate;
        }

        // --- Handle Parameter Changes ---
        logarithmicBins = (bool)params[LOG_BINS_PARAM].getValue();
        size_t newBands = static_cast<size_t>(std::max(1, (int)params[NUM_BANDS_PARAM].getValue()));
        updateNumBands(newBands);

        // --- Handle Responsiveness (Window Size) Parameter Change ---
        size_t responsivenessIndex = static_cast<size_t>(params[RESPONSIVENESS_PARAM].getValue());
        if (responsivenessIndex < WINDOW_SIZES.size()) {
            size_t newWindowSize = WINDOW_SIZES[responsivenessIndex];
            updateWindowSize(newWindowSize);  // Call helper which handles reallocation etc.
        } else {
            // Handle potential invalid index if slider range/step changes?
            WARN("Invalid responsiveness index: %zu", responsivenessIndex);
        }

        // --- Check if FFT is valid before proceeding ---
        if (!fft || !fftInput || !fftOutput || windowSize == 0) {
            // Clear levels if FFT isn't ready
            std::fill(bandLevels.begin(), bandLevels.end(), 0.f);
            std::fill(bandPeaks.begin(), bandPeaks.end(), 0.f);
            return;  // Cannot process
        }

        // --- Process Audio Input ---
        if (inputs[AUDIO_INPUT].isConnected()) {
            float gain = params[GAIN_PARAM].getValue();
            float summedInput = 0.f;
            int channels = inputs[AUDIO_INPUT].getChannels();

            // Input Handling: Average multi-channel, take mono as is
            if (channels == 1) {
                summedInput = inputs[AUDIO_INPUT].getVoltage(0) * gain * 0.1f;
            } else if (channels > 1) {
                for (int c = 0; c < channels; c++) {
                    summedInput += inputs[AUDIO_INPUT].getVoltage(c);
                }
                summedInput = (summedInput / channels) * gain * 0.1f;  // Average channels
            }
            // else: channels = 0, summedInput remains 0.f

            // --- Store sample in circular buffer ---
            sampleBuffer[bufferWritePos] = summedInput;
            bufferWritePos = (bufferWritePos + 1) % windowSize;
            samplesSinceLastFFT++;

            // --- Track initial fill ---
            if (!isBufferInitiallyFilled && samplesSinceLastFFT >= windowSize) {
                isBufferInitiallyFilled = true;
                samplesSinceLastFFT = 0;  // Ready for the first hop
            }

            // --- Trigger FFT processing on hop ---
            if (isBufferInitiallyFilled && samplesSinceLastFFT >= hopSize) {
                prepareFFTInput();        // Copy from circular buffer to linear aligned fftInput
                processFFT();             // Perform FFT and update bands using fftInput/fftOutput
                samplesSinceLastFFT = 0;  // Reset hop counter
            }

        } else {
            // --- Input disconnected: Slowly decay levels ---
            float deltaTime = args.sampleTime;  // Use Rack's delta time
                                                // Prevent division by zero for delay times
            float fallDelay = std::max(0.001f, params[FALL_DELAY_PARAM].getValue());
            float peakFallDelay = std::max(0.001f, params[PEAK_FALL_DELAY_PARAM].getValue());
            float fallDecay = std::exp(-deltaTime / fallDelay);
            float peakDecay = std::exp(-deltaTime / peakFallDelay);

            for (size_t b = 0; b < bandLevels.size(); b++) {
                bandLevels[b] *= fallDecay;
                bandPeaks[b] *= peakDecay;
                // Ensure levels don't go negative due to float inaccuracy
                bandLevels[b] = std::max(0.f, bandLevels[b]);
                bandPeaks[b] = std::max(0.f, bandPeaks[b]);
            }
            // Reset buffer state if input disconnects
            isBufferInitiallyFilled = false;
            samplesSinceLastFFT = 0;
            bufferWritePos = 0;
            // Optionally clear the circular buffer
            // std::fill(sampleBuffer.begin(), sampleBuffer.end(), 0.f);
        }
    }

    // --- Helper to copy data from circular buffer to linear aligned FFT input ---
    void prepareFFTInput() {
        if (!fftInput || windowSize == 0) return;  // Safety check

        // Calculate the start position in the circular buffer for the segment
        size_t readStartPos = (bufferWritePos + windowSize) % windowSize;  // Correct wrap-around logic

        if (readStartPos + windowSize <= sampleBuffer.size()) {
            // No wrap-around needed for the read segment
            // Use std::copy for potentially better performance than memcpy with floats?
            std::copy(sampleBuffer.begin() + readStartPos,
                      sampleBuffer.begin() + readStartPos + windowSize,
                      fftInput);  // Copy directly to the aligned buffer fftInput
        } else {
            // Read wraps around the end of the circular buffer
            size_t firstChunkSize = sampleBuffer.size() - readStartPos;
            size_t secondChunkSize = windowSize - firstChunkSize;

            // Use std::copy
            std::copy(sampleBuffer.begin() + readStartPos,
                      sampleBuffer.end(),
                      fftInput);
            std::copy(sampleBuffer.begin(),
                      sampleBuffer.begin() + secondChunkSize,
                      fftInput + firstChunkSize);  // Offset destination pointer
        }
    }

    void processFFT() {
        // Safety checks
        if (!fft || !fftInput || !fftOutput || windowSize == 0 || sampleRate <= 0) return;

        // --- Apply Blackman window function and remove DC offset ---
        // Operate directly on the prepared fftInput buffer
        float sum = 0.0f;
        for (size_t i = 0; i < windowSize; i++) {
            sum += fftInput[i];
        }
        float dcOffset = (windowSize > 0) ? (sum / windowSize) : 0.f;

        // Blackman Window Calculation
        const float a0 = 0.35875f;
        const float a1 = 0.48829f;
        const float a2 = 0.14128f;
        const float a3 = 0.01168f;
        const float k = 2.f * M_PI / (windowSize - 1);
        for (size_t i = 0; i < windowSize; ++i) {
            float c1 = std::cos(k * i);
            float c2 = std::cos(k * 2.f * i);
            float c3 = std::cos(k * 3.f * i);
            float w = a0 - a1 * c1 + a2 * c2 - a3 * c3;
            fftInput[i] = (fftInput[i] - dcOffset) * w;
        }

        // --- Perform FFT using dsp::RealFFT ---
        fft->rfft(fftInput, fftOutput);  // Real FFT (in-place for real part, complex output)

        // --- Magnitude Calculation (Adapted for dsp::RealFFT output format) ---
        const size_t N = windowSize;
        const size_t numBins = N / 2 + 1;  // Number of unique frequency bins
        const float binWidth = sampleRate / N;

        // Define audible frequency range limits in terms of bins
        // Bin 0 is DC, bin N/2 is Nyquist
        const size_t minBin = std::max<size_t>(2, static_cast<size_t>(std::ceil(40.0f / binWidth)));          // Skip DC
        const size_t maxBin = std::min<size_t>(static_cast<size_t>(std::floor(20000.0f / binWidth)), N / 2);  // Up to Nyquist

        // Vector to store power (magnitude squared) for each bin
        std::vector<float> power(numBins, 0.f);
        const size_t N_half = N / 2;

        // Calculate Power from dsp::RealFFT output (interleaved format)
        // Output Format: [Re(0), Re(N/2), Re(1), Im(1), Re(2), Im(2), ..., Re(N/2-1), Im(N/2-1)]
        if (N > 0) {
            // DC component (Bin 0) - Real part is fftOutput[0]
            power[0] = fftOutput[0] * fftOutput[0];

            // Nyquist component (Bin N/2) - Real part is fftOutput[1] (pffft convention)
            if (N_half > 0 && N_half < numBins) {  // Check index validity
                power[N_half] = fftOutput[1] * fftOutput[1];
            }

            // Other bins (k = 1 to N/2 - 1)
            for (size_t k = 1; k < N_half; k++) {
                size_t indexReal = 2 * k;
                size_t indexImag = 2 * k + 1;
                // Check index bounds before accessing
                if (indexImag < N) {  // fftOutput has size N
                    float re = fftOutput[indexReal];
                    float im = fftOutput[indexImag];
                    power[k] = re * re + im * im;  // Power = Re^2 + Im^2
                    power[k] *= 2.f;
                } else {
                    // Should not happen if loop condition is correct, but safety first
                    power[k] = 0.f;
                }
            }
        }

        // --- Bin Grouping (Logarithmic/Linear) using PEAK dB ---
        size_t numBands = bandLevels.size();
        std::vector<float> newLevels(numBands, 0.f);  // Store new levels temporarily
        const float epsilon = 1e-12f;                 // To prevent log10(0)

        // Check if valid range exists
        if (maxBin < minBin || numBands == 0) {
            // If no valid bins or no bands, clear levels and return
            std::fill(bandLevels.begin(), bandLevels.end(), 0.f);
            std::fill(bandPeaks.begin(), bandPeaks.end(), 0.f);
            return;
        }

        if (logarithmicBins) {
            const float minFreq = 20.0f;
            const float maxFreq = std::min(20000.0f, sampleRate / 2.0f);
            // Ensure minFreq < maxFreq to avoid issues with pow or log
            if (minFreq >= maxFreq) {
                std::fill(bandLevels.begin(), bandLevels.end(), 0.f);
                std::fill(bandPeaks.begin(), bandPeaks.end(), 0.f);
                return;
            }
            const float freqRatio = maxFreq / minFreq;
            const float logFreqRatio = std::log(freqRatio);  // Use log for potentially better precision

            for (size_t b = 0; b < numBands; b++) {
                float relStart = static_cast<float>(b) / numBands;
                float relEnd = static_cast<float>(b + 1) / numBands;
                // More numerically stable way for log scale?
                // float bandStartFreq = minFreq * std::pow(freqRatio, relStart);
                // float bandEndFreq = minFreq * std::pow(freqRatio, relEnd);
                float bandStartFreq = minFreq * std::exp(relStart * logFreqRatio);
                float bandEndFreq = minFreq * std::exp(relEnd * logFreqRatio);

                // Convert frequency edges to bin indices
                size_t startBin = static_cast<size_t>(bandStartFreq / binWidth);
                size_t endBin = static_cast<size_t>(bandEndFreq / binWidth);

                // Clamp bins to valid/audible range [minBin, maxBin]
                startBin = clamp((float)startBin, minBin, maxBin);
                // endBin needs careful clamping: It's the exclusive end, so clamp to maxBin+1
                endBin = clamp((float)endBin, minBin, maxBin + 1);
                // Ensure at least one potential bin in the range, and start <= end
                endBin = std::max(endBin, startBin + 1);

                /* mean power (not max) for a fair comparison across bands */
                double sumP = 0.0;
                size_t samples = 0;
                for (size_t i = startBin; i < endBin && i < numBins; i++) {
                    sumP += power[i];
                    ++samples;
                }
                if (samples) {
                    float bandDb = 10.f * std::log10((sumP / samples) + epsilon);
                    newLevels[b] = rack::math::clamp((bandDb + 60.f) / 60.f, 0.f, 1.f);
                } else {
                    newLevels[b] = 0.f;
                }
            }
        } else {  // Linear Bins
            const size_t audibleBins = maxBin - minBin + 1;
            // Calculate bins per band, ensuring at least 1
            // Use floating point division for potentially more even distribution
            float binsPerBandFloat = static_cast<float>(audibleBins) / numBands;

            size_t currentBin = minBin;
            for (size_t b = 0; b < numBands; b++) {
                size_t start = currentBin;
                // Calculate end bin index for this band
                size_t end = (b == numBands - 1)
                                 ? (maxBin + 1)  // Last band takes all remaining bins up to maxBin
                                 : minBin + static_cast<size_t>(std::round((b + 1) * binsPerBandFloat));

                // Clamp bins to valid range [minBin, maxBin+1] and ensure progress
                start = clamp((float)start, minBin, maxBin);
                end = clamp((float)end, minBin, maxBin + 1);
                end = std::max(end, start);  // Ensure end >= start

                float maxDbInBand = -200.0f;  // Initialize low
                bool bandHasData = false;

                // Find the maximum dB value within this band's bins
                for (size_t i = start; i < end && i < numBins; i++) {  // Iterate bins in the band
                                                                       // Use the calculated power vector
                    float currentDb = 10.0f * std::log10(power[i] + epsilon);
                    maxDbInBand = std::max(maxDbInBand, currentDb);
                    bandHasData = true;
                }

                if (bandHasData) {
                    // Normalize the MAXIMUM dB value found in the band to [0, 1]
                    newLevels[b] = rack::math::clamp((maxDbInBand + 60.0f) / 60.0f, 0.0f, 1.0f);
                } else {
                    newLevels[b] = 0.f;  // Band had no valid bins
                }
                // Prepare start bin for the next band
                currentBin = end;
                // Safety break if currentBin gets stuck (shouldn't happen with std::max(end, start+1))
                if (currentBin > maxBin && b < numBands - 1) break;
            }
        }

        // --- Update Band Levels & Peaks (using calculated newLevels) ---
        float deltaTime = (float)hopSize / sampleRate;  // Time between FFT calculations
        // Ensure delays are positive to avoid issues with std::exp
        float fallDelay = std::max(0.001f, params[FALL_DELAY_PARAM].getValue());
        float peakFallDelay = std::max(0.001f, params[PEAK_FALL_DELAY_PARAM].getValue());
        float fallDecay = std::exp(-deltaTime / fallDelay);
        float peakDecay = std::exp(-deltaTime / peakFallDelay);

        for (size_t b = 0; b < numBands; b++) {
            float newLevel = newLevels[b];

            // Update main level (fast attack, exponential decay)
            if (newLevel >= bandLevels[b]) {
                bandLevels[b] = newLevel;
            } else {
                bandLevels[b] *= fallDecay;
                // Prevent denormals or excessive decay
                if (bandLevels[b] < 1e-6f) bandLevels[b] = 0.f;
            }

            // Update peak level (fast attack, slower exponential decay)
            if (newLevel >= bandPeaks[b]) {
                bandPeaks[b] = newLevel;
            } else {
                bandPeaks[b] *= peakDecay;
                if (bandPeaks[b] < 1e-6f) bandPeaks[b] = 0.f;
            }

            // Clamp levels just in case (though normalization should handle it)
            bandLevels[b] = rack::math::clamp(bandLevels[b], 0.f, 1.f);
            bandPeaks[b] = rack::math::clamp(bandPeaks[b], 0.f, 1.f);
        }
    }  // End processFFT
};

struct VFDDisplay : LedDisplay {
    VFDFreqAnalyzer* module;
    const float dotRadius = 2.0f;
    const float dotSpacing = 2.0f;
    // const NVGcolor activeColor = nvgRGB(0x90, 0xFF, 0x30);
    const NVGcolor activeColor = nvgRGB(0x93, 0xEA, 0xFF);
    const NVGcolor inactiveColor = nvgRGB(0x20, 0x20, 0x20);
    const NVGcolor peakColor = nvgRGB(0xFF, 0x30, 0x30);

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1 || !module) return;

        nvgSave(args.vg);

        // Draw dark background
        // nvgBeginPath(args.vg);
        // nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        // nvgFillColor(args.vg, nvgRGB(0x10, 0x10, 0x10));
        // nvgFill(args.vg);

        size_t numBands = module->bandLevels.size();
        if (numBands < 1) {
            nvgRestore(args.vg);
            return;
        }

        const float bandMargin = 3.0f;
        const float totalWidth = box.size.x - 2 * bandMargin;
        const float bandWidth = totalWidth / numBands;

        for (size_t b = 0; b < numBands; b++) {
            float level = module->bandLevels[b];
            float peak = module->bandPeaks[b];
            float x = bandMargin + b * bandWidth;

            // Calculate dot grid dimensions
            float availableWidth = bandWidth - bandMargin * 2;
            int cols = calculateColumnCount(availableWidth);
            int rows = calculateRowCount(box.size.y - 3 * bandMargin);

            // Center the grid horizontally
            float gridWidth = cols * (dotRadius * 2 + dotSpacing) - dotSpacing;
            float xStart = x + (availableWidth - gridWidth) / 2 + bandMargin;

            // Vertical positioning
            float yStart = 2 * bandMargin + 1.5;
            float gridHeight = rows * (dotRadius * 2 + dotSpacing) - dotSpacing;
            float yStep = (box.size.y - bandMargin * 2 - gridHeight) / (rows - 1);

            // Draw inactive dots
            drawDotGrid(args.vg, xStart, yStart, cols, rows, inactiveColor);

            // Draw active dots
            if (level > 0.0f) {
                float activeHeight = box.size.y * level;
                drawActiveDots(args.vg, xStart, yStart, cols, rows, activeHeight);
            }

            // Draw peak indicator
            if (peak > 0.0f) {
                nvgFillColor(args.vg, peakColor);
                float peakY = box.size.y - (box.size.y * peak);
                int r = rows - ceil(peak * rows);
                float dy = yStart + r * (dotRadius * 2 + dotSpacing);
                for (int c = 0; c < cols; c++) {
                    float dx = xStart + c * (dotRadius * 2 + dotSpacing);

                    nvgBeginPath(args.vg);
                    nvgCircle(args.vg, dx, dy, dotRadius);
                    nvgFill(args.vg);
                }
            }
        }

        nvgRestore(args.vg);
    }

   private:
    int calculateColumnCount(float availableWidth) {
        int cols = static_cast<int>((availableWidth + dotSpacing) /
                                    (dotRadius * 2 + dotSpacing));
        return clamp(cols, 3, 100);
    }

    int calculateRowCount(float availableHeight) {
        int rows = static_cast<int>((availableHeight + dotSpacing) /
                                    (dotRadius * 2 + dotSpacing));
        return rows;
    }

    void drawDotGrid(NVGcontext* vg, float x, float y, int cols, int rows, NVGcolor color) {
        nvgFillColor(vg, color);
        for (int c = 0; c < cols; c++) {
            for (int r = 0; r < rows; r++) {
                float dx = x + c * (dotRadius * 2 + dotSpacing);
                float dy = y + r * (dotRadius * 2 + dotSpacing);
                nvgBeginPath(vg);
                nvgCircle(vg, dx, dy, dotRadius);
                nvgFill(vg);
            }
        }
    }

    void drawActiveDots(NVGcontext* vg, float x, float y, int cols, int rows, float activeHeight) {
        nvgFillColor(vg, activeColor);
        for (int c = 0; c < cols; c++) {
            for (int r = 0; r < rows; r++) {
                float dy = y + r * (dotRadius * 2 + dotSpacing);
                if (box.size.y - dy <= activeHeight) {
                    float dx = x + c * (dotRadius * 2 + dotSpacing);

                    NVGpaint paint = nvgRadialGradient(vg, dx, dy, dotRadius * 0.f, dotRadius * 3, nvgTransRGBA(activeColor, 100), nvgTransRGBA(activeColor, 0.f));

                    nvgBeginPath(vg);
                    nvgCircle(vg, dx, dy, 3 * dotRadius);
                    nvgFillPaint(vg, paint);
                    nvgFill(vg);

                    nvgBeginPath(vg);
                    nvgCircle(vg, dx, dy, dotRadius);
                    nvgFillColor(vg, activeColor);
                    nvgFill(vg);
                }
            }
        }
    }
};

struct VFDFreqAnalyzerWidget : ModuleWidget {
    VFDFreqAnalyzerWidget(VFDFreqAnalyzer* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/VFDFreqAnalyzer.svg"), asset::plugin(pluginInstance, "res/VFDFreqAnalyzer-dark.svg")));

        VFDDisplay* display = new VFDDisplay();
        display->module = module;
        display->box.pos = Vec(0.0, 25.0);
        display->box.size = Vec(496.0, 280.0);
        addChild(display);

        addParam(createParamCentered<RoundBlackKnob>(Vec(160, 329.25), module, VFDFreqAnalyzer::NUM_BANDS_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(205, 329.25), module, VFDFreqAnalyzer::FALL_DELAY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(250, 329.25), module, VFDFreqAnalyzer::PEAK_FALL_DELAY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(295, 329.25), module, VFDFreqAnalyzer::GAIN_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(340, 329.25), module, VFDFreqAnalyzer::RESPONSIVENESS_PARAM));
        // addParam(createParamCentered<VCVButton>(Vec(385, 329.25), module, VFDFreqAnalyzer::LOG_BINS_PARAM));
        addParam(createParamCentered<CKSS>(Vec(385, 329.25), module, VFDFreqAnalyzer::LOG_BINS_PARAM));

        addInput(createInputCentered<PJ301MPort>(Vec(115, 329.25), module, VFDFreqAnalyzer::AUDIO_INPUT));
    }
};

Model* modelVFDFreqAnalyzer = createModel<VFDFreqAnalyzer, VFDFreqAnalyzerWidget>("VFDFreqAnalyzer");