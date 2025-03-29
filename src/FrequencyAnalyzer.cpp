#include <ffft/FFTReal.h>  // Include the FFTReal library header

#include "plugin.hpp"

struct VFDFreqAnalyzer : Module {
    enum ParamIds {
        NUM_BANDS_PARAM,
        FALL_DELAY_PARAM,
        PEAK_FALL_DELAY_PARAM,
        GAIN_PARAM,
        RESPONSIVENESS_PARAM,  // Now selects discrete window sizes
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

    std::vector<float> sampleBuffer;
    size_t bufferIndex = 0;
    size_t windowSize = 1024;
    std::vector<float> fftInput;
    std::vector<float> fftOutput;
    ffft::FFTReal<float>* fft;
    std::vector<float> bandLevels;
    std::vector<float> bandPeaks;
    float sampleRate = 44100.f;
    bool logarithmicBins = false;
    dsp::SchmittTrigger logBinTrigger;

    VFDFreqAnalyzer() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(NUM_BANDS_PARAM, 1.f, 25.f, 16.f, "Number of Bands");
        configParam(FALL_DELAY_PARAM, 0.1f, 2.f, 0.5f, "Fall Delay", "s");
        configParam(PEAK_FALL_DELAY_PARAM, 0.1f, 2.f, 1.f, "Peak Fall Delay", "s");
        configParam(GAIN_PARAM, 0.f, 2.f, 1.f, "Gain");
        configParam(RESPONSIVENESS_PARAM, 0.f, 5.f, 2.f, "FFT Window Size", "", 0.f, 1.f, WINDOW_SIZES.size() - 1);
        configParam(LOG_BINS_PARAM, 0.f, 1.f, 0.f, "Bins", {"Linear", "Logarithmic"});
        configInput(AUDIO_INPUT, "Audio");

        // Initialize with default window size
        windowSize = WINDOW_SIZES[2];  // Default to 1024
        sampleBuffer.resize(windowSize);
        fftInput.resize(windowSize);
        fftOutput.resize(windowSize);
        fft = new ffft::FFTReal<float>(windowSize);
        bandLevels.resize(16, 0.f);
        bandPeaks.resize(16, 0.f);
    }

    ~VFDFreqAnalyzer() {
        if (fft) {
            delete fft;
            fft = nullptr;
        }
    }

    void process(const ProcessArgs& args) override {
        sampleRate = args.sampleRate;

        // Handle logarithmic bins toggle
        // if (logBinTrigger.process(params[LOG_BINS_PARAM].getValue() > 0.f)) {
        //     logarithmicBins ^= true;
        // }
        logarithmicBins = (bool)params[LOG_BINS_PARAM].getValue();
        size_t newBands = static_cast<size_t>(std::max(1, (int)params[NUM_BANDS_PARAM].getValue()));
        if (bandLevels.size() != newBands) {
            bandLevels.resize(newBands, 0.f);
            bandPeaks.resize(newBands, 0.f);
        }

        // Get discrete window size selection
        size_t newWindowSize = WINDOW_SIZES[static_cast<int>(params[RESPONSIVENESS_PARAM].getValue())];
        if (windowSize != newWindowSize) {
            windowSize = newWindowSize;
            sampleBuffer.resize(windowSize);
            fftInput.resize(windowSize);
            fftOutput.resize(windowSize);
            bufferIndex = 0;
            if (fft) delete fft;
            fft = new ffft::FFTReal<float>(windowSize);
        }

        if (inputs[AUDIO_INPUT].isConnected()) {
            float gain = params[GAIN_PARAM].getValue();
            int channels = inputs[AUDIO_INPUT].getChannels();
            for (int c = 0; c < channels; c++) {
                sampleBuffer[bufferIndex] += inputs[AUDIO_INPUT].getVoltage(c) * gain / 10.f;
            }
            bufferIndex++;

            if (bufferIndex >= windowSize) {
                processFFT();
                bufferIndex = 0;
                std::fill(sampleBuffer.begin(), sampleBuffer.end(), 0.f);
            }
        }
    }

