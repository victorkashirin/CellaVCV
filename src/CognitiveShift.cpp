#include <algorithm>
#include <vector>

#include "components.hpp"
#include "plugin.hpp"

// Define constants for clarity
const int NUM_STEPS = 8;
const int NUM_DAC = 4;
const int NUM_COMPLEX_INPUTS = 3;  // only DATA, XOR and LOGIC
const float GATE_VOLTAGE = 10.0f;
const float CLOCK_HIGH_THRESHOLD = 1.0f;

const std::vector<std::string> logicOperatorNames = {
    "XOR", "NAND", "XNOR", "OR", "AND", "NOR"};

// --- Module Logic ---
struct CognitiveShift : Module {
    enum ParamIds {
        WRITE_BUTTON_PARAM,
        ERASE_BUTTON_PARAM,
        CLEAR_BUTTON_PARAM,
        THRESHOLD_PARAM,
        THRESHOLD_CV_ATTN_PARAM,
        DAC_1_ATTN_PARAM,
        DAC_2_ATTN_PARAM,
        DAC_3_ATTN_PARAM,
        DAC_4_ATTN_PARAM,
        INPUT_BUTTON_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        DATA_INPUT,
        LOGIC_INPUT,
        XOR_INPUT,
        CLOCK_INPUT,
        CLEAR_INPUT,
        THRESHOLD_CV_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        DAC_1_OUTPUT,
        DAC_2_OUTPUT,
        DAC_3_OUTPUT,
        BIT_1_OUTPUT,
        BIT_2_OUTPUT,
        BIT_3_OUTPUT,
        BIT_4_OUTPUT,
        BIT_5_OUTPUT,
        BIT_6_OUTPUT,
        BIT_7_OUTPUT,
        BIT_8_OUTPUT,
        DAC_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds {
        STEP_LIGHTS,                                   // 0-7
        BUTTON_PRESS_LIGHT = STEP_LIGHTS + NUM_STEPS,  // 8
        DAC_LIGHTS,
        NUM_LIGHTS = DAC_LIGHTS + 8  // 9
    };

    // Internal state
    dsp::SchmittTrigger clockInputTrigger;
    dsp::SchmittTrigger clearTrigger;
    dsp::SchmittTrigger clearInputTrigger;
    dsp::SchmittTrigger writeTrigger;
    dsp::SchmittTrigger eraseTrigger;

    dsp::PulseGenerator pulseGens[NUM_STEPS];
    dsp::BooleanTrigger inputBoolean;
    bool bits[NUM_STEPS] = {};
    bool previousBits[NUM_STEPS] = {};
    int64_t currentClock = 0;
    int64_t previousClock = 0;
    bool editMode = false;

    CognitiveShift* connectedSourceModules[NUM_COMPLEX_INPUTS];
    int connectedSourceOutputIds[NUM_COMPLEX_INPUTS] = {-1};
    bool wasInputConnected[NUM_COMPLEX_INPUTS] = {false};

    enum OutputType {
        CLOCK_OUTPUT,
        GATE_OUTPUT,
        TRIGGER_OUTPUT
    };

    enum AllBitDACOutputType {
        BIPOLAR,
        UNIPOLAR
    };

    // Context menu options
    int outputType = OutputType::CLOCK_OUTPUT;
    int logicType = 0;
    bool inputOverridesEverything = true;
    int dacOutputType = AllBitDACOutputType::BIPOLAR;

    bool showBitLights = true;
    bool showDACLights = true;

    CognitiveShift() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configButton(WRITE_BUTTON_PARAM, "Data + (Write)");
        configButton(ERASE_BUTTON_PARAM, "Data - (Erase)");
        configButton(CLEAR_BUTTON_PARAM, "Clear register");
        configButton(INPUT_BUTTON_PARAM, "Manual input");
        configParam(THRESHOLD_PARAM, 1.f, 9.f, 1.f, "Data Input Threshold");
        configParam(THRESHOLD_CV_ATTN_PARAM, -1.f, 1.f, 0.f, "Threshold CV Attenuverter");
        configParam(DAC_1_ATTN_PARAM, -1.f, 1.f, 1.f, "DAC 1 (Bits 1-4) Level");
        configParam(DAC_2_ATTN_PARAM, -1.f, 1.f, 1.f, "DAC 2 (Bits 3-6) Level");
        configParam(DAC_3_ATTN_PARAM, -1.f, 1.f, 1.f, "DAC 3 (Bits 5-8) Level");
        configParam(DAC_4_ATTN_PARAM, -1.f, 1.f, 1.f, "DAC 4 (Bits 1-8) Level");

