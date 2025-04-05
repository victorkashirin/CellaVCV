#include <algorithm>  // For std::copy, std::fill
#include <vector>     // Still needed for std::vector in dataFromJson potentially, though not strictly necessary for current state vars

#include "components.hpp"  // Assuming this includes necessary VCV components like SchmittTrigger, etc.
#include "plugin.hpp"      // Assuming this includes Module, ModuleWidget, etc.

// Define constants for clarity
const int NUM_STEPS = 8;
const float R2R_MAX_VOLTAGE = 10.0f;
const float R2R_SCALE = R2R_MAX_VOLTAGE / 15.0f;  // For 4 bits (2^4 - 1 = 15)
const float GATE_VOLTAGE = 10.0f;
const float DATA_INPUT_THRESHOLD = 1.0f;  // Voltage threshold for data/xor input trigger
const float CLOCK_HIGH_THRESHOLD = 1.0f;  // Voltage threshold for clock input considered 'high' for gate output
const float DAC_BIPOLAR_VOLTAGE = 5.0f;   // Target nominal bipolar range (+/- 5V) before attenuverter

// --- Module Logic ---
struct CognitiveShift : Module {
    enum ParamIds {
        WRITE_BUTTON_PARAM,
        ERASE_BUTTON_PARAM,
        RESET_BUTTON_PARAM,
        THRESHOLD_PARAM,
        THRESHOLD_CV_ATTENUVERTER_PARAM,
        R2R_1_ATTN_PARAM,
        R2R_2_ATTN_PARAM,
        R2R_3_ATTN_PARAM,
        DAC_ATTENUVERTER_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        CLOCK_INPUT,
        DATA_INPUT,
        XOR_INPUT,
        XOR_2_INPUT,
        RESET_INPUT,
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
        STEP_LIGHTS,  // 0-7
        // CLOCK_LIGHT, // REMOVED - Light ID 8 is now BUTTON_PRESS_LIGHT
        BUTTON_PRESS_LIGHT = STEP_LIGHTS + NUM_STEPS,  // 8
        NUM_LIGHTS                                     // 9
    };

    // Internal state
    dsp::SchmittTrigger clockInputTrigger;
    dsp::SchmittTrigger resetTrigger;
    dsp::SchmittTrigger resetInputTrigger;
    dsp::PulseGenerator pulseGen;
    bool bits[NUM_STEPS] = {};
    bool previousBits[NUM_STEPS] = {};
    int64_t currentClock = 0;
    int64_t previousClock = 0;

    CognitiveShift* inMods[NUM_INPUTS];
    int inputBits[NUM_INPUTS] = {-1};
    bool wasInputConnected[NUM_INPUTS] = {false};

    enum OutputType {
        CLOCK_OUTPUT,
        GATE_OUTPUT,
        TRIGGER_OUTPUT
    };

    int outputType = OutputType::CLOCK_OUTPUT;
    bool signalDelayCompensation = true;

    CognitiveShift() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configButton(WRITE_BUTTON_PARAM, "Data + (Write)");
        configButton(ERASE_BUTTON_PARAM, "Data - (Erase)");
        configButton(RESET_BUTTON_PARAM, "Reset Bits");
        configParam(THRESHOLD_PARAM, 1.f, 9.f, 1.f, "Data Input Threshold");
        configParam(THRESHOLD_CV_ATTENUVERTER_PARAM, -1.f, 1.f, 0.f, "Threshold CV Attenuverter");
        configParam(R2R_1_ATTN_PARAM, -1.f, 1.f, 1.f, "R2R 1 (Bits 1-4) Level");
        configParam(R2R_2_ATTN_PARAM, -1.f, 1.f, 1.f, "R2R 2 (Bits 3-6) Level");
        configParam(R2R_3_ATTN_PARAM, -1.f, 1.f, 1.f, "R2R 3 (Bits 5-8) Level");
        configParam(DAC_ATTENUVERTER_PARAM, -1.f, 1.f, 1.f, "8-Bit DAC Level");

        configInput(CLOCK_INPUT, "Clock Trigger");
        configInput(DATA_INPUT, "Data");
        configInput(XOR_INPUT, "XOR A");
        configInput(XOR_2_INPUT, "XOR B");
        configInput(RESET_INPUT, "Reset");
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

