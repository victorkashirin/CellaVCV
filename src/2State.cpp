#include "components.hpp"
#include "plugin.hpp"

struct TwoState : Module {
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
        LOW1_LIGHT,
        LOW2_LIGHT,
        LOW3_LIGHT,
        HIGH1_LIGHT,
        HIGH2_LIGHT,
        HIGH3_LIGHT,
        LIGHTS_LEN
    };

    dsp::SchmittTrigger trigger[3];
    bool latchedStates[3] = {false, false, false};
    int rangeIndex = 0;

    bool latchEnabledByDefault = true;
    bool cascadingEnabled = true;

    float rangeSelect[10][2] = {
        {0.f, 10.f},
        {0.f, 5.f},
        {0.f, 3.f},
        {0.f, 2.f},
        {0.f, 1.f},
        {1.f, 5.f},
        {1.f, 2.5f},
        {1.f, 1.5f},
        {1.f, 1.f},
        {1.f, 0.5f},
    };

  
    TwoState() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configButton(GATE1_PARAM, "Gate 1");
        configButton(GATE2_PARAM, "Gate 2");
        configButton(GATE3_PARAM, "Gate 3");
        configSwitch(LATCH1_PARAM, 0.0f, 1.0f, 1.0f, "Latch 1", {"Disabled", "Enabled"});
        configSwitch(LATCH2_PARAM, 0.0f, 1.0f, 1.0f, "Latch 2", {"Disabled", "Enabled"});
        configSwitch(LATCH3_PARAM, 0.0f, 1.0f, 1.0f, "Latch 3", {"Disabled", "Enabled"});
        configParam<RangeQuantity>(LOW1_PARAM, -1.0f, 1.0f, 0.0f, "Low 1", "V");
        configParam<RangeQuantity>(HIGH1_PARAM, -1.0f, 1.0f, 0.0f, "High 1", "V");
        configParam<RangeQuantity>(LOW2_PARAM, -1.0f, 1.0f, 0.0f, "Low 2", "V");
        configParam<RangeQuantity>(HIGH2_PARAM, -1.0f, 1.0f, 0.0f, "High 2", "V");
        configParam<RangeQuantity>(LOW3_PARAM, -1.0f, 1.0f, 0.0f, "Low 3", "V");
        configParam<RangeQuantity>(HIGH3_PARAM, -1.0f, 1.0f, 0.0f, "High 3", "V");
        configInput(GATE1_INPUT, "Gate 1");
        configInput(GATE2_INPUT, "Gate 2");
        configInput(GATE3_INPUT, "Gate 3");
        configOutput(OUT1_OUTPUT, "Out 1");
        configOutput(OUT2_OUTPUT, "Out 2");
        configOutput(OUT3_OUTPUT, "Out 3");
    }

    void onReset() override {
        for (int i = 0; i < 3; i++) {
            params[LATCH1_PARAM + i].setValue(latchEnabledByDefault ? 1.0f : 0.0f);
        }
    }

    struct RangeQuantity : rack::ParamQuantity {

        float getDisplayValue() override {
            if (!module) {
                return rack::ParamQuantity::getDisplayValue();
            }

            float knobValue = getValue();
            TwoState* twoStateModule = dynamic_cast<TwoState*>(module);
            if (!twoStateModule) {
                return rack::ParamQuantity::getDisplayValue();  // Fallback if cast fails
            }
            int rangeIndex = twoStateModule->rangeIndex;
            float rangeOffset = twoStateModule->rangeSelect[rangeIndex][0];
            float rangeScale = twoStateModule->rangeSelect[rangeIndex][1];
            float displayValue = (rangeOffset + knobValue) * rangeScale;
            return displayValue;
        }
    };

    float scaleValue(float value) {
        int rangeIndex = this->rangeIndex;
        float rangeOffset = this->rangeSelect[rangeIndex][0];
        float rangeScale = this->rangeSelect[rangeIndex][1];
        return (value + rangeOffset) * rangeScale;
    }

    void process(const ProcessArgs &args) override {
        for (int i = 0; i < 3; i++) {
            bool gate = false;
            bool buttonPressed = params[GATE1_PARAM + i].getValue() > 0.0f;
            bool gateInput = inputs[GATE1_INPUT + i].isConnected() && inputs[GATE1_INPUT + i].getVoltage() >= 2.0f;

            bool lowLightOn = false;
            bool highLightOn = false;
            
            if (gateInput || buttonPressed) {
                gate = true;
            }
            else if (i > 0 && cascadingEnabled) {
                for (int j = i - 1; j >= 0; j--) {
                    if (inputs[GATE1_INPUT + j].isConnected()) {
                        gate = inputs[GATE1_INPUT + j].getVoltage() >= 2.0f;
                        
                    }
                    if (params[GATE1_PARAM + j].getValue() > 0.0f) {
                        gate = true;
                        
                    }
                    if (gate) {
                        break;
                    }
                }
            }

            bool latchEnabled = params[LATCH1_PARAM + i].getValue() > 0.0f;
            if (latchEnabled) {
                if (trigger[i].process(gate)) {
                    latchedStates[i] = !latchedStates[i];
                }
                outputs[OUT1_OUTPUT + i].setVoltage(
                    latchedStates[i] ? 
                    scaleValue(params[HIGH1_PARAM + i].getValue()) : 
                    scaleValue(params[LOW1_PARAM + i].getValue())
                );
            } else {
                outputs[OUT1_OUTPUT + i].setVoltage(
                    gate ? 
                    scaleValue(params[HIGH1_PARAM + i].getValue()) : 
                    scaleValue(params[LOW1_PARAM + i].getValue())
                );
            }

            if (latchEnabled) {
                lowLightOn = latchedStates[i] == false;
                highLightOn = latchedStates[i] == true;
            } else {
                lowLightOn = gate == false;
                highLightOn = gate == true;
            }

            lights[LOW1_LIGHT + i].setBrightness(lowLightOn*0.5f);
            lights[HIGH1_LIGHT + i].setBrightness(highLightOn*0.5f);
        }
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_t* a = json_array();
			for (int c = 0; c < 3; ++c) {
				json_array_append_new(a, json_boolean(latchedStates[c]));
			}
		json_object_set_new(rootJ, "latched_states", a);
        json_object_set_new(rootJ, "rangeIndex", json_integer(rangeIndex));
        json_object_set_new(rootJ, "latchEnabledByDefault", json_boolean(latchEnabledByDefault));
        json_object_set_new(rootJ, "cascadingEnabled", json_boolean(cascadingEnabled));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* latchedStatesJ = json_object_get(rootJ, "latched_states");
        if (latchedStatesJ && json_array_size(latchedStatesJ) == 3) {
            for (int c = 0; c < 3; ++c) {
                json_t* ls = json_array_get(latchedStatesJ, c);
                if (ls && json_is_true(ls)) {
                    latchedStates[c] = true;
                }
            }
        }
        json_t* rangeIndexJ = json_object_get(rootJ, "rangeIndex");
        if (rangeIndexJ) {
            rangeIndex = json_integer_value(rangeIndexJ);
        }
        json_t* latchEnabledByDefaultJ = json_object_get(rootJ, "latchEnabledByDefault");
        if (latchEnabledByDefaultJ) {
            latchEnabledByDefault = json_is_true(latchEnabledByDefaultJ);
        }
        json_t* cascadingEnabledJ = json_object_get(rootJ, "cascadingEnabled");
        if (cascadingEnabledJ) {
            cascadingEnabled = json_is_true(cascadingEnabledJ);
        }
    }
};

