#include "components.hpp"
#include "filter/ripples.hpp"
#include "plugin.hpp"

struct TwinPeaks : Module {
    enum ParamIds {
        FREQ_A_PARAM,
        FREQ_B_PARAM,
        RES_PARAM,
        RES_CV_PARAM,
        TRACK_A_PARAM,
        TRACK_B_PARAM,
        FM_GLOBAL_A_PARAM,
        FM_GLOBAL_B_PARAM,
        FM_CV_A_PARAM,
        FM_CV_B_PARAM,

        XFM_B_PARAM,
        TYPE_SWITCH,
        CURVE_B_PARAM,
        CURVE_B_CV_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        RES_INPUT,
        FREQ_A_INPUT,
        FREQ_B_INPUT,
        FM_CV_A_INPUT,
        FM_CV_B_INPUT,
        IN_INPUT,
        CURVE_B_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        OUT_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds {
        NUM_LIGHTS
    };

    ripples::RipplesEngine enginesA[16];
    ripples::RipplesEngine enginesB[16];

    TwinPeaks() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

        configParam(FREQ_A_PARAM, std::log2(ripples::kFreqKnobMin), std::log2(ripples::kFreqKnobMax), std::log2(ripples::kFreqKnobMin), "Frequency A", " Hz", 2.f);
        configParam(FREQ_B_PARAM, std::log2(ripples::kFreqKnobMin), std::log2(ripples::kFreqKnobMax), std::log2(ripples::kFreqKnobMax), "Frequency B", " Hz", 2.f);

        configParam(FM_GLOBAL_A_PARAM, -1.f, 1.f, 1.f, "Frequency A mod", "%", 0, 100);
        configParam(RES_PARAM, 0.f, 1.f, 0.6f, "Resonance", "%", 0, 100);
        configParam(CURVE_B_PARAM, 0.f, 1.f, 1.f, "Low Pass<>Twin Peak", "%", 0, 100);
        configParam(FM_GLOBAL_B_PARAM, -1.f, 1.f, 1.f, "Frequency B mod", "%", 0, 100);

        configParam(FM_CV_A_PARAM, -1.f, 1.f, 0.f, "Frequency A mod CV", "%", 0, 100);
        configParam(RES_CV_PARAM, -1.f, 1.f, 0.f, "Resonance mod CV", "%", 0, 100);
        configParam(CURVE_B_CV_PARAM, -1.f, 1.f, 0.f, "LP<>TWP mod CV", "%", 0, 100);
        configParam(FM_CV_B_PARAM, -1.f, 1.f, 0.f, "Frequency B mod CV", "%", 0, 100);

        configParam(TRACK_A_PARAM, -1.f, 1.f, 0.f, "Track A", "%", 0, 100);
        configParam(XFM_B_PARAM, -1.f, 1.f, 0.f, "B->A FM", "%", 0, 100);
        configSwitch(TYPE_SWITCH, -1.f, 1.f, 0.f, "Filter type", {"12dB", "18dB", "24dB"});
        configParam(TRACK_B_PARAM, -1.f, 1.f, 0.f, "Track B", "%", 0, 100);

        configInput(RES_INPUT, "Resonance");
        configInput(CURVE_B_INPUT, "LP<>TWP");
        configInput(FREQ_A_INPUT, "Frequency A");
        configInput(FREQ_B_INPUT, "Frequency B");
        configInput(FM_CV_A_INPUT, "FM A");
        configInput(FM_CV_B_INPUT, "FM B");
        configInput(IN_INPUT, "Audio");

        configOutput(OUT_OUTPUT, "Audio");
        configBypass(IN_INPUT, OUT_OUTPUT);
        onSampleRateChange();
    }

    void onReset() override {
        onSampleRateChange();
    }

    void onSampleRateChange() override {
        // TODO In Rack v2, replace with args.sampleRate
        for (int c = 0; c < 16; c++) {
            enginesA[c].setSampleRate(APP->engine->getSampleRate());
            enginesB[c].setSampleRate(APP->engine->getSampleRate());
        }
    }

