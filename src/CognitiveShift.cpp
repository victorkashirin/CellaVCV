#include <algorithm>  // For std::copy, std::fill
#include <vector>     // Still needed for std::vector in dataFromJson potentially, though not strictly necessary for current state vars

#include "components.hpp"  // Assuming this includes necessary VCV components like SchmittTrigger, etc.
#include "plugin.hpp"      // Assuming this includes Module, ModuleWidget, etc.

// Define constants for clarity
const int NUM_STEPS = 8;
const float R2R_MAX_VOLTAGE = 8.0f;
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
        // CLOCK_RATE_PARAM, // REMOVED
        // CLOCK_RATE_CV_ATTENUVERTER_PARAM, // REMOVED
        R2R_1_ATTN_PARAM,
        R2R_2_ATTN_PARAM,
        R2R_3_ATTN_PARAM,
        // GATE_LENGTH_PARAM, // REMOVED
        DAC_ATTENUVERTER_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        CLOCK_INPUT,
        DATA_INPUT,
        XOR_INPUT,
        // CLOCK_RATE_CV_INPUT, // REMOVED
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
        // CLOCK_OUTPUT, // REMOVED
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
    // Removed intention triggers: writeTrigger, eraseTrigger, dataInputTrigger, xorInputTrigger
    // Removed internal clock state: phase, clockOutputPulse, estimatedClockInterval, lastClockTime
    // Removed gate timing: gateEndTime
    // Removed intention flags: writeIntention, eraseIntention, dataInputHighIntention, xorInputHighIntention
    bool bits[NUM_STEPS] = {};

    CognitiveShift() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configButton(WRITE_BUTTON_PARAM, "Data + (Write)");
        configButton(ERASE_BUTTON_PARAM, "Data - (Erase)");
        configButton(RESET_BUTTON_PARAM, "Reset Bits");
        // configParam(CLOCK_RATE_PARAM, -2.f, 6.f, 2.f, "Clock Rate", " Hz", 2.f); // REMOVED
        // configParam(CLOCK_RATE_CV_ATTENUVERTER_PARAM, -1.f, 1.f, 1.f, "Clock Rate CV Attenuation"); // REMOVED
        configParam(R2R_1_ATTN_PARAM, -1.f, 1.f, 1.f, "R2R 1 (Bits 1-4) Level");
        configParam(R2R_2_ATTN_PARAM, -1.f, 1.f, 1.f, "R2R 2 (Bits 3-6) Level");
        configParam(R2R_3_ATTN_PARAM, -1.f, 1.f, 1.f, "R2R 3 (Bits 5-8) Level");
        // configParam(GATE_LENGTH_PARAM, 0.f, 1.f, 0.5f, "Gate Length", "%", 0.f, 100.f); // REMOVED
        configParam(DAC_ATTENUVERTER_PARAM, -1.f, 1.f, 1.f, "8-Bit DAC Level");

        configInput(CLOCK_INPUT, "Clock Trigger");  // Renamed for clarity
        configInput(DATA_INPUT, "Data In");
        configInput(XOR_INPUT, "XOR In");
        // configInput(CLOCK_RATE_CV_INPUT, "Clock Rate CV"); // REMOVED

        configOutput(R2R_1_OUTPUT, "R2R 1 (Bits 1-4)");
        configOutput(R2R_2_OUTPUT, "R2R 2 (Bits 3-6)");
        configOutput(R2R_3_OUTPUT, "R2R 3 (Bits 5-8)");
        configOutput(BIT_1_OUTPUT, "Bit 1 Out");
        configOutput(BIT_2_OUTPUT, "Bit 2 Out");
        configOutput(BIT_3_OUTPUT, "Bit 3 Out");
        configOutput(BIT_4_OUTPUT, "Bit 4 Out");
        configOutput(BIT_5_OUTPUT, "Bit 5 Out");
        configOutput(BIT_6_OUTPUT, "Bit 6 Out");
        configOutput(BIT_7_OUTPUT, "Bit 7 Out");
        configOutput(BIT_8_OUTPUT, "Bit 8 Out");
        // configOutput(CLOCK_OUTPUT, "Clock Out"); // REMOVED
        configOutput(DAC_OUTPUT, "8-Bit DAC Out");

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
        // Clear removed state variables
        // Randomize DAC attenuverter? Optional.
        // params[DAC_ATTENUVERTER_PARAM].setValue(random::uniform() * 2.f - 1.f);
    }

    int outputIdToBitIndex(int outputId) {
        if (outputId >= BIT_1_OUTPUT && outputId <= BIT_8_OUTPUT) {
            return outputId - BIT_1_OUTPUT;
        }
        return -1;
    }

    void process(const ProcessArgs& args) override {
        // --- Immediate Reset Button Logic ---
        if (resetTrigger.process(params[RESET_BUTTON_PARAM].getValue())) {
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

            // 1. Read button states
            bool writeButtonPressed = params[WRITE_BUTTON_PARAM].getValue() > 0.f;
            bool eraseButtonPressed = params[ERASE_BUTTON_PARAM].getValue() > 0.f;

            // 2. Determine effective Data Input state
            bool effectiveDataInputHigh = false;

            if (inputs[DATA_INPUT].isConnected()) {
                effectiveDataInputHigh = inputs[DATA_INPUT].getVoltage() >= DATA_INPUT_THRESHOLD;  // Default

                // Iterate through cable IDs and get Cable objects
                for (uint64_t cableId : APP->engine->getCableIds()) {
                    Cable* cable = APP->engine->getCable(cableId);
                    if (!cable)  // Safety check
                        continue;

                    // Is this cable plugged into *our* DATA_INPUT?
                    if (cable->inputModule == this && cable->inputId == DATA_INPUT) {
                        Module* outputModule = cable->outputModule;
                        if (!outputModule) continue;  // Safety check
                        if (outputModule->getModel() == this->getModel()) {
                            CognitiveShift* sourceCognitiveShift = dynamic_cast<CognitiveShift*>(outputModule);
                            int sourceOutputId = cable->outputId;
                            int bitIndex = outputIdToBitIndex(sourceOutputId);
                            if (bitIndex != -1) {
                                // Self-patched from one of our bit outputs: read internal state
                                effectiveDataInputHigh = sourceCognitiveShift->bits[bitIndex];
                            }
                        }
                        break;
                    }
                }
            }

            // 3. Determine effective XOR Input state (using the same logic)
            bool effectiveXorInputHigh = false;

            if (inputs[XOR_INPUT].isConnected()) {
                effectiveXorInputHigh = inputs[XOR_INPUT].getVoltage() >= DATA_INPUT_THRESHOLD;  // Default

                for (uint64_t cableId : APP->engine->getCableIds()) {
                    rack::engine::Cable* cable = APP->engine->getCable(cableId);
                    if (!cable)
                        continue;

                    if (cable->inputModule == this && cable->inputId == XOR_INPUT) {
                        Module* outputModule = cable->outputModule;
                        if (!outputModule) continue;  // Safety check
                        if (outputModule->getModel() == this->getModel()) {
                            CognitiveShift* sourceCognitiveShift = dynamic_cast<CognitiveShift*>(outputModule);
                            int sourceOutputId = cable->outputId;
                            int bitIndex = outputIdToBitIndex(sourceOutputId);
                            if (bitIndex != -1) {
                                effectiveXorInputHigh = sourceCognitiveShift->bits[bitIndex];
                            }
                        }
                        break;  // Found the cable
                    }
                }
            }

            // --- End: Input Reading with Self-Patch Detection ---

            // 4. Determine final dataBit based on priority: Write > Erase > Effective Data Input
            bool dataBit = false;
            if (writeButtonPressed) {
                dataBit = true;
            } else if (eraseButtonPressed) {
                dataBit = false;
            } else {
                dataBit = effectiveDataInputHigh;  // Use the potentially overridden value
            }

            // 5. Determine final xorBit from the effective XOR input
            bool xorBit = effectiveXorInputHigh;  // Use the potentially overridden value

            // 6. Calculate the bit to shift in
            bool nextBit = dataBit ^ xorBit;
            // rack::logger::info("Clock tick! write:%d erase:%d dataInHigh:%d xorInHigh:%d | dataBit:%d xorBit:%d | nextBit:%d", writeButtonPressed, eraseButtonPressed, effectiveDataInputHigh, effectiveXorInputHigh, dataBit, xorBit, nextBit);

            // 7. Perform the shift
            for (int i = NUM_STEPS - 1; i > 0; --i) {
                bits[i] = bits[i - 1];
            }
            bits[0] = nextBit;

        }  // End of clocked_this_frame

        // --- Update Individual Bit Outputs & Lights ---
        // (This part remains unchanged)
        float clockInputVoltage = inputs[CLOCK_INPUT].isConnected() ? inputs[CLOCK_INPUT].getVoltage() : 0.f;
        bool isClockHigh = clockInputVoltage >= CLOCK_HIGH_THRESHOLD;

        for (int i = 0; i < NUM_STEPS; ++i) {
            float bitVoltage = (bits[i] && isClockHigh) ? GATE_VOLTAGE : 0.0f;
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
        // addParam(createParamCentered<RoundBlackKnob>(Vec(col1, 53.5f), module, CognitiveShift::CLOCK_RATE_PARAM)); // REMOVED
        // addInput(createInputCentered<ThemedPJ301MPort>(Vec(col2, 53.5f), module, CognitiveShift::CLOCK_RATE_CV_INPUT)); // REMOVED
        // addParam(createParamCentered<Trimpot>(Vec(col3, 53.5f), module, CognitiveShift::CLOCK_RATE_CV_ATTENUVERTER_PARAM)); // REMOVED
        // addParam(createParamCentered<RoundBlackKnob>(Vec(col4, 53.5f), module, CognitiveShift::GATE_LENGTH_PARAM)); // REMOVED

        // Row 3: Inputs and CLOCK_OUTPUT REMOVED
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col1, 153.5f), module, CognitiveShift::CLOCK_INPUT));  // Kept position
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col2, 153.5f), module, CognitiveShift::DATA_INPUT));   // Kept position
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col3, 153.5f), module, CognitiveShift::XOR_INPUT));    // Kept position
        // addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col4, 153.5f), module, CognitiveShift::CLOCK_OUTPUT)); // REMOVED

        // Row 4: Buttons and Button Press Light (Kept positions)
        addChild(createLightCentered<LargeFresnelLight<RedLight>>(Vec(col1, 103.5f), module, CognitiveShift::BUTTON_PRESS_LIGHT));  // Kept position
        addParam(createParamCentered<VCVButton>(Vec(col2, 103.5f), module, CognitiveShift::WRITE_BUTTON_PARAM));                    // Kept position
        addParam(createParamCentered<VCVButton>(Vec(col3, 103.5f), module, CognitiveShift::ERASE_BUTTON_PARAM));                    // Kept position
        addParam(createParamCentered<VCVButton>(Vec(col4, 103.5f), module, CognitiveShift::RESET_BUTTON_PARAM));                    // Kept position

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
};

// --- Plugin Registration --- (Ensure your plugin/model slugs are correct)
Model* modelCognitiveShift = createModel<CognitiveShift, CognitiveShiftWidget>("CognitiveShift");  // Use your actual plugin slug and a unique model slug