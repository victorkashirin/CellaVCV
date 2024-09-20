#include "filter/ripples.hpp"
#include "plugin.hpp"

struct TwinPings : Module {
    enum ParamIds {
        RES_PARAM,
        FREQ_PARAM,
        FM_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        RES_INPUT,
        FREQ_INPUT,
        FM_INPUT,
        IN_INPUT,
        GAIN_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        BP2_OUTPUT,
        LP2_OUTPUT,
        LP4_OUTPUT,
        LP3_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds {
        NUM_LIGHTS
    };

    ripples::RipplesEngine engines[16];

    TwinPings() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(RES_PARAM, 0.f, 1.f, 0.f, "Resonance", "%", 0, 100);
        configParam(FREQ_PARAM, std::log2(ripples::kFreqKnobMin), std::log2(ripples::kFreqKnobMax), std::log2(ripples::kFreqKnobMax), "Frequency", " Hz", 2.f);
        configParam(FM_PARAM, -1.f, 1.f, 0.f, "Frequency modulation", "%", 0, 100);

        configInput(RES_INPUT, "Resonance");
        configInput(FREQ_INPUT, "Frequency");
        configInput(FM_INPUT, "FM");
        configInput(IN_INPUT, "Audio");
        configInput(GAIN_INPUT, "Gain");

        configOutput(BP2_OUTPUT, "Band-pass 2-pole (12 dB/oct)");
        configOutput(LP2_OUTPUT, "Low-pass 2-pole (12 dB/oct)");
        configOutput(LP4_OUTPUT, "Low-pass 4-pole (24 dB/oct)");
        configOutput(LP3_OUTPUT, "Low-pass 3-pole (12 dB/oct)");

        configBypass(IN_INPUT, BP2_OUTPUT);
        configBypass(IN_INPUT, LP2_OUTPUT);
        configBypass(IN_INPUT, LP4_OUTPUT);
        configBypass(IN_INPUT, LP3_OUTPUT);

        onSampleRateChange();
    }

    void onReset() override {
        onSampleRateChange();
    }

    void onSampleRateChange() override {
        // TODO In Rack v2, replace with args.sampleRate
        for (int c = 0; c < 16; c++) {
            engines[c].setSampleRate(APP->engine->getSampleRate());
        }
    }

    void process(const ProcessArgs& args) override {
        int channels = std::max(inputs[IN_INPUT].getChannels(), 1);

        // Reuse the same frame object for multiple engines because the params aren't touched.
        ripples::RipplesEngine::Frame frame;
        frame.res_knob = params[RES_PARAM].getValue();
        frame.freq_knob = rescale(params[FREQ_PARAM].getValue(), std::log2(ripples::kFreqKnobMin), std::log2(ripples::kFreqKnobMax), 0.f, 1.f);
        frame.fm_knob = params[FM_PARAM].getValue();
        frame.gain_cv_present = inputs[GAIN_INPUT].isConnected();

        for (int c = 0; c < channels; c++) {
            frame.res_cv = inputs[RES_INPUT].getPolyVoltage(c);
            frame.freq_cv = inputs[FREQ_INPUT].getPolyVoltage(c);
            frame.fm_cv = inputs[FM_INPUT].getPolyVoltage(c);
            frame.input = inputs[IN_INPUT].getVoltage(c);
            frame.gain_cv = inputs[GAIN_INPUT].getPolyVoltage(c);

            engines[c].process(frame);

            outputs[BP2_OUTPUT].setVoltage(frame.bp2, c);
            outputs[LP2_OUTPUT].setVoltage(frame.lp2, c);
            outputs[LP3_OUTPUT].setVoltage(frame.lp3, c);
            outputs[LP4_OUTPUT].setVoltage(frame.lp4, c);
        }

        outputs[BP2_OUTPUT].setChannels(channels);
        outputs[LP2_OUTPUT].setChannels(channels);
        outputs[LP3_OUTPUT].setChannels(channels);
        outputs[LP4_OUTPUT].setChannels(channels);
    }
};

struct TwinPingsWidget : ModuleWidget {
    TwinPingsWidget(TwinPings* module) {
        setModule(module);
        setPanel(Svg::load(asset::plugin(pluginInstance, "res/TwinPings2.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        addParam(createParamCentered<Rogan2PSRed>(mm2px(Vec(8.872, 20.877)), module, TwinPings::RES_PARAM));
        addParam(createParamCentered<Rogan3PSWhite>(mm2px(Vec(20.307, 42.468)), module, TwinPings::FREQ_PARAM));
        addParam(createParamCentered<Rogan2PSGreen>(mm2px(Vec(31.732 + 0.1, 64.059)), module, TwinPings::FM_PARAM));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.227, 86.909)), module, TwinPings::RES_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.297, 86.909)), module, TwinPings::FREQ_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(32.367, 86.909)), module, TwinPings::FM_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.227, 98.979)), module, TwinPings::IN_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.227, 111.05)), module, TwinPings::GAIN_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(20.297, 98.979)), module, TwinPings::BP2_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(32.367, 98.979)), module, TwinPings::LP2_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(20.297, 111.05)), module, TwinPings::LP4_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(32.367, 111.05)), module, TwinPings::LP3_OUTPUT));
    }
};

Model* modelTwinPings = createModel<TwinPings, TwinPingsWidget>("TwinPings");