        configInput(CLOCK_INPUT, "Clock Trigger");
        configInput(DATA_INPUT, "Data");
        configInput(XOR_INPUT, "XOR");
        configInput(LOGIC_INPUT, "Logic");
        configInput(CLEAR_INPUT, "Clear register");
        configInput(THRESHOLD_CV_INPUT, "Threshold CV");

        configOutput(DAC_1_OUTPUT, "DAC 1 (Bits 1-4)");
        configOutput(DAC_2_OUTPUT, "DAC 2 (Bits 3-6)");
        configOutput(DAC_3_OUTPUT, "DAC 3 (Bits 5-8)");
        configOutput(BIT_1_OUTPUT, "Bit 1");
        configOutput(BIT_2_OUTPUT, "Bit 2");
        configOutput(BIT_3_OUTPUT, "Bit 3");
        configOutput(BIT_4_OUTPUT, "Bit 4");
        configOutput(BIT_5_OUTPUT, "Bit 5");
        configOutput(BIT_6_OUTPUT, "Bit 6");
        configOutput(BIT_7_OUTPUT, "Bit 7");
        configOutput(BIT_8_OUTPUT, "Bit 8");
        configOutput(DAC_OUTPUT, "DAC 4 (Bits 1-8)");

        onReset();
    }

    float calculateDACvalue(int startIndex, int numBits = 4) {
        float dac3Value = 0.0f;
        float weight = 1.0f;
        for (int i = 0; i < numBits; ++i) {
            int bitIndex = startIndex + i;
            if (bitIndex < NUM_STEPS && bits[bitIndex])
                dac3Value += weight;
            weight *= 2.0f;
        }
        return dac3Value;
    }

    void onReset() override {
        std::fill(bits, bits + NUM_STEPS, false);
        std::fill(previousBits, previousBits + NUM_STEPS, false);
        std::fill(connectedSourceModules, connectedSourceModules + NUM_COMPLEX_INPUTS, nullptr);
        std::fill(connectedSourceOutputIds, connectedSourceOutputIds + NUM_COMPLEX_INPUTS, -1);
        std::fill(wasInputConnected, wasInputConnected + NUM_COMPLEX_INPUTS, false);
        editMode = false;
    }

    void onRandomize() override {
        for (int i = 0; i < NUM_STEPS; ++i) {
            bits[i] = (random::uniform() > 0.5f);
        }
    }

    int outputIdToBitIndex(int outputId) {
        if (outputId >= BIT_1_OUTPUT && outputId <= BIT_8_OUTPUT) {
            return outputId - BIT_1_OUTPUT;
        }
        return -1;
    }

    void checkInputConnections() {
        for (int i = 0; i < NUM_COMPLEX_INPUTS; ++i) {
            if (inputs[i].isConnected()) {
                if (!wasInputConnected[i]) {
                    connectedSourceModules[i] = nullptr;
                    wasInputConnected[i] = true;
                    for (uint64_t cableId : APP->engine->getCableIds()) {
                        Cable* cable = APP->engine->getCable(cableId);
                        if (!cable) continue;
                        if (cable->inputModule == this && cable->inputId == i) {
                            Module* outputModule = cable->outputModule;
                            if (outputModule && outputModule->getModel() == this->getModel()) {
                                CognitiveShift* sourceCognitiveShift = dynamic_cast<CognitiveShift*>(outputModule);
                                connectedSourceModules[i] = sourceCognitiveShift;
                                connectedSourceOutputIds[i] = cable->outputId;
                            }
                        }
                    }
                }
            } else if (wasInputConnected[i]) {
                connectedSourceModules[i] = nullptr;
                connectedSourceOutputIds[i] = -1;
                wasInputConnected[i] = false;
            }
        }
    }