        onReset();  // Initialize state properly
    }

    // R2R Helper (unchanged)
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

    // 8-Bit DAC Calculation Helper (unchanged)
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
        // Clear removed state variables
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
        if (!signalDelayCompensation) {
            return;
        }
        for (int i = 0; i < NUM_INPUTS; ++i) {
            if (inputs[i].isConnected()) {
                if (!wasInputConnected[i]) {
                    inMods[i] = nullptr;
                    wasInputConnected[i] = true;
                    for (uint64_t cableId : APP->engine->getCableIds()) {
                        Cable* cable = APP->engine->getCable(cableId);
                        if (!cable) continue;
                        if (cable->inputModule == this && cable->inputId == i) {
                            Module* outputModule = cable->outputModule;
                            if (outputModule && outputModule->getModel() == this->getModel()) {
                                CognitiveShift* sourceCognitiveShift = dynamic_cast<CognitiveShift*>(outputModule);
                                inMods[i] = sourceCognitiveShift;
                                inputBits[i] = cable->outputId;
                            }
                        }
                    }
                }
            } else if (wasInputConnected[i]) {
                inMods[i] = nullptr;
                inputBits[i] = -1;
                wasInputConnected[i] = false;
            }
        }
    }

    bool getDataInput(int inputId, float threshold = DATA_INPUT_THRESHOLD) {
        bool effectiveDataInputHigh = false;

        if (inputs[inputId].isConnected()) {
            if (signalDelayCompensation && inMods[inputId] != nullptr) {
                // If we have a connected input, check if it's self-patched
                int sourceOutputId = inputBits[inputId];
                int bitIndex = outputIdToBitIndex(sourceOutputId);
                if (bitIndex != -1) {
                    if (inMods[inputId]->previousClock == currentClock) {
                        effectiveDataInputHigh = inMods[inputId]->previousBits[bitIndex];
                    } else {
                        effectiveDataInputHigh = inMods[inputId]->bits[bitIndex];
                    }
                }
            } else {
                DEBUG("Voltage input %d: %f", inputId, inputs[inputId].getVoltage());
                effectiveDataInputHigh = inputs[inputId].getVoltage() >= threshold;  // Default
            }
        }
        return effectiveDataInputHigh;
    }

    void process(const ProcessArgs& args) override {
        checkInputConnections();

        // --- Immediate Reset Button Logic ---
        if (resetTrigger.process(params[RESET_BUTTON_PARAM].getValue()) || resetInputTrigger.process(inputs[RESET_INPUT].getVoltage())) {
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
            // --- Start: Input Reading with Self-Patch Detection (Rack v2 Engine ID Method) ---

            float thresholdCV = params[THRESHOLD_CV_ATTENUVERTER_PARAM].getValue() * inputs[THRESHOLD_CV_INPUT].getVoltage();
            float threshold = clamp(params[THRESHOLD_PARAM].getValue() + thresholdCV, 1.f, 9.f);

            // 1. Read button states
            bool writeButtonPressed = params[WRITE_BUTTON_PARAM].getValue() > 0.f;
            bool eraseButtonPressed = params[ERASE_BUTTON_PARAM].getValue() > 0.f;

            // --- End: Input Reading with Self-Patch Detection ---

            // 4. Determine final dataBit based on priority: Write > Erase > Effective Data Input
            bool dataBit = false;
            if (writeButtonPressed) {
                dataBit = true;
            } else if (eraseButtonPressed) {
                dataBit = false;
            } else {
                dataBit = getDataInput(DATA_INPUT, threshold);  // Use the potentially overridden value
            }

            // 5. Determine final xorBit from the effective XOR input
            bool xorBit = getDataInput(XOR_INPUT, threshold);     // Use the potentially overridden value
            bool xor2Bit = getDataInput(XOR_2_INPUT, threshold);  // Use the potentially overridden value

            // 6. Calculate the bit to shift in
            bool nextBit = dataBit ^ xorBit ^ xor2Bit;

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
        // (This part remains unchanged)
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

        // --- Calculate and Output R2R --- (Unchanged)
        float r2r1_raw = calculateR2R(0, 4);
        float r2r2_raw = calculateR2R(2, 4);
        float r2r3_raw = calculateR2R(4, 4);
        float output_scale = 10.0f / R2R_MAX_VOLTAGE;
        float r2r1_final = r2r1_raw * params[R2R_1_ATTN_PARAM].getValue() * output_scale;
        float r2r2_final = r2r2_raw * params[R2R_2_ATTN_PARAM].getValue() * output_scale;
        float r2r3_final = r2r3_raw * params[R2R_3_ATTN_PARAM].getValue() * output_scale;
        outputs[R2R_1_OUTPUT].setVoltage(r2r1_final);
        outputs[R2R_2_OUTPUT].setVoltage(r2r2_final);
        outputs[R2R_3_OUTPUT].setVoltage(r2r3_final);

        // --- Calculate and Output 8-Bit DAC --- (Unchanged)
        float dacRawValue = calculate8BitDACRaw();
        float dacBipolarValue = (dacRawValue / 255.0f) * (2.0f * DAC_BIPOLAR_VOLTAGE) - DAC_BIPOLAR_VOLTAGE;
        float dacAttn = params[DAC_ATTENUVERTER_PARAM].getValue();
        float finalDacOutput = dacBipolarValue * dacAttn;
        outputs[DAC_OUTPUT].setVoltage(finalDacOutput);

        // --- Update Button Press Light --- (Unchanged)
        bool writePressed = params[WRITE_BUTTON_PARAM].getValue() > 0.f;
        bool erasePressed = params[ERASE_BUTTON_PARAM].getValue() > 0.f;
        bool resetPressed = params[RESET_BUTTON_PARAM].getValue() > 0.f;
        lights[BUTTON_PRESS_LIGHT].setBrightness((writePressed || erasePressed || resetPressed) ? 1.0f : 0.0f);
    }

    // dataToJson/FromJson remain the same as they only handle the 'bits' array
    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_t* valuesJ = json_array();
        for (int i = 0; i < NUM_STEPS; i++) {
            json_array_insert_new(valuesJ, i, json_boolean(bits[i]));  // Use json_boolean for clarity
        }
        json_object_set_new(rootJ, "values", valuesJ);
        json_object_set_new(rootJ, "outputType", json_integer(outputType));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* valuesJ = json_object_get(rootJ, "values");
        if (valuesJ) {
            for (int i = 0; i < NUM_STEPS; i++) {
                json_t* valueJ = json_array_get(valuesJ, i);
                if (valueJ)
                    bits[i] = json_boolean_value(valueJ);  // Use json_boolean_value
            }
        }
        json_t* outputTypeJ = json_object_get(rootJ, "outputType");
        if (outputTypeJ)
            outputType = json_integer_value(outputTypeJ);
    }
};

