#include <algorithm>
#include <vector>

#include "components.hpp"
#include "plugin.hpp"

// Define constants for clarity
const int NUM_STEPS = 8;
const int NUM_R2R_DAC = 4;
const int NUM_COMPLEX_INPUTS = 3;  // only DATA, XOR and LOGIC
const float R2R_MAX_VOLTAGE = 10.0f;
const float R2R_SCALE = R2R_MAX_VOLTAGE / 15.0f;  // For 4 bits (2^4 - 1 = 15)
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
        THRESHOLD_CV_ATTENUVERTER_PARAM,
        R2R_1_ATTN_PARAM,
        R2R_2_ATTN_PARAM,
        R2R_3_ATTN_PARAM,
        DAC_ATTENUVERTER_PARAM,
        LOGIC_PARAM,
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
        R2R_1_OUTPUT,
        R2R_2_OUTPUT,
        R2R_3_OUTPUT,
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
        R2R_LIGHTS,
        NUM_LIGHTS = R2R_LIGHTS + 8  // 9
    };

    // Internal state
    dsp::SchmittTrigger clockInputTrigger;
    dsp::SchmittTrigger clearTrigger;
    dsp::SchmittTrigger clearInputTrigger;
    dsp::PulseGenerator pulseGen;
    bool bits[NUM_STEPS] = {};
    bool previousBits[NUM_STEPS] = {};
    int64_t currentClock = 0;
    int64_t previousClock = 0;

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
    bool showR2RLights = true;

    CognitiveShift() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configButton(WRITE_BUTTON_PARAM, "Data + (Write)");
        configButton(ERASE_BUTTON_PARAM, "Data - (Erase)");
        configButton(CLEAR_BUTTON_PARAM, "Clear register");
        configParam(THRESHOLD_PARAM, 1.f, 9.f, 1.f, "Data Input Threshold");
        configParam(THRESHOLD_CV_ATTENUVERTER_PARAM, -1.f, 1.f, 0.f, "Threshold CV Attenuverter");
        configParam(R2R_1_ATTN_PARAM, -1.f, 1.f, 1.f, "R2R 1 (Bits 1-4) Level");
        configParam(R2R_2_ATTN_PARAM, -1.f, 1.f, 1.f, "R2R 2 (Bits 3-6) Level");
        configParam(R2R_3_ATTN_PARAM, -1.f, 1.f, 1.f, "R2R 3 (Bits 5-8) Level");
        configParam(DAC_ATTENUVERTER_PARAM, -1.f, 1.f, 1.f, "8-Bit DAC Level");

        configInput(CLOCK_INPUT, "Clock Trigger");
        configInput(DATA_INPUT, "Data");
        configInput(XOR_INPUT, "XOR");
        configInput(LOGIC_INPUT, "Logic");
        configInput(CLEAR_INPUT, "Clear register");
        configInput(THRESHOLD_CV_INPUT, "Threshold CV");

        configOutput(R2R_1_OUTPUT, "R2R 1 (Bits 1-4)");
        configOutput(R2R_2_OUTPUT, "R2R 2 (Bits 3-6)");
        configOutput(R2R_3_OUTPUT, "R2R 3 (Bits 5-8)");
        configOutput(BIT_1_OUTPUT, "Bit 1");
        configOutput(BIT_2_OUTPUT, "Bit 2");
        configOutput(BIT_3_OUTPUT, "Bit 3");
        configOutput(BIT_4_OUTPUT, "Bit 4");
        configOutput(BIT_5_OUTPUT, "Bit 5");
        configOutput(BIT_6_OUTPUT, "Bit 6");
        configOutput(BIT_7_OUTPUT, "Bit 7");
        configOutput(BIT_8_OUTPUT, "Bit 8");
        configOutput(DAC_OUTPUT, "8-Bit Bipolar DAC");

        onReset();
    }

    // R2R Helper
    float calculateR2R(int startIndex, int numBits = 4) {
        float r2rValue = 0.0f;
        float weight = 1.0f;
        for (int i = 0; i < numBits; ++i) {
            int bitIndex = startIndex + i;
            if (bitIndex < NUM_STEPS && bits[bitIndex])
                r2rValue += weight;
            weight *= 2.0f;
        }
        return r2rValue * R2R_SCALE;
    }

    // 8-Bit DAC Calculation Helper
    float calculate8BitDACRaw() {
        float dacValue = 0.0f;
        float weight = 1.0f;  // Start with LSB weight
        for (int i = 0; i < NUM_STEPS; ++i) {
            if (bits[i]) {
                dacValue += weight;
            }
            weight *= 2.0f;  // Move to next bit's weight
        }
        return dacValue;  // Raw value is 0 to 255
    }

    void onReset() override {
        std::fill(bits, bits + NUM_STEPS, false);
        std::fill(previousBits, previousBits + NUM_STEPS, false);
        std::fill(connectedSourceModules, connectedSourceModules + NUM_COMPLEX_INPUTS, nullptr);
        std::fill(connectedSourceOutputIds, connectedSourceOutputIds + NUM_COMPLEX_INPUTS, -1);
        std::fill(wasInputConnected, wasInputConnected + NUM_COMPLEX_INPUTS, false);
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
        // --- Immediate Reset Button Logic ---
        if (clearTrigger.process(params[CLEAR_BUTTON_PARAM].getValue()) || clearInputTrigger.process(inputs[CLEAR_INPUT].getVoltage())) {
            std::fill(bits, bits + NUM_STEPS, false);
        }

        // --- Clock Input Processing ---
        bool clocked_this_frame = false;
        if (inputs[CLOCK_INPUT].isConnected()) {
            if (clockInputTrigger.process(inputs[CLOCK_INPUT].getVoltage())) {
                clocked_this_frame = true;
            }
        }

        // --- Shift Register Logic (Only execute on clock tick) ---
        if (clocked_this_frame) {
            float thresholdCV = params[THRESHOLD_CV_ATTENUVERTER_PARAM].getValue() * inputs[THRESHOLD_CV_INPUT].getVoltage();
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
        float clockInputVoltage = inputs[CLOCK_INPUT].isConnected() ? inputs[CLOCK_INPUT].getVoltage() : 0.f;
        bool isClockHigh = clockInputVoltage >= CLOCK_HIGH_THRESHOLD;

        for (int i = 0; i < NUM_STEPS; ++i) {
            float bitVoltage = 0.0f;

            if (bits[i]) {
                if (outputType == OutputType::GATE_OUTPUT) {
                    bitVoltage = GATE_VOLTAGE;
                } else if (outputType == OutputType::TRIGGER_OUTPUT) {
                    if (clocked_this_frame) {
                        pulseGen.trigger(0.001f);
                    }
                    bitVoltage = pulseGen.process(args.sampleTime) ? GATE_VOLTAGE : 0.0f;
                } else if (outputType == OutputType::CLOCK_OUTPUT) {
                    bitVoltage = isClockHigh ? GATE_VOLTAGE : 0.0f;
                }
            }

            int outputId = BIT_1_OUTPUT + i;
            if (outputId >= BIT_1_OUTPUT && outputId <= BIT_8_OUTPUT) {
                outputs[outputId].setVoltage(bitVoltage);
            }
            lights[STEP_LIGHTS + i].setBrightness(bits[i] ? 1.0f : 0.0f);
        }

        // --- Calculate and Output R2R ---
        float r2r1_raw = calculateR2R(0, 4);
        float r2r2_raw = calculateR2R(2, 4);
        float r2r3_raw = calculateR2R(4, 4);
        float r2r1_final = r2r1_raw * params[R2R_1_ATTN_PARAM].getValue();
        float r2r2_final = r2r2_raw * params[R2R_2_ATTN_PARAM].getValue();
        float r2r3_final = r2r3_raw * params[R2R_3_ATTN_PARAM].getValue();
        outputs[R2R_1_OUTPUT].setVoltage(r2r1_final);
        outputs[R2R_2_OUTPUT].setVoltage(r2r2_final);
        outputs[R2R_3_OUTPUT].setVoltage(r2r3_final);

        lights[R2R_LIGHTS + 0].setBrightness(fmaxf(0.0f, r2r1_final / 10.f));
        lights[R2R_LIGHTS + 1].setBrightness(fmaxf(0.0f, -r2r1_final / 10.f));

        lights[R2R_LIGHTS + 2].setBrightness(fmaxf(0.0f, r2r2_final / 10.f));
        lights[R2R_LIGHTS + 3].setBrightness(fmaxf(0.0f, -r2r2_final / 10.f));

        lights[R2R_LIGHTS + 4].setBrightness(fmaxf(0.0f, r2r3_final / 10.f));
        lights[R2R_LIGHTS + 5].setBrightness(fmaxf(0.0f, -r2r3_final / 10.f));

        // --- Calculate and Output 8-Bit DAC ---
        float dacRawValue = calculate8BitDACRaw();
        float dacBipolarValue = dacRawValue / 255.0f;
        if (dacOutputType == AllBitDACOutputType::BIPOLAR) {
            dacBipolarValue = dacBipolarValue * 10.0f - 5.f;  // Convert to bipolar range
        } else {
            dacBipolarValue = dacBipolarValue * 10.f;  // Convert to unipolar range
        }

        float dacAttn = params[DAC_ATTENUVERTER_PARAM].getValue();
        float finalDacOutput = dacBipolarValue * dacAttn;
        outputs[DAC_OUTPUT].setVoltage(finalDacOutput);

        float lightScale = (dacOutputType == AllBitDACOutputType::BIPOLAR) ? 5.f : 10.f;

        lights[R2R_LIGHTS + 6]
            .setBrightness(fmaxf(0.0f, finalDacOutput / lightScale));
        lights[R2R_LIGHTS + 7].setBrightness(fmaxf(0.0f, -finalDacOutput / lightScale));

        // --- Update Button Press Light ---
        bool writePressed = params[WRITE_BUTTON_PARAM].getValue() > 0.f;
        bool erasePressed = params[ERASE_BUTTON_PARAM].getValue() > 0.f;
        bool clearPressed = params[CLEAR_BUTTON_PARAM].getValue() > 0.f;
        lights[BUTTON_PRESS_LIGHT].setBrightness((writePressed || erasePressed || clearPressed) ? 1.0f : 0.0f);
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
        json_object_set_new(rootJ, "showR2RLights", json_boolean(showR2RLights));
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

        json_t* showR2RLightsJ = json_object_get(rootJ, "showR2RLights");
        if (showR2RLightsJ)
            showR2RLights = json_boolean_value(showR2RLightsJ);
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
    rack::widget::Widget* r2rDacLightWidgets[NUM_R2R_DAC];

    CognitiveShiftWidget(CognitiveShift* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/CognitiveShift.svg"), asset::plugin(pluginInstance, "res/CognitiveShift-dark.svg")));

        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        float col1 = 22.5f;
        float col2 = 67.5f;
        float col3 = 112.5f;
        float col4 = 157.5f;

        // Buttons and Button Press Light
        addParam(createParamCentered<VCVButton>(Vec(col1, 53.5f), module, CognitiveShift::CLEAR_BUTTON_PARAM));
        addParam(createParamCentered<VCVButton>(Vec(col2, 53.5f), module, CognitiveShift::WRITE_BUTTON_PARAM));
        addParam(createParamCentered<VCVButton>(Vec(col3, 53.5f), module, CognitiveShift::ERASE_BUTTON_PARAM));
        addChild(createLightCentered<LargeFresnelLight<BlueLight>>(Vec(col4, 53.5f), module, CognitiveShift::BUTTON_PRESS_LIGHT));

        // INPUTS
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col1, 153.5f), module, CognitiveShift::CLOCK_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col2, 153.5f), module, CognitiveShift::DATA_INPUT));
        addInput(createInputCentered<LogicThemedPJ301MPort>(Vec(col3, 153.5f), module, CognitiveShift::LOGIC_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col4, 153.5f), module, CognitiveShift::XOR_INPUT));

        // Params
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col1, 103.5f), module, CognitiveShift::CLEAR_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col2, 103.5f), module, CognitiveShift::THRESHOLD_CV_INPUT));
        addParam(createParamCentered<Trimpot>(Vec(col3, 103.5f), module, CognitiveShift::THRESHOLD_CV_ATTENUVERTER_PARAM));
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

        float light_row_r2r_y = 219.58;
        for (int i = 0; i < NUM_R2R_DAC; i++) {
            float lightX = light_start_x + i * light_spacing_x;
            auto* light = createLightCentered<TinyLight<GreenRedLight>>(Vec(lightX, light_row_r2r_y), module, CognitiveShift::R2R_LIGHTS + 2 * i);
            r2rDacLightWidgets[i] = light;
            addChild(light);
        }

        // R2R and DAC Outputs
        float r2r_out_y = 231.29;
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col1, r2r_out_y), module, CognitiveShift::R2R_1_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col2, r2r_out_y), module, CognitiveShift::R2R_2_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col3, r2r_out_y), module, CognitiveShift::R2R_3_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col4, r2r_out_y), module, CognitiveShift::DAC_OUTPUT));

        // R2R and DAC Attenuators
        float r2r_attn_y = 203.81;
        addParam(createParamCentered<Trimpot>(Vec(col1, r2r_attn_y), module, CognitiveShift::R2R_1_ATTN_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col2, r2r_attn_y), module, CognitiveShift::R2R_2_ATTN_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col3, r2r_attn_y), module, CognitiveShift::R2R_3_ATTN_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col4, r2r_attn_y), module, CognitiveShift::DAC_ATTENUVERTER_PARAM));

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
            bool r2rLightsVisible = csModule->showR2RLights;

            // Update visibility of Step (Bit) light widgets
            for (int i = 0; i < NUM_STEPS; ++i) {
                if (stepLightWidgets[i]) {
                    stepLightWidgets[i]->visible = bitLightsVisible;
                }
            }

            for (int i = 0; i < NUM_R2R_DAC; i++) {
                if (r2rDacLightWidgets[i]) {
                    r2rDacLightWidgets[i]->visible = r2rLightsVisible;
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
        menu->addChild(createBoolPtrMenuItem("Show R2R lights", "", &module->showR2RLights));
        menu->addChild(createBoolPtrMenuItem("Show bit lights", "", &module->showBitLights));
    }
};

Model* modelCognitiveShift = createModel<CognitiveShift, CognitiveShiftWidget>("CognitiveShift");