    bool getDataInput(int inputId, float threshold) {
        bool effectiveDataInputHigh = false;

        if (inputs[inputId].isConnected()) {
            if (connectedSourceModules[inputId] != nullptr) {
                // If we have a connected input, check if it's self-patched
                int sourceOutputId = connectedSourceOutputIds[inputId];
                int bitIndex = outputIdToBitIndex(sourceOutputId);
                if (bitIndex != -1) {
                    if (connectedSourceModules[inputId]->previousClock == currentClock) {
                        effectiveDataInputHigh = connectedSourceModules[inputId]->previousBits[bitIndex];
                    } else {
                        effectiveDataInputHigh = connectedSourceModules[inputId]->bits[bitIndex];
                    }
                }
            } else {
                effectiveDataInputHigh = inputs[inputId].getVoltage() >= threshold;  // Default
            }
        }
        return effectiveDataInputHigh;
    }

    bool applyLogicOperation(bool dataBit, bool logicBit, int logicType) {
        switch (logicType) {
            case 0:  // XOR
                return dataBit ^ logicBit;
            case 1:  // NAND
                return !(dataBit && logicBit);
            case 2:  // XNOR
                return !(dataBit ^ logicBit);
            case 3:  // OR
                return dataBit || logicBit;
            case 4:  // AND
                return dataBit && logicBit;
            case 5:  // NOR
                return !(dataBit || logicBit);
            default:
                return dataBit;  // Default to no operation
        }
    }

