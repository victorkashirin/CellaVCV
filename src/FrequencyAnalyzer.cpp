#include "plugin.hpp"

struct VFDFreqAnalyzer : Module {
    enum ParamIds {
        NUM_BANDS_PARAM,
        FALL_DELAY_PARAM,
        PEAK_FALL_DELAY_PARAM,
        GAIN_PARAM,
        RESPONSIVENESS_PARAM,
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

    std::vector<float> sampleBuffer;
    size_t bufferIndex = 0;
    size_t windowSize = 1024;
    std::vector<std::complex<float>> fftBuffer;
    std::vector<float> bandLevels;
    std::vector<float> bandPeaks;
    float sampleRate = 44100.f;

    VFDFreqAnalyzer() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(NUM_BANDS_PARAM, 1.f, 64.f, 16.f, "Number of Bands");
        configParam(FALL_DELAY_PARAM, 0.1f, 2.f, 0.5f, "Fall Delay", "s");
        configParam(PEAK_FALL_DELAY_PARAM, 0.1f, 2.f, 1.f, "Peak Fall Delay", "s");
        configParam(GAIN_PARAM, 0.f, 2.f, 1.f, "Gain");
        configParam(RESPONSIVENESS_PARAM, 0.f, 3.f, 1.f, "Responsiveness", "", 0.5f, 256.f);
        configInput(AUDIO_INPUT, "Audio");
        sampleBuffer.resize(windowSize);
        fftBuffer.resize(windowSize);
        bandLevels.resize(16, 0.f);
        bandPeaks.resize(16, 0.f);
    }

    void process(const ProcessArgs& args) override {
        sampleRate = args.sampleRate;
        size_t newBands = static_cast<size_t>(std::max(1, (int)params[NUM_BANDS_PARAM].getValue()));
        if (bandLevels.size() != newBands) {
            bandLevels.resize(newBands, 0.f);
            bandPeaks.resize(newBands, 0.f);
        }

        int resp = static_cast<int>(params[RESPONSIVENESS_PARAM].getValue());
        windowSize = 256 * (1 << resp);
        if (sampleBuffer.size() != windowSize) {
            sampleBuffer.resize(windowSize);
            fftBuffer.resize(windowSize);
            bufferIndex = 0;
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
        for (size_t i = 0; i < windowSize; i++) {
            float window = 0.5f * (1.f - std::cos(2.f * M_PI * i / (windowSize - 1)));
            fftBuffer[i] = std::complex<float>(sampleBuffer[i] * window, 0.f);
        }

        fft(fftBuffer);

        size_t numBins = windowSize / 2;
        std::vector<float> magnitudes(numBins);
        for (size_t i = 0; i < numBins; i++) {
            magnitudes[i] = std::norm(fftBuffer[i]);
        }

        size_t numBands = bandLevels.size();
        size_t binsPerBand = std::max<size_t>(1, numBins / numBands);
        std::vector<float> newLevels(numBands, 0.f);

        for (size_t b = 0; b < numBands; b++) {
            size_t start = b * binsPerBand;
            size_t end = (b == numBands - 1) ? numBins : (b + 1) * binsPerBand;
            float sum = 0.f;
            for (size_t i = start; i < end; i++) {
                sum += magnitudes[i];
            }
            sum /= (end - start);

            float dB = 10.f * std::log10(sum + 1e-12f);
            newLevels[b] = clamp((dB + 60.f) / 60.f, 0.f, 1.f);
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

    void fft(std::vector<std::complex<float>>& x) {
        const size_t N = x.size();
        if (N <= 1) return;

        std::vector<std::complex<float>> even(N / 2);
        std::vector<std::complex<float>> odd(N / 2);
        for (size_t i = 0; i < N / 2; i++) {
            even[i] = x[2 * i];
            odd[i] = x[2 * i + 1];
        }

        fft(even);
        fft(odd);

        for (size_t k = 0; k < N / 2; k++) {
            float angle = -2.f * static_cast<float>(M_PI) * k / N;
            std::complex<float> t = std::polar<float>(1.f, angle) * odd[k];
            x[k] = even[k] + t;
            x[k + N / 2] = even[k] - t;
        }
    }
};

struct VFDDisplay : Widget {
    VFDFreqAnalyzer* module;
    float segmentSpacing = 0.5f;

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1 || !module) return;

        nvgSave(args.vg);
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(args.vg, nvgRGB(0x10, 0x10, 0x10));
        nvgFill(args.vg);

        size_t numBands = module->bandLevels.size();
        if (numBands < 1) {
            nvgRestore(args.vg);
            return;
        }

        float bandWidth = box.size.x / numBands;
        float segHeight = box.size.y / 25.f;

        for (size_t b = 0; b < numBands; b++) {
            float level = module->bandLevels[b];
            float peak = module->bandPeaks[b];
            float x = b * bandWidth;

            for (int s = 0; s < 25; s++) {
                float yPos = box.size.y - (s + 1) * segHeight;
                float alpha = (s < 25 * level) ? 0.3f + 0.7f * (s / 24.f) : 0.f;
                nvgFillColor(args.vg, nvgRGBA(0x90, 0xFF, 0x30, (int)(alpha * 255)));
                nvgBeginPath(args.vg);
                nvgRect(args.vg, x + 1, yPos, bandWidth - 2, segHeight - segmentSpacing);
                nvgFill(args.vg);
            }

            if (peak > 0.f) {
                float peakY = box.size.y - (peak * 24.f + 1) * segHeight;
                nvgFillColor(args.vg, nvgRGB(0xFF, 0x30, 0x30));
                nvgBeginPath(args.vg);
                nvgRect(args.vg, x + 1, peakY, bandWidth - 2, 2.f);
                nvgFill(args.vg);
            }
        }

        nvgRestore(args.vg);
    }
};

struct VFDFreqAnalyzerWidget : ModuleWidget {
    VFDFreqAnalyzerWidget(VFDFreqAnalyzer* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/VFDFreqAnalyzer.svg")));

        VFDDisplay* display = new VFDDisplay();
        display->module = module;
        display->box.pos = mm2px(Vec(5.0, 15.0));
        display->box.size = mm2px(Vec(120.0, 80.0));
        addChild(display);

        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(20, 100)), module, VFDFreqAnalyzer::NUM_BANDS_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40, 100)), module, VFDFreqAnalyzer::FALL_DELAY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(60, 100)), module, VFDFreqAnalyzer::PEAK_FALL_DELAY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(80, 100)), module, VFDFreqAnalyzer::GAIN_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(100, 100)), module, VFDFreqAnalyzer::RESPONSIVENESS_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(110, 120)), module, VFDFreqAnalyzer::AUDIO_INPUT));
    }
};

Model* modelVFDFreqAnalyzer = createModel<VFDFreqAnalyzer, VFDFreqAnalyzerWidget>("VFDFreqAnalyzer");