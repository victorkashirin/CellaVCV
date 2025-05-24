#include "components.hpp"
#include "plugin.hpp"

struct TwoStep : Module {
    enum ParamId {
        GATE1_PARAM,
        GATE2_PARAM,
        GATE3_PARAM,
        LATCH1_PARAM,
        LATCH2_PARAM,
        LATCH3_PARAM,
        LOW1_PARAM,
        LOW2_PARAM,
        LOW3_PARAM,
        HIGH1_PARAM,
        HIGH2_PARAM,
        HIGH3_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        GATE1_INPUT,
        GATE2_INPUT,
        GATE3_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        OUT1_OUTPUT,
        OUT2_OUTPUT,
        OUT3_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        LIGHTS_LEN
    };

    dsp::SchmittTrigger trigger[3];

  
    TwoStep() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configButton(GATE1_PARAM, "Gate 1");
        configButton(GATE2_PARAM, "Gate 2");
        configButton(GATE3_PARAM, "Gate 3");
        configSwitch(LATCH1_PARAM, 0.0f, 1.0f, 0.0f, "Latch", {"Disabled", "Enabled"});
        configSwitch(LATCH2_PARAM, 0.0f, 1.0f, 0.0f, "Latch", {"Disabled", "Enabled"});
        configSwitch(LATCH3_PARAM, 0.0f, 1.0f, 0.0f, "Latch", {"Disabled", "Enabled"});
        configParam(LOW1_PARAM, -10.0f, 10.0f, 0.0f, "Low 1");
        configParam(HIGH1_PARAM, -10.0f, 10.0f, 0.0f, "High 1");
        configParam(LOW2_PARAM, -10.0f, 10.0f, 0.0f, "Low 2");
        configParam(HIGH2_PARAM, -10.0f, 10.0f, 0.0f, "High 2");
        configParam(LOW3_PARAM, -10.0f, 10.0f, 0.0f, "Low 3");
        configParam(HIGH3_PARAM, -10.0f, 10.0f, 0.0f, "High 3");
        configInput(GATE1_INPUT, "Gate 1");
        configInput(GATE2_INPUT, "Gate 2");
        configInput(GATE3_INPUT, "Gate 3");
        configOutput(OUT1_OUTPUT, "Out 1");
        configOutput(OUT2_OUTPUT, "Out 2");
        configOutput(OUT3_OUTPUT, "Out 3");
    }

    

    void process(const ProcessArgs &args) override {
        
    }
};

struct TwoStepWidget : ModuleWidget {
    TwoStepWidget(TwoStep *module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/2Step.svg"), asset::plugin(pluginInstance, "res/2Step-dark.svg")));

        float col1 = 15.f;
        float col2 = 45.f;
        float row1 = 53.4f;
        float row2 = 104.36f;
        float step = 113.35f;

        addParam(createParamCentered<Trimpot>(Vec(col1, row1), module, TwoStep::LOW1_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col2, row1), module, TwoStep::HIGH1_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col1, row1 + step), module, TwoStep::LOW2_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col2, row1 + step), module, TwoStep::HIGH2_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col1, row1 + step * 2), module, TwoStep::LOW3_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col2, row1 + step * 2), module, TwoStep::HIGH3_PARAM));

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col1, row2), module, TwoStep::GATE1_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col1, row2 + step), module, TwoStep::GATE2_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col1, row2 + step * 2), module, TwoStep::GATE3_INPUT));

        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col2, row2), module, TwoStep::OUT1_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col2, row2 + step), module, TwoStep::OUT2_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col2, row2 + step * 2), module, TwoStep::OUT3_OUTPUT));
    }
};

Model *model2Step = createModel<TwoStep, TwoStepWidget>("2Step");