    void process(const ProcessArgs& args) override {
        if (inputBoolean.process(params[INPUT_BUTTON_PARAM].getValue()))
            editMode ^= true;

        bool editShift = false;

        // --- Immediate Reset Button Logic ---
        if (clearTrigger.process(params[CLEAR_BUTTON_PARAM].getValue()) || clearInputTrigger.process(inputs[CLEAR_INPUT].getVoltage())) {
            std::fill(bits, bits + NUM_STEPS, false);
        }

        if (editMode) {
            bool value = false;
            if (writeTrigger.process(params[WRITE_BUTTON_PARAM].getValue())) {
                editShift = true;
                value = true;
            }
            if (eraseTrigger.process(params[ERASE_BUTTON_PARAM].getValue())) {
                editShift = true;
                value = false;
            }
            if (editShift) {
                for (int i = NUM_STEPS - 1; i > 0; --i) {
                    bits[i] = bits[i - 1];
                }
                bits[0] = value;
            }
        }

        // --- Clock Input Processing ---
        bool clocked_this_frame = false;
        if (!editMode & inputs[CLOCK_INPUT].isConnected()) {
            if (clockInputTrigger.process(inputs[CLOCK_INPUT].getVoltage())) {
                clocked_this_frame = true;
            }
        }

        // --- Shift Register Logic (Only execute on clock tick) ---
        if (clocked_this_frame) {
            float thresholdCV = params[THRESHOLD_CV_ATTN_PARAM].getValue() * inputs[THRESHOLD_CV_INPUT].getVoltage();
            float threshold = clamp(params[THRESHOLD_PARAM].getValue() + thresholdCV, 1.f, 9.f);

            // 1. Read button states
            bool writeButtonPressed = params[WRITE_BUTTON_PARAM].getValue() > 0.f;
            bool eraseButtonPressed = params[ERASE_BUTTON_PARAM].getValue() > 0.f;

            // --- End: Input Reading with Self-Patch Detection ---

            // 4. Determine final dataBit based on priority: Write > Erase > Effective Data Input
            bool nextBit = false;
            if (writeButtonPressed) {
                nextBit = true;
            } else if (eraseButtonPressed) {
                nextBit = false;
            } else {
                nextBit = getDataInput(DATA_INPUT, threshold);
            }

            bool buttonPressed = writeButtonPressed || eraseButtonPressed;
            bool doXorAndLogic = (!buttonPressed) || (buttonPressed && !inputOverridesEverything);

            if (doXorAndLogic) {
                // 5. Determine final xorBit from the effective XOR input
                bool xorBit = getDataInput(XOR_INPUT, threshold);

                if (getInput(LOGIC_INPUT).isConnected()) {
                    bool logicBit = getDataInput(LOGIC_INPUT, threshold);
                    nextBit = applyLogicOperation(nextBit, logicBit, logicType);
                }

                // 6. Calculate the bit to shift in
                nextBit = nextBit ^ xorBit;
            }

            for (int i = 0; i < NUM_STEPS; i++) {
                previousBits[i] = bits[i];
            }

            // 7. Perform the shift
            for (int i = NUM_STEPS - 1; i > 0; --i) {
                bits[i] = bits[i - 1];
            }
            bits[0] = nextBit;
            previousClock = currentClock;
            currentClock = args.frame;

        }  // End of clocked_this_frame

        // --- Update Individual Bit Outputs & Lights ---
        for (int i = 0; i < NUM_STEPS; ++i) {
            float bitVoltage = 0.0f;

            if (bits[i]) {
                if (editMode) {
                    if (editShift) {
                        pulseGens[i].trigger(0.001f);
                    }
                    bitVoltage = pulseGens[i].process(args.sampleTime) ? GATE_VOLTAGE : 0.0f;
                } else if (outputType == OutputType::GATE_OUTPUT) {
                    bitVoltage = GATE_VOLTAGE;
                } else if (outputType == OutputType::TRIGGER_OUTPUT) {
                    if (clocked_this_frame) {
                        pulseGens[i].trigger(0.001f);
                    }
                    bitVoltage = pulseGens[i].process(args.sampleTime) ? GATE_VOLTAGE : 0.0f;
                } else if (outputType == OutputType::CLOCK_OUTPUT) {
                    float clockInputVoltage = inputs[CLOCK_INPUT].isConnected() ? inputs[CLOCK_INPUT].getVoltage() : 0.f;
                    bool isClockHigh = clockInputVoltage >= CLOCK_HIGH_THRESHOLD;
                    bitVoltage = isClockHigh ? GATE_VOLTAGE : 0.0f;
                }
            }

            int outputId = BIT_1_OUTPUT + i;
            if (outputId >= BIT_1_OUTPUT && outputId <= BIT_8_OUTPUT) {
                outputs[outputId].setVoltage(bitVoltage);
            }
            lights[STEP_LIGHTS + i].setBrightness(bits[i] ? 1.0f : 0.0f);
        }

        // --- Calculate and Output DAC ---
        float dac4bitScale = 10.f / 15.0f;
        float dac1raw = calculateDACvalue(0, 4) * dac4bitScale;
        float dac2raw = calculateDACvalue(2, 4) * dac4bitScale;
        float dac3raw = calculateDACvalue(4, 4) * dac4bitScale;
        float dac1final = dac1raw * params[DAC_1_ATTN_PARAM].getValue();
        float dac2final = dac2raw * params[DAC_2_ATTN_PARAM].getValue();
        float dac3final = dac3raw * params[DAC_3_ATTN_PARAM].getValue();
        outputs[DAC_1_OUTPUT].setVoltage(dac1final);
        outputs[DAC_2_OUTPUT].setVoltage(dac2final);
        outputs[DAC_3_OUTPUT].setVoltage(dac3final);

        lights[DAC_LIGHTS + 0].setBrightness(fmaxf(0.0f, dac1final / 10.f));
        lights[DAC_LIGHTS + 1].setBrightness(fmaxf(0.0f, -dac1final / 10.f));

        lights[DAC_LIGHTS + 2].setBrightness(fmaxf(0.0f, dac2final / 10.f));
        lights[DAC_LIGHTS + 3].setBrightness(fmaxf(0.0f, -dac2final / 10.f));

        lights[DAC_LIGHTS + 4].setBrightness(fmaxf(0.0f, dac3final / 10.f));
        lights[DAC_LIGHTS + 5].setBrightness(fmaxf(0.0f, -dac3final / 10.f));

        // --- Calculate and Output 8-Bit DAC ---
        float dacRawValue = calculateDACvalue(0, 8);
        float dacScaledValue = dacRawValue / 255.0f;
        if (dacOutputType == AllBitDACOutputType::BIPOLAR) {
            dacScaledValue = dacScaledValue * 10.0f - 5.f;  // Convert to bipolar range
        } else {
            dacScaledValue = dacScaledValue * 10.f;  // Convert to unipolar range
        }

        float dacAttn = params[DAC_4_ATTN_PARAM].getValue();
        float finalDacOutput = dacScaledValue * dacAttn;
        outputs[DAC_OUTPUT].setVoltage(finalDacOutput);

        float lightScale = (dacOutputType == AllBitDACOutputType::BIPOLAR) ? 5.f : 10.f;

        lights[DAC_LIGHTS + 6]
            .setBrightness(fmaxf(0.0f, finalDacOutput / lightScale));
        lights[DAC_LIGHTS + 7].setBrightness(fmaxf(0.0f, -finalDacOutput / lightScale));

        // --- Update Button Press Light ---
        bool writePressed = params[WRITE_BUTTON_PARAM].getValue() > 0.f;
        bool erasePressed = params[ERASE_BUTTON_PARAM].getValue() > 0.f;
        bool clearPressed = params[CLEAR_BUTTON_PARAM].getValue() > 0.f;
        lights[BUTTON_PRESS_LIGHT].setBrightness((writePressed || erasePressed || clearPressed || editMode) ? 1.0f : 0.0f);
    }