struct TwoStateWidget : ModuleWidget {
    TwoStateWidget(TwoState *module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/2State.svg"), asset::plugin(pluginInstance, "res/2State-dark.svg")));

        float col1 = 15.f;
        float col2 = 45.f;
        float row1 = 53.4f;
        float row2 = 102.55f;
        float step = 113.35f;

        float buttonRow = 33.12f;
        float buttonCol1 = 7.25f;
        float buttonCol2 = 37.25;

        addParam(createParamCentered<Trimpot>(Vec(col1, row1), module, TwoState::LOW1_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col2, row1), module, TwoState::HIGH1_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col1, row1 + step), module, TwoState::LOW2_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col2, row1 + step), module, TwoState::HIGH2_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col1, row1 + step * 2), module, TwoState::LOW3_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col2, row1 + step * 2), module, TwoState::HIGH3_PARAM));

        addParam(createParamCentered<VCVButtonTiny>(Vec(buttonCol1, buttonRow), module, TwoState::GATE1_PARAM));
        addParam(createParamCentered<VCVButtonTiny>(Vec(buttonCol1, buttonRow + step), module, TwoState::GATE2_PARAM));
        addParam(createParamCentered<VCVButtonTiny>(Vec(buttonCol1, buttonRow + step * 2), module, TwoState::GATE3_PARAM));

        addParam(createParamCentered<VCVSwitchTiny>(Vec(buttonCol2, buttonRow), module, TwoState::LATCH1_PARAM));
        addParam(createParamCentered<VCVSwitchTiny>(Vec(buttonCol2, buttonRow + step), module, TwoState::LATCH2_PARAM));
        addParam(createParamCentered<VCVSwitchTiny>(Vec(buttonCol2, buttonRow + step * 2), module, TwoState::LATCH3_PARAM));


        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col1, row2), module, TwoState::GATE1_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col1, row2 + step), module, TwoState::GATE2_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col1, row2 + step * 2), module, TwoState::GATE3_INPUT));

        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col2, row2), module, TwoState::OUT1_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col2, row2 + step), module, TwoState::OUT2_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col2, row2 + step * 2), module, TwoState::OUT3_OUTPUT));

        float lightRow = 82.34f;
        addChild(createLightCentered<SmallLight<YellowLight>>(Vec(col1, lightRow), module, TwoState::LOW1_LIGHT));
        addChild(createLightCentered<SmallLight<YellowLight>>(Vec(col2, lightRow), module, TwoState::HIGH1_LIGHT));
        addChild(createLightCentered<SmallLight<YellowLight>>(Vec(col1, lightRow + step), module, TwoState::LOW2_LIGHT));
        addChild(createLightCentered<SmallLight<YellowLight>>(Vec(col2, lightRow + step), module, TwoState::HIGH2_LIGHT));
        addChild(createLightCentered<SmallLight<YellowLight>>(Vec(col1, lightRow + step * 2), module, TwoState::LOW3_LIGHT));
        addChild(createLightCentered<SmallLight<YellowLight>>(Vec(col2, lightRow + step * 2), module, TwoState::HIGH3_LIGHT));
    }

    void appendContextMenu(Menu *menu) override {
        TwoState *module = dynamic_cast<TwoState *>(this->module);
        assert(module);
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Settings"));
        menu->addChild(createIndexPtrSubmenuItem("Range",
                                                 {"+/-10V",
                                                  "+/-5V",
                                                  "+/-3V",
                                                  "+/-2V",
                                                  "+/-1V",
                                                  "0V-10V",
                                                  "0V-5V",
                                                  "0V-3V",
                                                  "0V-2V", 
                                                  "0V-1V"}, &module->rangeIndex));
        menu->addChild(createIndexPtrSubmenuItem("Default mode",
                                                 {"Gate", "Latch"}, &module->latchEnabledByDefault));
        menu->addChild(createIndexPtrSubmenuItem("Cascading",
                                                 {"Disabled", "Enabled"}, &module->cascadingEnabled));
    }
};

Model *model2State = createModel<TwoState, TwoStateWidget>("2State");