#include <ffft/FFTReal.h>  // Include the FFTReal library header

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

    std::vector<float> sampleBuffer;  // Will act as a circular buffer
    size_t windowSize = 1024;
    size_t hopSize = 512;                  // --- New: Hop size (windowSize / 2)
    size_t bufferWritePos = 0;             // --- New: Current writing position in the circular buffer
    size_t samplesSinceLastFFT = 0;        // --- New: Counter for triggering FFT based on hop size
    bool isBufferInitiallyFilled = false;  // --- New: Tracks if buffer has enough data for first FFT

    std::vector<float> fftInput;  // Linear buffer for FFT library input
    std::vector<float> fftOutput;
    ffft::FFTReal<float>* fft = nullptr;  // Initialize to nullptr
    std::vector<float> bandLevels;
    std::vector<float> bandPeaks;
    float sampleRate = 44100.f;
    bool logarithmicBins = false;
    // dsp::SchmittTrigger logBinTrigger; // Removed as unused

    VFDFreqAnalyzer() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(NUM_BANDS_PARAM, 1.f, 25.f, 16.f, "Number of Bands");
        configParam(FALL_DELAY_PARAM, 0.1f, 2.f, 0.5f, "Fall Delay", "s");
        configParam(PEAK_FALL_DELAY_PARAM, 0.1f, 2.f, 1.f, "Peak Fall Delay", "s");
        configParam(GAIN_PARAM, 0.f, 2.f, 1.f, "Gain");
        configParam(RESPONSIVENESS_PARAM, 0.f, 5.f, 2.f, "FFT Window Size", "", 0.f, 1.f, WINDOW_SIZES.size() - 1);
        configParam(LOG_BINS_PARAM, 0.f, 1.f, 0.f, "Bins", {"Linear", "Logarithmic"});
        configInput(AUDIO_INPUT, "Audio");

        // Initialize with default window size (will call helper function)
        updateWindowSize(WINDOW_SIZES[2]);  // Default to 1024
        updateNumBands(16);                 // Default bands
    }

    ~VFDFreqAnalyzer() {
        if (fft) {
            delete fft;
            fft = nullptr;
        }
    }

    // --- New helper function to handle window size changes ---
    void updateWindowSize(size_t newWindowSize) {
        if (windowSize == newWindowSize && fft != nullptr) return;  // No change needed

        windowSize = newWindowSize;
        hopSize = windowSize / 2;

        // Resize buffers
        sampleBuffer.resize(windowSize, 0.f);  // Initialize with zeros
        fftInput.resize(windowSize);
        fftOutput.resize(windowSize);

        // Reset buffer state
        bufferWritePos = 0;
        samplesSinceLastFFT = 0;
        isBufferInitiallyFilled = false;
        std::fill(sampleBuffer.begin(), sampleBuffer.end(), 0.f);  // Clear buffer explicitly

        // Recreate FFT object
        if (fft) delete fft;
        fft = new ffft::FFTReal<float>(windowSize);
        // rack::INFO("FFT Analyzer: Window Size set to %zu, Hop Size %zu", windowSize, hopSize);
    }

    // --- New helper function to handle band changes ---
    void updateNumBands(size_t newBands) {
        if (bandLevels.size() == newBands) return;  // No change needed
        bandLevels.resize(newBands, 0.f);
        bandPeaks.resize(newBands, 0.f);
        // rack::INFO("FFT Analyzer: Number of bands set to %zu", newBands);
    }

    void process(const ProcessArgs& args) override {
        // --- Update sample rate ---
        if (sampleRate != args.sampleRate) {
            sampleRate = args.sampleRate;
            // No buffer reset needed just for sample rate change, decay times will adjust implicitly
        }

        // --- Handle Parameter Changes ---
        logarithmicBins = (bool)params[LOG_BINS_PARAM].getValue();
        size_t newBands = static_cast<size_t>(std::max(1, (int)params[NUM_BANDS_PARAM].getValue()));
        updateNumBands(newBands);  // Use helper

        size_t newWindowSize = WINDOW_SIZES[static_cast<int>(params[RESPONSIVENESS_PARAM].getValue())];
        updateWindowSize(newWindowSize);  // Use helper

        // --- Process Audio Input ---
        if (inputs[AUDIO_INPUT].isConnected()) {
            float gain = params[GAIN_PARAM].getValue();
            float summedInput = 0.f;
            int channels = inputs[AUDIO_INPUT].getChannels();

            // --- Input Handling: Average stereo, take mono as is ---
            // --- Removed the division by 10.f ---
            if (channels == 1) {
                summedInput = inputs[AUDIO_INPUT].getVoltage(0) * gain;
            } else {
                for (int c = 0; c < channels; c++) {
                    summedInput += inputs[AUDIO_INPUT].getVoltage(c);
                }
                summedInput = (summedInput / channels) * gain;  // Average channels
            }

            // --- Store sample in circular buffer ---
            sampleBuffer[bufferWritePos] = summedInput;
            bufferWritePos = (bufferWritePos + 1) % windowSize;  // Wrap around
            samplesSinceLastFFT++;

            // --- Track initial fill ---
            if (!isBufferInitiallyFilled && samplesSinceLastFFT >= windowSize) {
                isBufferInitiallyFilled = true;
                samplesSinceLastFFT = 0;  // Reset counter to start counting for the *first* hop
                                          // rack::INFO("Buffer initially filled.");
            }

            // --- Trigger FFT processing on hop ---
            if (isBufferInitiallyFilled && samplesSinceLastFFT >= hopSize) {
                prepareFFTInput();        // Copy from circular buffer to linear FFT input
                processFFT();             // Perform FFT and update bands
                samplesSinceLastFFT = 0;  // Reset hop counter
            }

        } else {
            // Optional: If no input, maybe slowly decay levels?
            // Or just do nothing, levels will hold/decay based on last FFT.
            // Let's add a slow decay if input disconnects:
            float deltaTime = args.sampleTime;  // Use Rack's provided delta time for this case
            float fallDecay = std::exp(-deltaTime / params[FALL_DELAY_PARAM].getValue());
            float peakDecay = std::exp(-deltaTime / params[PEAK_FALL_DELAY_PARAM].getValue());
            for (size_t b = 0; b < bandLevels.size(); b++) {
                bandLevels[b] *= fallDecay;
                bandPeaks[b] *= peakDecay;
            }
            isBufferInitiallyFilled = false;  // Reset fill state if input disconnects
            samplesSinceLastFFT = 0;
            bufferWritePos = 0;  // Reset position too
        }
    }

    // --- New helper to copy data from circular buffer to linear FFT input ---
    void prepareFFTInput() {
        // The `windowSize` samples we need end just BEFORE the current `bufferWritePos`.
        // The starting position of the segment in the circular buffer:
        size_t readStartPos = (bufferWritePos + windowSize) % windowSize;  // + windowSize handles wrap-around correctly

        if (readStartPos + windowSize <= sampleBuffer.size()) {
            // No wrap-around needed for the read segment
            std::copy(sampleBuffer.begin() + readStartPos,
                      sampleBuffer.begin() + readStartPos + windowSize,
                      fftInput.begin());
        } else {
            // Read wraps around the end of the circular buffer
            size_t firstChunkSize = sampleBuffer.size() - readStartPos;
            size_t secondChunkSize = windowSize - firstChunkSize;

            std::copy(sampleBuffer.begin() + readStartPos,
                      sampleBuffer.end(),
                      fftInput.begin());
            std::copy(sampleBuffer.begin(),
                      sampleBuffer.begin() + secondChunkSize,
                      fftInput.begin() + firstChunkSize);
        }
    }

    void processFFT() {
        if (!fft) return;  // Safety check

        // --- Apply Blackman window function and remove DC offset ---
        // Note: Operate on fftInput which now holds the correct time-domain window
        float sum = 0.0f;
        for (size_t i = 0; i < windowSize; i++) {
            sum += fftInput[i];
        }
        float dcOffset = sum / windowSize;

        // --- Blackman Window Calculation ---
        const float a0 = 0.42f;
        const float a1 = 0.50f;
        const float a2 = 0.08f;
        // Pre-calculate the factor for efficiency
        const float factor = 2.f * M_PI / (windowSize > 1 ? (windowSize - 1) : 1);  // Avoid division by zero if windowSize=1

        for (size_t i = 0; i < windowSize; i++) {
            float term1 = std::cos(factor * i);
            float term2 = std::cos(2.f * factor * i);
            float window = a0 - a1 * term1 + a2 * term2;
            // Apply window and remove DC offset *from the prepared fftInput*
            fftInput[i] = (fftInput[i] - dcOffset) * window;
        }
        // --- End Blackman Window ---

        // Perform FFT using FFTReal
        fft->do_fft(fftOutput.data(), fftInput.data());

        // --- Magnitude Calculation (Mostly Unchanged Logic) ---
        const size_t N = windowSize;
        const size_t numBins = N / 2 + 1;
        const float binWidth = sampleRate / N;

        // minBin calculation ensures we skip DC bin 0 later
        const size_t minBin = std::max<size_t>(1, std::ceil(20.0f / binWidth));
        const size_t maxBin = std::min<size_t>(std::floor(20000.0f / binWidth), numBins - 1);

        // Using a temporary vector for magnitudes (Power = Mag^2)
        std::vector<float> magnitudes(numBins, 0.f);
        const size_t N_half = N / 2;

        // Calculate Magnitudes (Power) - Assuming validated FFTReal indexing
        magnitudes[0] = fftOutput[0] * fftOutput[0];  // DC power (usually ignored later)
        if (N % 2 == 0) {                             // Nyquist
            magnitudes[N_half] = fftOutput[N_half] * fftOutput[N_half];
        }
        for (size_t k = 1; k < N_half; k++) {  // Bins 1 to N/2 - 1
            float re = fftOutput[k];
            float im = fftOutput[N_half + k];   // Verified index for FFTReal
            magnitudes[k] = re * re + im * im;  // Power
        }

        // --- Bin Grouping (Logarithmic/Linear) using PEAK dB ---
        size_t numBands = bandLevels.size();
        std::vector<float> newLevels(numBands, 0.f);  // Store new levels temporarily
        const float epsilon = 1e-12f;                 // To prevent log10(0)

        if (logarithmicBins) {
            const float minFreq = 20.0f;  // Use actual min frequency for range calculation
            const float maxFreq = std::min(20000.0f, sampleRate / 2.0f);
            const float freqRatio = maxFreq / minFreq;

            for (size_t b = 0; b < numBands; b++) {
                float relStart = static_cast<float>(b) / numBands;
                float relEnd = static_cast<float>(b + 1) / numBands;
                float bandStartFreq = minFreq * std::pow(freqRatio, relStart);
                float bandEndFreq = minFreq * std::pow(freqRatio, relEnd);

                // Convert frequency edges to bin indices
                size_t startBin = static_cast<size_t>(bandStartFreq / binWidth);
                size_t endBin = static_cast<size_t>(bandEndFreq / binWidth);

                // Clamp bins to valid/audible range (minBin already excludes DC)
                startBin = clamp((float)startBin, minBin, maxBin);
                endBin = clamp((float)endBin, minBin, maxBin + 1);  // Allow endBin to reach maxBin+1 for loop condition
                endBin = std::max(endBin, startBin + 1);            // Ensure at least one bin potential

                float maxDbInBand = -200.0f;  // Initialize to a very low dB value
                bool bandHasData = false;

                // Find the maximum dB value within this band's bins
                for (size_t i = startBin; i < endBin && i < numBins; i++) {  // Iterate bins in the band
                    float currentDb = 10.0f * std::log10(magnitudes[i] + epsilon);
                    maxDbInBand = std::max(maxDbInBand, currentDb);
                    bandHasData = true;  // Mark that we processed at least one bin
                }

                if (bandHasData) {
                    // Normalize the MAXIMUM dB value found in the band to [0, 1]
                    // Using -60dB to 0dB range for normalization
                    newLevels[b] = clamp((maxDbInBand + 60.0f) / 60.0f, 0.0f, 1.0f);
                } else {
                    newLevels[b] = 0.f;  // Band had no valid bins
                }
            }
        } else {                          // Linear Bins
                                          // Ensure we only consider bins within the audible range
            if (maxBin < minBin) return;  // Avoid issues if range is invalid
            const size_t audibleBins = maxBin - minBin + 1;
            if (audibleBins < 1 || numBands < 1) {
                return;
            }

            // Calculate bins per band, ensuring at least 1
            size_t binsPerBand = std::max<size_t>(1, audibleBins / numBands);

            for (size_t b = 0; b < numBands; b++) {
                size_t start = minBin + b * binsPerBand;
                // Ensure the last band goes all the way to maxBin
                size_t end = (b == numBands - 1) ? (maxBin + 1) : std::min(start + binsPerBand, maxBin + 1);
                start = std::min(start, maxBin);  // Clamp start just in case

                float maxDbInBand = -200.0f;  // Initialize to a very low dB value
                bool bandHasData = false;

                // Find the maximum dB value within this band's bins
                for (size_t i = start; i < end && i < numBins; i++) {  // Iterate bins in the band
                    float currentDb = 10.0f * std::log10(magnitudes[i] + epsilon);
                    maxDbInBand = std::max(maxDbInBand, currentDb);
                    bandHasData = true;  // Mark that we processed at least one bin
                }

                if (bandHasData) {
                    // Normalize the MAXIMUM dB value found in the band to [0, 1]
                    // Using -60dB to 0dB range for normalization
                    newLevels[b] = clamp((maxDbInBand + 60.0f) / 60.0f, 0.0f, 1.0f);
                } else {
                    newLevels[b] = 0.f;  // Band had no valid bins
                }
            }
        }

        // --- Update Band Levels & Peaks (Unchanged Logic, uses newLevels) ---
        float deltaTime = (float)hopSize / sampleRate;  // Still based on hop rate
        float fallDecay = std::exp(-deltaTime / params[FALL_DELAY_PARAM].getValue());
        float peakDecay = std::exp(-deltaTime / params[PEAK_FALL_DELAY_PARAM].getValue());

        for (size_t b = 0; b < numBands; b++) {
            // Using the temporary newLevels vector calculated based on peak dB
            float newLevel = newLevels[b];

            // Update main level (fast attack, slow decay)
            if (newLevel >= bandLevels[b]) {
                bandLevels[b] = newLevel;
            } else {
                bandLevels[b] *= fallDecay;
            }

            // Update peak level (fast attack, slower decay)
            if (newLevel >= bandPeaks[b]) {
                bandPeaks[b] = newLevel;
            } else {
                bandPeaks[b] *= peakDecay;
            }
            // Clamp levels just in case (floating point inaccuracies)
            bandLevels[b] = clamp(bandLevels[b], 0.f, 1.f);
            bandPeaks[b] = clamp(bandPeaks[b], 0.f, 1.f);
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