// --- Module Widget (GUI) ---
struct CognitiveShiftWidget : ModuleWidget {
    CognitiveShiftWidget(CognitiveShift* module) {
        setModule(module);
        // Ensure panel files exist at these paths
        setPanel(createPanel(asset::plugin(pluginInstance, "res/CognitiveShift.svg"), asset::plugin(pluginInstance, "res/CognitiveShift-dark.svg")));

        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Panel layout constants (Keep existing ones)
        float col1 = 22.5f;
        float col2 = 67.5f;
        float col3 = 112.5f;
        float col4 = 157.5f;

        // Row 1 & 2: Original Clock Rate controls and Gate Length REMOVED
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col1, 103.5f), module, CognitiveShift::RESET_INPUT));             // Kept position
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col2, 103.5f), module, CognitiveShift::THRESHOLD_CV_INPUT));      // REMOVED
        addParam(createParamCentered<Trimpot>(Vec(col3, 103.5f), module, CognitiveShift::THRESHOLD_CV_ATTENUVERTER_PARAM));  // REMOVED
        addParam(createParamCentered<RoundBlackKnob>(Vec(col4, 103.5f), module, CognitiveShift::THRESHOLD_PARAM));

        // Row 3: Inputs and CLOCK_OUTPUT REMOVED
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col1, 153.5f), module, CognitiveShift::CLOCK_INPUT));  // Kept position
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col2, 153.5f), module, CognitiveShift::DATA_INPUT));   // Kept position
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col3, 153.5f), module, CognitiveShift::XOR_INPUT));    // Kept position
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col4, 153.5f), module, CognitiveShift::XOR_2_INPUT));  // Kept position
        // addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col4, 153.5f), module, CognitiveShift::CLOCK_OUTPUT)); // REMOVED

        // Row 4: Buttons and Button Press Light (Kept positions)
        addParam(createParamCentered<VCVButton>(Vec(col1, 53.5f), module, CognitiveShift::RESET_BUTTON_PARAM));                    // Kept position
        addParam(createParamCentered<VCVButton>(Vec(col2, 53.5f), module, CognitiveShift::WRITE_BUTTON_PARAM));                    // Kept position
        addParam(createParamCentered<VCVButton>(Vec(col3, 53.5f), module, CognitiveShift::ERASE_BUTTON_PARAM));                    // Kept position
        addChild(createLightCentered<LargeFresnelLight<RedLight>>(Vec(col4, 53.5f), module, CognitiveShift::BUTTON_PRESS_LIGHT));  // Kept position

        // Row 5 & 6: Step Lights (Kept positions)
        float light_start_x = 34.84f;
        float light_spacing_x = 45.f;
        float light_row1_y = 268.03f;
        for (int i = 0; i < 4; ++i) {
            float lightX = light_start_x + i * light_spacing_x;
            addChild(createLightCentered<TinyLight<GreenLight>>(Vec(lightX, light_row1_y), module, CognitiveShift::STEP_LIGHTS + i));
        }
        float light_row2_y = 318.58;
        for (int i = 0; i < 4; ++i) {
            float lightX = light_start_x + i * light_spacing_x;
            addChild(createLightCentered<TinyLight<GreenLight>>(Vec(lightX, light_row2_y), module, CognitiveShift::STEP_LIGHTS + 4 + i));
        }

        // Row 7: R2R and DAC Outputs (Kept positions)
        float r2r_out_y = 231.29;
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col1, r2r_out_y), module, CognitiveShift::R2R_1_OUTPUT));  // Kept position
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col2, r2r_out_y), module, CognitiveShift::R2R_2_OUTPUT));  // Kept position
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col3, r2r_out_y), module, CognitiveShift::R2R_3_OUTPUT));  // Kept position
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col4, r2r_out_y), module, CognitiveShift::DAC_OUTPUT));    // Kept position

        // Row 8: R2R and DAC Attenuators (Kept positions)
        float r2r_attn_y = 203.81;
        addParam(createParamCentered<Trimpot>(Vec(col1, r2r_attn_y), module, CognitiveShift::R2R_1_ATTN_PARAM));        // Kept position
        addParam(createParamCentered<Trimpot>(Vec(col2, r2r_attn_y), module, CognitiveShift::R2R_2_ATTN_PARAM));        // Kept position
        addParam(createParamCentered<Trimpot>(Vec(col3, r2r_attn_y), module, CognitiveShift::R2R_3_ATTN_PARAM));        // Kept position
        addParam(createParamCentered<Trimpot>(Vec(col4, r2r_attn_y), module, CognitiveShift::DAC_ATTENUVERTER_PARAM));  // Kept position

        // Row 9 & 10: Individual Bit Outputs (Kept positions)
        float bit_out_start_x = 22.5f;
        float bit_out_spacing_x = 45.f;
        float bit_out_row1_y = 280;  // Adjusted slightly based on light Y coord? Keep original if preferred.
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(bit_out_start_x + 0 * bit_out_spacing_x, bit_out_row1_y), module, CognitiveShift::BIT_1_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(bit_out_start_x + 1 * bit_out_spacing_x, bit_out_row1_y), module, CognitiveShift::BIT_2_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(bit_out_start_x + 2 * bit_out_spacing_x, bit_out_row1_y), module, CognitiveShift::BIT_3_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(bit_out_start_x + 3 * bit_out_spacing_x, bit_out_row1_y), module, CognitiveShift::BIT_4_OUTPUT));
        float bit_out_row2_y = 329.25;  // Adjusted slightly based on light Y coord? Keep original if preferred.
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(bit_out_start_x + 0 * bit_out_spacing_x, bit_out_row2_y), module, CognitiveShift::BIT_5_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(bit_out_start_x + 1 * bit_out_spacing_x, bit_out_row2_y), module, CognitiveShift::BIT_6_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(bit_out_start_x + 2 * bit_out_spacing_x, bit_out_row2_y), module, CognitiveShift::BIT_7_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(bit_out_start_x + 3 * bit_out_spacing_x, bit_out_row2_y), module, CognitiveShift::BIT_8_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        CognitiveShift* module = dynamic_cast<CognitiveShift*>(this->module);
        assert(module);
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexPtrSubmenuItem("Bit output mode",
                                                 {"Clocks", "Gates", "Triggers"},
                                                 &module->outputType));
        menu->addChild(createIndexPtrSubmenuItem("Signal delay compensation",
                                                 {"Disabled", "Enabled"},
                                                 &module->signalDelayCompensation));
    }
};

// --- Plugin Registration --- (Ensure your plugin/model slugs are correct)
Model* modelCognitiveShift = createModel<CognitiveShift, CognitiveShiftWidget>("CognitiveShift");  // Use your actual plugin slug and a unique model slug