    // dataToJson/FromJson remain the same as they only handle the 'bits' array
    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_t* valuesJ = json_array();
        for (int i = 0; i < NUM_STEPS; i++) {
            json_array_insert_new(valuesJ, i, json_boolean(bits[i]));
        }
        json_object_set_new(rootJ, "values", valuesJ);
        json_object_set_new(rootJ, "outputType", json_integer(outputType));
        json_object_set_new(rootJ, "logicType", json_integer(logicType));
        json_object_set_new(rootJ, "inputOverridesEverything", json_boolean(inputOverridesEverything));
        json_object_set_new(rootJ, "allBitDACOutputType", json_integer(dacOutputType));
        json_object_set_new(rootJ, "showBitLights", json_boolean(showBitLights));
        json_object_set_new(rootJ, "showDACLights", json_boolean(showDACLights));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* valuesJ = json_object_get(rootJ, "values");
        if (valuesJ) {
            for (int i = 0; i < NUM_STEPS; i++) {
                json_t* valueJ = json_array_get(valuesJ, i);
                if (valueJ)
                    bits[i] = json_boolean_value(valueJ);
            }
        }
        json_t* outputTypeJ = json_object_get(rootJ, "outputType");
        if (outputTypeJ)
            outputType = json_integer_value(outputTypeJ);

        json_t* logicTypeJ = json_object_get(rootJ, "logicType");
        if (logicTypeJ)
            logicType = json_integer_value(logicTypeJ);

        json_t* inputOverridesEverythingJ = json_object_get(rootJ, "inputOverridesEverything");
        if (inputOverridesEverythingJ)
            inputOverridesEverything = json_boolean_value(inputOverridesEverythingJ);

        json_t* allBitDACOutputTypeJ = json_object_get(rootJ, "allBitDACOutputType");
        if (allBitDACOutputTypeJ)
            dacOutputType = json_integer_value(allBitDACOutputTypeJ);

        // UI

        json_t* showBitLightsJ = json_object_get(rootJ, "showBitLights");
        if (showBitLightsJ)
            showBitLights = json_boolean_value(showBitLightsJ);

        json_t* showDACLightsJ = json_object_get(rootJ, "showDACLights");
        if (showDACLightsJ)
            showDACLights = json_boolean_value(showDACLightsJ);
    }
};

struct LogicThemedPJ301MPort : ThemedPJ301MPort {
    bool isHovered = false;
    Tooltip* customTooltip = nullptr;
    Tooltip* originalTooltip = nullptr;

    ~LogicThemedPJ301MPort() {
        cleanupTooltips();
    }

    void cleanupTooltips() {
        if (customTooltip) {
            customTooltip->requestDelete();
            customTooltip = nullptr;
        }
        originalTooltip = nullptr;
    }

    void onEnter(const EnterEvent& e) override {
        ThemedPJ301MPort::onEnter(e);
        this->isHovered = true;
    }

    void onDragDrop(const DragDropEvent& e) override {
        ThemedPJ301MPort::onDragDrop(e);
        this->isHovered = true;
    }

    void onLeave(const LeaveEvent& e) override {
        ThemedPJ301MPort::onLeave(e);
        this->isHovered = false;
        cleanupTooltips();
    }

    void onDragStart(const DragStartEvent& e) override {
        ThemedPJ301MPort::onDragStart(e);
        this->isHovered = false;
        cleanupTooltips();
    }

    std::string getLogicModeName(CognitiveShift* module) {
        if (!module) return "Unknown";
        int currentLogicType = module->logicType;
        return (currentLogicType >= 0 && (size_t)currentLogicType < logicOperatorNames.size())
                   ? logicOperatorNames[currentLogicType]
                   : "Unknown";
    }