    void process(const ProcessArgs& args) override {
        int channels = std::max(inputs[IN_INPUT].getChannels(), 1);

        // Filter A Frame
        ripples::RipplesEngine::Frame frameA;
        frameA.res_knob = params[RES_PARAM].getValue();
        frameA.freq_knob = rescale(params[FREQ_A_PARAM].getValue(), std::log2(ripples::kFreqKnobMin), std::log2(ripples::kFreqKnobMax), 0.f, 1.f);
        frameA.fm_knob = params[FM_CV_A_PARAM].getValue();
        frameA.fm_global_knob = params[FM_GLOBAL_A_PARAM].getValue();
        frameA.track_knob = params[TRACK_A_PARAM].getValue();
        frameA.xfm_knob = params[XFM_B_PARAM].getValue();
        frameA.mode = (int)params[TYPE_SWITCH].getValue() + 2.f;

        // Filter B Frame
        ripples::RipplesEngine::Frame frameB;
        frameB.res_knob = params[RES_PARAM].getValue();
        frameB.freq_knob = rescale(params[FREQ_B_PARAM].getValue(), std::log2(ripples::kFreqKnobMin), std::log2(ripples::kFreqKnobMax), 0.f, 1.f);
        frameB.fm_knob = params[FM_CV_B_PARAM].getValue();
        frameB.fm_global_knob = params[FM_GLOBAL_B_PARAM].getValue();
        frameB.track_knob = params[TRACK_B_PARAM].getValue();
        frameB.mode = (int)params[TYPE_SWITCH].getValue() + 2.f;
        frameB.b_output = 0.f;

        float curve = clamp(
            params[CURVE_B_PARAM].getValue() + params[CURVE_B_CV_PARAM].getValue() * inputs[CURVE_B_INPUT].getVoltage() * 0.1f,
            0.f, 1.f);

        for (int c = 0; c < channels; c++) {
            float res_cv = inputs[RES_INPUT].getPolyVoltage(c) * params[RES_CV_PARAM].getValue();
            float input = inputs[IN_INPUT].getVoltage(c);
            frameB.res_cv = res_cv;
            frameB.freq_cv = inputs[FREQ_B_INPUT].getPolyVoltage(c);
            frameB.fm_cv = (inputs[FM_CV_B_INPUT].isConnected()) ? inputs[FM_CV_B_INPUT].getPolyVoltage(c) : inputs[FM_CV_A_INPUT].getPolyVoltage(c);
            frameB.input = input;

            enginesB[c].process(frameB);

            frameA.res_cv = res_cv;
            frameA.freq_cv = inputs[FREQ_A_INPUT].getPolyVoltage(c);
            frameA.fm_cv = inputs[FM_CV_A_INPUT].getPolyVoltage(c);
            frameA.input = input;
            frameA.b_output = frameB.output;
            enginesA[c].process(frameA);

            outputs[OUT_OUTPUT].setVoltage(clamp(frameA.output - curve * frameB.output, -12.f, 12.f), c);
        }

        outputs[OUT_OUTPUT].setChannels(channels);
    }
};

struct TwinPeaksWidget : ModuleWidget {
    TwinPeaksWidget(TwinPeaks* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/TwinPeaks.svg"), asset::plugin(pluginInstance, "res/TwinPeaks-dark.svg")));

        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        addParam(createParamCentered<RoundHugeBlackKnob>(Vec(45, 78.88), module, TwinPeaks::FREQ_A_PARAM));
        addParam(createParamCentered<RoundHugeBlackKnob>(Vec(135, 78.88), module, TwinPeaks::FREQ_B_PARAM));

        addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 153.38), module, TwinPeaks::FM_GLOBAL_A_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 153.38), module, TwinPeaks::RES_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(112.5, 153.38), module, TwinPeaks::CURVE_B_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(157.5, 153.38), module, TwinPeaks::FM_GLOBAL_B_PARAM));

        addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 203.79), module, TwinPeaks::TRACK_A_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 203.79), module, TwinPeaks::XFM_B_PARAM));
        addParam(createParamCentered<CKSSThree>(Vec(103.02, 208.09), module, TwinPeaks::TYPE_SWITCH));
        addParam(createParamCentered<RoundBlackKnob>(Vec(157.5, 203.79), module, TwinPeaks::TRACK_B_PARAM));

        addParam(createParamCentered<Trimpot>(Vec(22.5, 252.5), module, TwinPeaks::FM_CV_A_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(67.5, 252.5), module, TwinPeaks::RES_CV_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(112.5, 252.5), module, TwinPeaks::CURVE_B_CV_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(157.5, 252.5), module, TwinPeaks::FM_CV_B_PARAM));

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(22.5, 280.01), module, TwinPeaks::FM_CV_A_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(67.5, 280.01), module, TwinPeaks::RES_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(112.5, 280.01), module, TwinPeaks::CURVE_B_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(157.5, 280.01), module, TwinPeaks::FM_CV_B_INPUT));

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(22.5, 329.25), module, TwinPeaks::IN_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(67.5, 329.25), module, TwinPeaks::FREQ_A_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(112.5, 329.25), module, TwinPeaks::FREQ_B_INPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(157.5, 329.25), module, TwinPeaks::OUT_OUTPUT));
    }
};

Model* modelTwinPeaks = createModel<TwinPeaks, TwinPeaksWidget>("TwinPeaks");
