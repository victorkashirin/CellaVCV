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

  
    TwoStep() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configButton(GATE1_PARAM, "Gate 1");
        configButton(GATE2_PARAM, "Gate 2");
        configButton(GATE3_PARAM, "Gate 3");
        configSwitch(LATCH1_PARAM, 0.0f, 1.0f, 0.0f, "Latch", {"Disabled", "Enabled"});
        configSwitch(LATCH2_PARAM, 0.0f, 1.0f, 0.0f, "Latch", {"Disabled", "Enabled"});
        configSwitch(LATCH3_PARAM, 0.0f, 1.0f, 0.0f, "Latch", {"Disabled", "Enabled"});
        configParam(LOW1_PARAM, -10.0f, 10.0f, 0.0f, "Low 1", "V");
        configParam(HIGH1_PARAM, -10.0f, 10.0f, 0.0f, "High 1", "V");
        configParam(LOW2_PARAM, -10.0f, 10.0f, 0.0f, "Low 2", "V");
        configParam(HIGH2_PARAM, -10.0f, 10.0f, 0.0f, "High 2", "V");
        configParam(LOW3_PARAM, -10.0f, 10.0f, 0.0f, "Low 3", "V");
        configParam(HIGH3_PARAM, -10.0f, 10.0f, 0.0f, "High 3", "V");
        configInput(GATE1_INPUT, "Gate 1");
        configInput(GATE2_INPUT, "Gate 2");
        configInput(GATE3_INPUT, "Gate 3");
        configOutput(OUT1_OUTPUT, "Out 1");
        configOutput(OUT2_OUTPUT, "Out 2");
        configOutput(OUT3_OUTPUT, "Out 3");
    }

    

    void process(const ProcessArgs &args) override {
        // Process each step
        for (int i = 0; i < 3; i++) {
            // Get gate input, cascading from above if not connected
            bool gate = false;
            bool buttonPressed = params[GATE1_PARAM + i].getValue() > 0.0f;
            bool gateInput = inputs[GATE1_INPUT + i].isConnected() && inputs[GATE1_INPUT + i].getVoltage() >= 2.0f;

            bool lowLightOn = false;
            bool highLightOn = false;
            
            // Check if this input is connected
            if (gateInput || buttonPressed) {
                gate = true;
            }
            // If neither input nor button, cascade from above input
            else if (i > 0) {
                // Check all inputs above this one
                for (int j = i - 1; j >= 0; j--) {
                    if (inputs[GATE1_INPUT + j].isConnected()) {
                        gate = inputs[GATE1_INPUT + j].getVoltage() >= 2.0f;
                        break;
                    }
                }
            }

            // Handle latch mode
            bool latchEnabled = params[LATCH1_PARAM + i].getValue() > 0.0f;
            if (latchEnabled) {
                // Check for button press or input trigger
                if (trigger[i].process(gate)) {
                    latchedStates[i] = !latchedStates[i];
                }
                // Output based on latched state
                outputs[OUT1_OUTPUT + i].setVoltage(
                    latchedStates[i] ? 
                    params[HIGH1_PARAM + i].getValue() : 
                    params[LOW1_PARAM + i].getValue()
                );
            } else {
                // Direct mode - output high value when gate is high
                outputs[OUT1_OUTPUT + i].setVoltage(
                    gate ? 
                    params[HIGH1_PARAM + i].getValue() : 
                    params[LOW1_PARAM + i].getValue()
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
};

struct TwoStepWidget : ModuleWidget {
    TwoStepWidget(TwoStep *module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/2Step.svg"), asset::plugin(pluginInstance, "res/2Step-dark.svg")));

        float col1 = 15.f;
        float col2 = 45.f;
        float row1 = 53.4f;
        float row2 = 102.55f;
        float step = 113.35f;

        float buttonRow = 33.12f;
        float buttonCol1 = 7.25f;
        float buttonCol2 = 37.25;

        addParam(createParamCentered<Trimpot>(Vec(col1, row1), module, TwoStep::LOW1_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col2, row1), module, TwoStep::HIGH1_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col1, row1 + step), module, TwoStep::LOW2_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col2, row1 + step), module, TwoStep::HIGH2_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col1, row1 + step * 2), module, TwoStep::LOW3_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col2, row1 + step * 2), module, TwoStep::HIGH3_PARAM));

        addParam(createParamCentered<VCVButtonTiny>(Vec(buttonCol1, buttonRow), module, TwoStep::GATE1_PARAM));
        addParam(createParamCentered<VCVButtonTiny>(Vec(buttonCol1, buttonRow + step), module, TwoStep::GATE2_PARAM));
        addParam(createParamCentered<VCVButtonTiny>(Vec(buttonCol1, buttonRow + step * 2), module, TwoStep::GATE3_PARAM));

        addParam(createParamCentered<VCVSwitchTiny>(Vec(buttonCol2, buttonRow), module, TwoStep::LATCH1_PARAM));
        addParam(createParamCentered<VCVSwitchTiny>(Vec(buttonCol2, buttonRow + step), module, TwoStep::LATCH2_PARAM));
        addParam(createParamCentered<VCVSwitchTiny>(Vec(buttonCol2, buttonRow + step * 2), module, TwoStep::LATCH3_PARAM));


        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col1, row2), module, TwoStep::GATE1_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col1, row2 + step), module, TwoStep::GATE2_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col1, row2 + step * 2), module, TwoStep::GATE3_INPUT));

        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col2, row2), module, TwoStep::OUT1_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col2, row2 + step), module, TwoStep::OUT2_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col2, row2 + step * 2), module, TwoStep::OUT3_OUTPUT));

        float lightRow = 82.34f;
        addChild(createLightCentered<SmallLight<YellowLight>>(Vec(col1, lightRow), module, TwoStep::LOW1_LIGHT));
        addChild(createLightCentered<SmallLight<YellowLight>>(Vec(col2, lightRow), module, TwoStep::HIGH1_LIGHT));
        addChild(createLightCentered<SmallLight<YellowLight>>(Vec(col1, lightRow + step), module, TwoStep::LOW2_LIGHT));
        addChild(createLightCentered<SmallLight<YellowLight>>(Vec(col2, lightRow + step), module, TwoStep::HIGH2_LIGHT));
        addChild(createLightCentered<SmallLight<YellowLight>>(Vec(col1, lightRow + step * 2), module, TwoStep::LOW3_LIGHT));
        addChild(createLightCentered<SmallLight<YellowLight>>(Vec(col2, lightRow + step * 2), module, TwoStep::HIGH3_LIGHT));
        
    }
};

Model *model2Step = createModel<TwoStep, TwoStepWidget>("2Step");