    void step() override {
        ThemedPJ301MPort::step();
        // Only modify customTooltip if we're currently hovered

        if (!isHovered || !APP->scene) return;

        CognitiveShift* csModule = dynamic_cast<CognitiveShift*>(module);
        if (!csModule) return;

        if (!originalTooltip) {
            for (Widget* w : APP->scene->children) {
                if (Tooltip* tt = dynamic_cast<Tooltip*>(w)) {
                    originalTooltip = tt;
                    break;
                }
            }
            if (!originalTooltip) return;
        }

        if (!customTooltip) {
            customTooltip = new Tooltip;
            customTooltip->setPosition(originalTooltip->getPosition());
            APP->scene->addChild(customTooltip);
        }

        std::string modeText = getLogicModeName(csModule);
        customTooltip->text = rack::string::f("Current logic mode: %s\n%s",
                                              modeText.c_str(),
                                              originalTooltip->text.c_str());

        // Hide original tooltip
        originalTooltip->hide();
    }
};

struct CognitiveShiftWidget : ModuleWidget {
    rack::widget::Widget* stepLightWidgets[NUM_STEPS];
    rack::widget::Widget* dacLightWidgets[NUM_DAC];

    CognitiveShiftWidget(CognitiveShift* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/CognitiveShift.svg"), asset::plugin(pluginInstance, "res/CognitiveShift-dark.svg")));

        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        float col1 = 22.5f;
        float col2 = 67.5f;
        float col3 = 112.5f;
        float col4 = 157.5f;

        // addParam(createParamCentered<VCVButtonMini>(Vec(90.f, 38.5f), module, CognitiveShift::CLEAR_BUTTON_PARAM));

        // Buttons and Button Press Light
        addParam(createParamCentered<VCVButton>(Vec(col1, 53.5f), module, CognitiveShift::CLEAR_BUTTON_PARAM));
        addParam(createParamCentered<VCVButtonHuge>(Vec(col2, 53.5f), module, CognitiveShift::WRITE_BUTTON_PARAM));
        addParam(createParamCentered<VCVButtonHuge>(Vec(col3, 53.5f), module, CognitiveShift::ERASE_BUTTON_PARAM));
        addParam(createParamCentered<VCVButton>(Vec(col4, 53.5f), module, CognitiveShift::INPUT_BUTTON_PARAM));
        addChild(createLightCentered<LargeFresnelLight<BlueLight>>(Vec(col4, 53.5f), module, CognitiveShift::BUTTON_PRESS_LIGHT));

        // INPUTS
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col1, 153.5f), module, CognitiveShift::CLOCK_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col2, 153.5f), module, CognitiveShift::DATA_INPUT));
        addInput(createInputCentered<LogicThemedPJ301MPort>(Vec(col3, 153.5f), module, CognitiveShift::LOGIC_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col4, 153.5f), module, CognitiveShift::XOR_INPUT));

        // Params
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col1, 103.5f), module, CognitiveShift::CLEAR_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col2, 103.5f), module, CognitiveShift::THRESHOLD_CV_INPUT));
        addParam(createParamCentered<Trimpot>(Vec(col3, 103.5f), module, CognitiveShift::THRESHOLD_CV_ATTN_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(col4, 103.5f), module, CognitiveShift::THRESHOLD_PARAM));

        // Step and DAC Lights
        float light_start_x = 34.84f;
        float light_spacing_x = 45.f;
        float light_row1_y = 268.03f;
        for (int i = 0; i < 4; ++i) {
            float lightX = light_start_x + i * light_spacing_x;
            auto* light = createLightCentered<TinyLight<GreenLight>>(Vec(lightX, light_row1_y), module, CognitiveShift::STEP_LIGHTS + i);
            addChild(light);
            stepLightWidgets[i] = light;
        }
        float light_row2_y = 318.58;
        for (int i = 0; i < 4; ++i) {
            float lightX = light_start_x + i * light_spacing_x;
            auto* light = createLightCentered<TinyLight<GreenLight>>(Vec(lightX, light_row2_y), module, CognitiveShift::STEP_LIGHTS + 4 + i);
            addChild(light);
            stepLightWidgets[4 + i] = light;
        }

        float light_row_dac_y = 219.58;
        for (int i = 0; i < NUM_DAC; i++) {
            float lightX = light_start_x + i * light_spacing_x;
            auto* light = createLightCentered<TinyLight<GreenRedLight>>(Vec(lightX, light_row_dac_y), module, CognitiveShift::DAC_LIGHTS + 2 * i);
            dacLightWidgets[i] = light;
            addChild(light);
        }

        // DAC Outputs
        float dac_out_y = 231.29;
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col1, dac_out_y), module, CognitiveShift::DAC_1_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col2, dac_out_y), module, CognitiveShift::DAC_2_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col3, dac_out_y), module, CognitiveShift::DAC_3_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col4, dac_out_y), module, CognitiveShift::DAC_OUTPUT));

        // DAC Attenuators
        float dac_attn_y = 203.81;
        addParam(createParamCentered<Trimpot>(Vec(col1, dac_attn_y), module, CognitiveShift::DAC_1_ATTN_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col2, dac_attn_y), module, CognitiveShift::DAC_2_ATTN_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col3, dac_attn_y), module, CognitiveShift::DAC_3_ATTN_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col4, dac_attn_y), module, CognitiveShift::DAC_4_ATTN_PARAM));

        // Individual Bit Outputs
        float bit_out_start_x = 22.5f;
        float bit_out_spacing_x = 45.f;
        float bit_out_row1_y = 280;
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(bit_out_start_x + 0 * bit_out_spacing_x, bit_out_row1_y), module, CognitiveShift::BIT_1_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(bit_out_start_x + 1 * bit_out_spacing_x, bit_out_row1_y), module, CognitiveShift::BIT_2_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(bit_out_start_x + 2 * bit_out_spacing_x, bit_out_row1_y), module, CognitiveShift::BIT_3_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(bit_out_start_x + 3 * bit_out_spacing_x, bit_out_row1_y), module, CognitiveShift::BIT_4_OUTPUT));
        float bit_out_row2_y = 329.25;
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(bit_out_start_x + 0 * bit_out_spacing_x, bit_out_row2_y), module, CognitiveShift::BIT_5_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(bit_out_start_x + 1 * bit_out_spacing_x, bit_out_row2_y), module, CognitiveShift::BIT_6_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(bit_out_start_x + 2 * bit_out_spacing_x, bit_out_row2_y), module, CognitiveShift::BIT_7_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(bit_out_start_x + 3 * bit_out_spacing_x, bit_out_row2_y), module, CognitiveShift::BIT_8_OUTPUT));
    }

    void step() override {
        ModuleWidget::step();

        CognitiveShift* csModule = dynamic_cast<CognitiveShift*>(module);
        if (csModule) {
            // Run from UI thread to avoid lock collisions
            csModule->checkInputConnections();

            // Get desired visibility states from the module
            bool bitLightsVisible = csModule->showBitLights;
            bool dacLightsVisible = csModule->showDACLights;

            // Update visibility of Step (Bit) light widgets
            for (int i = 0; i < NUM_STEPS; ++i) {
                if (stepLightWidgets[i]) {
                    stepLightWidgets[i]->visible = bitLightsVisible;
                }
            }

            for (int i = 0; i < NUM_DAC; i++) {
                if (dacLightWidgets[i]) {
                    dacLightWidgets[i]->visible = dacLightsVisible;
                }
            }
        }
    }

    void appendContextMenu(Menu* menu) override {
        CognitiveShift* module = dynamic_cast<CognitiveShift*>(this->module);
        assert(module);
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Settings"));
        menu->addChild(createIndexPtrSubmenuItem("Bit output mode",
                                                 {"Clocks", "Gates", "Triggers"},
                                                 &module->outputType));
        menu->addChild(createIndexPtrSubmenuItem("Logic type",
                                                 logicOperatorNames,
                                                 &module->logicType));
        menu->addChild(createIndexPtrSubmenuItem("Input overrides",
                                                 {"Data", "Everything"},
                                                 &module->inputOverridesEverything));
        menu->addChild(createIndexPtrSubmenuItem("8 Bit DAC output",
                                                 {"Bipolar", "Unipolar"},
                                                 &module->dacOutputType));
        menu->addChild(createMenuLabel("UI"));
        menu->addChild(createBoolPtrMenuItem("Show DAC lights", "", &module->showDACLights));
        menu->addChild(createBoolPtrMenuItem("Show bit lights", "", &module->showBitLights));
    }
};

Model* modelCognitiveShift = createModel<CognitiveShift, CognitiveShiftWidget>("CognitiveShift");