    void processFFT() {
        // Apply window function and remove DC offset
        float sum = 0.0f;
        for (size_t i = 0; i < windowSize; i++) {
            sum += sampleBuffer[i];
        }
        float dcOffset = sum / windowSize;

        for (size_t i = 0; i < windowSize; i++) {
            // Apply Hann window and remove DC offset
            float window = 0.5f * (1.f - std::cos(2.f * M_PI * i / (windowSize - 1)));
            fftInput[i] = (sampleBuffer[i] - dcOffset) * window;
        }

        // Perform FFT using FFTReal
        fft->do_fft(fftOutput.data(), fftInput.data());

        const size_t N = windowSize;
        const size_t numBins = N / 2 + 1;  // Bins 0 to N/2
        const float binWidth = sampleRate / N;

        // Limit to human audible range (20Hz-20kHz)
        const size_t minBin = std::ceil(20.0f / binWidth);  // 20Hz
        const size_t maxBin = std::min<size_t>(
            std::floor(20000.0f / binWidth),
            numBins - 1);  // 20kHz or Nyquist

        std::vector<float> magnitudes(numBins, 0.f);
        const size_t N_half = N / 2;

        // Compute magnitudes from FFT output
        magnitudes[0] = fftOutput[0] * fftOutput[0];
        magnitudes[N_half] = fftOutput[N_half] * fftOutput[N_half];

        for (size_t k = 1; k < N_half; k++) {
            float re = fftOutput[k];
            float im = fftOutput[N_half + k];
            // magnitudes[k] = re * re + im * im;
            magnitudes[k] = (re * re + im * im) / (windowSize * windowSize);
        }

        size_t numBands = bandLevels.size();
        std::vector<float> newLevels(numBands, 0.f);

        if (logarithmicBins) {
            const float minFreq = 20.0f;
            const float maxFreq = std::min(20000.0f, sampleRate / 2.0f);
            const float freqRatio = maxFreq / minFreq;

            for (size_t b = 0; b < numBands; b++) {
                // Calculate frequency bounds using logarithmic spacing
                float relStart = static_cast<float>(b) / numBands;
                float relEnd = static_cast<float>(b + 1) / numBands;

                float bandStartFreq = minFreq * std::pow(freqRatio, relStart);
                float bandEndFreq = minFreq * std::pow(freqRatio, relEnd);

                // Convert to bin indices
                size_t startBin = static_cast<size_t>(bandStartFreq / binWidth);
                size_t endBin = static_cast<size_t>(bandEndFreq / binWidth);

                // Clamp to audible range and ensure valid bin indices
                startBin = (size_t)clamp((float)startBin, minBin, maxBin);
                endBin = (size_t)clamp((float)endBin, minBin, maxBin);
                endBin = std::max(endBin, startBin + 1);  // Ensure at least 1 bin width

                // DEBUG("Band %zu: %.1fHz - %.1fHz (bins %zu-%zu)",
                //       b, bandStartFreq, bandEndFreq, startBin, endBin);

                // Sum magnitudes in this band
                float sum = 0.0f;
                size_t count = 0;
                for (size_t i = startBin; i < endBin; i++) {
                    if (i >= 1 && i < numBins) {  // Skip DC and Nyquist if needed
                        sum += magnitudes[i];
                        count++;
                    }
                }

                if (count > 0) {
                    sum /= count;
                    float dB = 10.0f * std::log10(sum + 1e-12f);
                    newLevels[b] = clamp((dB + 60.0f) / 60.0f, 0.0f, 1.0f);
                } else {
                    newLevels[b] = 0.f;
                }
            }
        } else {
            const size_t audibleBins = maxBin - minBin + 1;
            size_t binsPerBand = std::max<size_t>(1, audibleBins / numBands);

            for (size_t b = 0; b < numBands; b++) {
                size_t start = minBin + b * binsPerBand;
                size_t end = (b == numBands - 1) ? maxBin : std::min(start + binsPerBand, maxBin);
                float sum = 0.f;
                for (size_t i = start; i < end; i++) {
                    sum += magnitudes[i];
                }
                sum /= (end - start);

                float dB = 10.f * std::log10(sum + 1e-12f);
                newLevels[b] = clamp((dB + 60.f) / 60.f, 0.f, 1.f);
            }
        }

        float deltaTime = windowSize / sampleRate;
        float fallDecay = std::exp(-deltaTime / params[FALL_DELAY_PARAM].getValue());
        float peakDecay = std::exp(-deltaTime / params[PEAK_FALL_DELAY_PARAM].getValue());

        for (size_t b = 0; b < bandLevels.size(); b++) {
            float newLevel = newLevels[b];

            if (newLevel > bandLevels[b]) {
                bandLevels[b] = newLevel;
            } else {
                bandLevels[b] *= fallDecay;
                bandLevels[b] = std::max(bandLevels[b], newLevel);
            }

            if (newLevel > bandPeaks[b]) {
                bandPeaks[b] = newLevel;
            } else {
                bandPeaks[b] *= peakDecay;
                bandPeaks[b] = std::max(bandPeaks[b], newLevel);
            }
        }
    }
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