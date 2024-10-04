#include "plugin.hpp"

struct Resonator : Module {
    enum ParamIds {
        FREQ1_PARAM,
        DECAY1_PARAM,
        AMP1_PARAM,
        FREQ2_PARAM,
        DECAY2_PARAM,
        AMP2_PARAM,
        FREQ3_PARAM,
        DECAY3_PARAM,
        AMP3_PARAM,
        FREQ4_PARAM,
        DECAY4_PARAM,
        AMP4_PARAM,
        RES_PARAM,
        FM_AMOUNT_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        IN_INPUT,
        FREQ_CV_INPUT,
        RES_CV_INPUT,
        FM_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        SUM_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds {
        NUM_LIGHTS
    };

    // Internal state for the resonator
    float phase = 0.f;
    std::vector<std::vector<float>> delayBuffers;
    std::vector<int> delayIndices;
    std::vector<dsp::SlewLimiter> freqSlewLimiters;
    float sampleRate = 44100.f;

    Resonator() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(FREQ1_PARAM, -54.f, 54.f, 0.f, "Frequency 1", " Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
        configParam(DECAY1_PARAM, 0.f, 1.f, 0.5f, "Decay 1");
        configParam(AMP1_PARAM, 0.f, 1.f, 0.5f, "Amplitude 1");
        configParam(FREQ2_PARAM, -54.f, 54.f, 0.f, "Frequency 2", " Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
        configParam(DECAY2_PARAM, 0.f, 1.f, 0.5f, "Decay 2");
        configParam(AMP2_PARAM, 0.f, 1.f, 0.5f, "Amplitude 2");
        configParam(FREQ3_PARAM, -54.f, 54.f, 0.f, "Frequency 3", " Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
        configParam(DECAY3_PARAM, 0.f, 1.f, 0.5f, "Decay 3");
        configParam(AMP3_PARAM, 0.f, 1.f, 0.5f, "Amplitude 3");
        configParam(FREQ4_PARAM, -54.f, 54.f, 0.f, "Frequency 4", " Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
        configParam(DECAY4_PARAM, 0.f, 1.f, 0.5f, "Decay 4");
        configParam(AMP4_PARAM, 0.f, 1.f, 0.5f, "Amplitude 4");
        configParam(RES_PARAM, 0.f, 1.f, 0.5f, "Resonance");
        configParam(FM_AMOUNT_PARAM, 0.f, 1.f, 0.0f, "FM Amount");

        configInput(IN_INPUT, "Input");
        configInput(FREQ_CV_INPUT, "Frequency CV");
        configInput(RES_CV_INPUT, "Resonance CV");
        configInput(FM_INPUT, "FM Input");

        configOutput(SUM_OUTPUT, "Sum Output");

        onSampleRateChange();
    }

    void onSampleRateChange() override {
        sampleRate = APP->engine->getSampleRate();
        delayBuffers.resize(4);
        delayIndices.resize(4, 0);
        freqSlewLimiters.resize(4);
        for (auto& buffer : delayBuffers) {
            buffer.resize((int)sampleRate);
            std::fill(buffer.begin(), buffer.end(), 0.f);
        }
        for (auto& slewLimiter : freqSlewLimiters) {
            slewLimiter.setRiseFall(sampleRate / 10.f, sampleRate / 10.f);  // Adjust the slew rate as needed
        }
    }

    void process(const ProcessArgs& args) override {
        float input = inputs[IN_INPUT].getVoltage();
        float res = params[RES_PARAM].getValue() + inputs[RES_CV_INPUT].getVoltage();
        float fmParam = params[FM_AMOUNT_PARAM].getValue();
        float fmInput = inputs[FM_INPUT].isConnected() ? inputs[FM_INPUT].getVoltage() * fmParam : 0.f;

        float sumOutput = 0.f;
        float totalAmp = 0.f;

        // Process each resonator
        for (int i = 0; i < 4; i++) {
            float rawParam = params[FREQ1_PARAM + i * 3].getValue() / 12.f;
            float freq = dsp::FREQ_C4 * dsp::exp2_taylor5(rawParam);
            freq += dsp::FREQ_C4 * fmInput * fmParam;
            // float rawFreq = params[FREQ1_PARAM + i * 3].getValue() + inputs[FREQ_CV_INPUT].getVoltage() + fmInput;
            float filteredFreq = freqSlewLimiters[i].process(args.sampleTime, freq);
            float decay = params[DECAY1_PARAM + i * 3].getValue();
            float amp = params[AMP1_PARAM + i * 3].getValue();

            // Calculate delay length for the resonator
            float delayLength = sampleRate / filteredFreq;
            delayLength = clamp(delayLength, 1.f, (float)delayBuffers[i].size());

            // Karplus-Strong string synthesis
            float delayedSample = delayBuffers[i][delayIndices[i]];
            float newSample = input + delayedSample * res * decay;
            delayBuffers[i][delayIndices[i]] = newSample;

            delayIndices[i]++;
            if (delayIndices[i] >= (int)delayLength) {
                delayIndices[i] = 0;
            }

            // Sum the resonated signal
            sumOutput += newSample * amp;
            totalAmp += amp;
        }

        // Normalize the output based on total amplitude
        if (totalAmp > 0.f) {
            sumOutput /= totalAmp;
        }

        // Output the summed signal
        outputs[SUM_OUTPUT].setVoltage(sumOutput);
    }
};

struct ResonatorWidget : ModuleWidget {
    ResonatorWidget(Resonator* module) {
        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Resonator.svg")));

        // Layout controls horizontally within 360px width and 380px height
        float xOffset = 30;
        float yOffset = 30;
        float xSpacing = 90;
        float ySpacing = 50;

        for (int i = 0; i < 4; i++) {
            addParam(createParamCentered<RoundBlackKnob>(Vec(xOffset + i * xSpacing, yOffset), module, Resonator::FREQ1_PARAM + i * 3));
            addParam(createParamCentered<RoundBlackKnob>(Vec(xOffset + i * xSpacing, yOffset + ySpacing), module, Resonator::DECAY1_PARAM + i * 3));
            addParam(createParamCentered<RoundBlackKnob>(Vec(xOffset + i * xSpacing, yOffset + 2 * ySpacing), module, Resonator::AMP1_PARAM + i * 3));
        }

        addParam(createParamCentered<RoundBlackKnob>(Vec(30, 230), module, Resonator::RES_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(90, 230), module, Resonator::FM_AMOUNT_PARAM));

        addInput(createInputCentered<PJ301MPort>(Vec(30, 280), module, Resonator::IN_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(90, 280), module, Resonator::FREQ_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(150, 280), module, Resonator::RES_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(210, 280), module, Resonator::FM_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(Vec(120, 330), module, Resonator::SUM_OUTPUT));

        // Labels
        addChild(createWidget<ScrewSilver>(Vec(15, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 30, 0)));
        addChild(createWidget<ScrewSilver>(Vec(15, 365)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 30, 365)));
    }
};

Model* modelResonator = createModel<Resonator, ResonatorWidget>("Resonator");