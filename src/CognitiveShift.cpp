#include <algorithm>  // For std::max, std::copy, std::fill
#include <cmath>
#include <numeric>  // For std::accumulate (potential alternative for bit sum)
#include <vector>

#include "components.hpp"
#include "plugin.hpp"

// Define constants for clarity
const int NUM_STEPS = 8;
const float R2R_MAX_VOLTAGE = 8.0f;
const float R2R_SCALE = R2R_MAX_VOLTAGE / 15.0f;  // For 4 bits (2^4 - 1 = 15)
const float GATE_VOLTAGE = 10.0f;
const float MIN_GATE_DURATION_SECONDS = 0.001f;  // 1ms minimum gate/trigger
const float DATA_INPUT_THRESHOLD = 1.0f;         // Voltage threshold for data/xor input trigger
const float DAC_BIPOLAR_VOLTAGE = 5.0f;          // Target nominal bipolar range (+/- 5V) before attenuverter

// --- Module Logic ---
struct CognitiveShift : Module {
    enum ParamIds {
        WRITE_BUTTON_PARAM,
        ERASE_BUTTON_PARAM,
        RESET_BUTTON_PARAM,
        CLOCK_RATE_PARAM,
        CLOCK_RATE_CV_ATTENUVERTER_PARAM,
        R2R_1_ATTN_PARAM,
        R2R_2_ATTN_PARAM,
        R2R_3_ATTN_PARAM,
        GATE_LENGTH_PARAM,
        DAC_ATTENUVERTER_PARAM,  // <<< NEW
        NUM_PARAMS
    };
    enum InputIds {
        CLOCK_INPUT,
        DATA_INPUT,
        XOR_INPUT,
        CLOCK_RATE_CV_INPUT,
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
        CLOCK_OUTPUT,
        DAC_OUTPUT,  // <<< NEW
        NUM_OUTPUTS
    };
    enum LightIds {
        STEP_LIGHTS,                            // 0-7
        CLOCK_LIGHT = STEP_LIGHTS + NUM_STEPS,  // 8
        BUTTON_PRESS_LIGHT,                     // 9 <<< NEW
        NUM_LIGHTS                              // 10
    };

    // Internal state
    dsp::SchmittTrigger clockInputTrigger;
    dsp::SchmittTrigger writeTrigger;
    dsp::SchmittTrigger eraseTrigger;
    dsp::SchmittTrigger resetTrigger;
    dsp::SchmittTrigger dataInputTrigger;
    dsp::SchmittTrigger xorInputTrigger;
    dsp::PulseGenerator clockOutputPulse;
    float phase = 0.f;
    double moduleElapsedTime = 0.0;
    float estimatedClockInterval = 0.25f;
    double lastClockTime = -1.0;
    std::vector<double> gateEndTime;
    bool writeIntention = false;
    bool eraseIntention = false;
    bool dataInputHighIntention = false;
    bool xorInputHighIntention = false;
    bool bits[NUM_STEPS] = {};

    CognitiveShift() : gateEndTime(NUM_STEPS, 0.0) {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configButton(WRITE_BUTTON_PARAM, "Data + (Write)");
        configButton(ERASE_BUTTON_PARAM, "Data - (Erase)");
        configButton(RESET_BUTTON_PARAM, "Reset Bits");
        configParam(CLOCK_RATE_PARAM, -2.f, 6.f, 2.f, "Clock Rate", " Hz", 2.f);
        configParam(CLOCK_RATE_CV_ATTENUVERTER_PARAM, -1.f, 1.f, 1.f, "Clock Rate CV Attenuation");
        configParam(R2R_1_ATTN_PARAM, -1.f, 1.f, 1.f, "R2R 1 (Bits 1-4) Level");
        configParam(R2R_2_ATTN_PARAM, -1.f, 1.f, 1.f, "R2R 2 (Bits 3-6) Level");
        configParam(R2R_3_ATTN_PARAM, -1.f, 1.f, 1.f, "R2R 3 (Bits 5-8) Level");
        configParam(GATE_LENGTH_PARAM, 0.f, 1.f, 0.5f, "Gate Length", "%", 0.f, 100.f);
        configParam(DAC_ATTENUVERTER_PARAM, -1.f, 1.f, 1.f, "8-Bit DAC Level");  // <<< NEW

        configInput(CLOCK_INPUT, "Clock In (Overrides Internal)");
        configInput(DATA_INPUT, "Data In");
        configInput(XOR_INPUT, "XOR In");
        configInput(CLOCK_RATE_CV_INPUT, "Clock Rate CV");

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
        configOutput(CLOCK_OUTPUT, "Clock Out");
        configOutput(DAC_OUTPUT, "8-Bit DAC Out");  // <<< NEW

        // No need to explicitly configLight for enums

        onReset();  // Initialize state properly
    }

    // R2R Helper (unchanged)
    float calculateR2R(int startIndex, int numBits = 4) {
        float r2rValue = 0.0f;
        float weight = 1.0f;
        for (int i = 0; i < numBits; ++i) {
            int bitIndex = startIndex + i;
            if (bitIndex < NUM_STEPS && bits[bitIndex]) r2rValue += weight;
            weight *= 2.0f;
        }
        // Max value for 4 bits is 1+2+4+8 = 15
        return r2rValue * R2R_SCALE;
    }

    // --- NEW: 8-Bit DAC Calculation Helper ---
    float calculate8BitDACRaw() {
        float dacValue = 0.0f;
        float weight = 1.0f;  // Start with LSB weight
        for (int i = 0; i < NUM_STEPS; ++i) {
            if (bits[i]) {
                dacValue += weight;
            }
            weight *= 2.0f;  // Move to next bit's weight
        }
        // Raw value is 0 to 255
        return dacValue;
    }

    void onReset() override {
        std::fill(bits, bits + NUM_STEPS, false);
        std::fill(gateEndTime.begin(), gateEndTime.end(), 0.0);
        phase = 0.f;
        estimatedClockInterval = 0.25f;
        lastClockTime = -1.0;
        moduleElapsedTime = 0.0;
        writeIntention = false;
        eraseIntention = false;
        dataInputHighIntention = false;
        xorInputHighIntention = false;
    }

    void onRandomize() override {
        for (int i = 0; i < NUM_STEPS; ++i) {
            bits[i] = (random::uniform() > 0.5f);
            gateEndTime[i] = 0.0;  // Reset gate times on randomize
        }
        params[CLOCK_RATE_PARAM].setValue(random::uniform() * 8.f - 2.f);
        // Reset intentions on randomize too
        writeIntention = false;
        eraseIntention = false;
        dataInputHighIntention = false;
        xorInputHighIntention = false;
        // Randomize new attenuverter? Optional, maybe not desired.
        // params[DAC_ATTENUVERTER_PARAM].setValue(random::uniform() * 2.f - 1.f);
    }

    void process(const ProcessArgs& args) override {
        moduleElapsedTime += args.sampleTime;

        // --- Immediate Reset Button Logic ---
        if (resetTrigger.process(params[RESET_BUTTON_PARAM].getValue())) {
            std::fill(bits, bits + NUM_STEPS, false);
            std::fill(gateEndTime.begin(), gateEndTime.end(), 0.0);
            writeIntention = false;
            eraseIntention = false;
            dataInputHighIntention = false;
            xorInputHighIntention = false;
        }

        // --- Intention Processing (Detect rising edges / button presses since last check) ---
        // (Intention logic remains the same)
        if (writeTrigger.process(params[WRITE_BUTTON_PARAM].getValue())) {
            writeIntention = true;
            eraseIntention = false;
            dataInputHighIntention = false;
        }
        if (eraseTrigger.process(params[ERASE_BUTTON_PARAM].getValue())) {
            eraseIntention = true;
            writeIntention = false;
            dataInputHighIntention = false;
        }
        if (!writeIntention && !eraseIntention) {
            float dataVoltage = inputs[DATA_INPUT].isConnected() ? inputs[DATA_INPUT].getVoltage() : 0.f;
            if (dataInputTrigger.process(dataVoltage >= DATA_INPUT_THRESHOLD)) {
                dataInputHighIntention = true;
            }
        }
        float xorVoltage = inputs[XOR_INPUT].isConnected() ? inputs[XOR_INPUT].getVoltage() : 0.f;
        if (xorInputTrigger.process(xorVoltage >= DATA_INPUT_THRESHOLD)) {
            xorInputHighIntention = true;
        }

        // --- Clock Logic & Interval Estimation ---
        // (Clock logic remains the same)
        bool externalClockActive = inputs[CLOCK_INPUT].isConnected();
        bool clocked_this_frame = false;
        float currentClockFreq = 0.f;
        if (externalClockActive) {
            if (clockInputTrigger.process(inputs[CLOCK_INPUT].getVoltage())) {
                clocked_this_frame = true;
                phase = 0.f;
                if (lastClockTime >= 0.0) {
                    estimatedClockInterval = float(moduleElapsedTime - lastClockTime);
                }
                lastClockTime = moduleElapsedTime;
                clockOutputPulse.trigger(MIN_GATE_DURATION_SECONDS);
            }
        } else {
            float rateKnob = params[CLOCK_RATE_PARAM].getValue();
            float rateCV = inputs[CLOCK_RATE_CV_INPUT].isConnected() ? inputs[CLOCK_RATE_CV_INPUT].getVoltage() : 0.f;
            float rateAttn = params[CLOCK_RATE_CV_ATTENUVERTER_PARAM].getValue() / 10.f;
            float combinedRateExp = rateKnob + rateCV * rateAttn * 0.5f * 8.f;
            currentClockFreq = powf(2.f, combinedRateExp);
            currentClockFreq = clamp(currentClockFreq, 0.01f, 1000.f);
            phase += currentClockFreq * args.sampleTime;
            if (phase >= 1.f) {
                phase -= 1.f;
                clocked_this_frame = true;
                if (currentClockFreq > 1e-5) {
                    estimatedClockInterval = 1.0f / currentClockFreq;
                } else {
                    estimatedClockInterval = 1.0f;
                }
                lastClockTime = moduleElapsedTime;
                clockOutputPulse.trigger(MIN_GATE_DURATION_SECONDS);
            }
        }
        estimatedClockInterval = std::max(MIN_GATE_DURATION_SECONDS * 2.f, estimatedClockInterval);

        // Process clock output pulse generator
        float clockOutputVoltage = clockOutputPulse.process(args.sampleTime) ? GATE_VOLTAGE : 0.0f;
        outputs[CLOCK_OUTPUT].setVoltage(clockOutputVoltage);
        lights[CLOCK_LIGHT].setBrightness(clockOutputVoltage > 0.f ? 1.0f : 0.0f);

        // --- Shift Register Logic (Only execute on clock tick) ---
        // (Shift logic remains the same, including XOR)
        bool previousBits[NUM_STEPS];
        if (clocked_this_frame) {
            std::copy(bits, bits + NUM_STEPS, previousBits);

            bool dataBit = false;
            bool xorBit = false;
            bool nextBit = false;

            if (writeIntention) {
                dataBit = true;
            } else if (eraseIntention) {
                dataBit = false;
            } else if (dataInputHighIntention) {
                dataBit = true;
            } else {
                if (inputs[DATA_INPUT].isConnected()) {
                    dataBit = (inputs[DATA_INPUT].getVoltage() >= DATA_INPUT_THRESHOLD);
                } else {
                    dataBit = false;
                }
            }

            if (xorInputHighIntention) {
                xorBit = true;
            } else {
                if (inputs[XOR_INPUT].isConnected()) {
                    xorBit = (inputs[XOR_INPUT].getVoltage() >= DATA_INPUT_THRESHOLD);
                } else {
                    xorBit = false;
                }
            }

            nextBit = dataBit ^ xorBit;

            for (int i = NUM_STEPS - 1; i > 0; --i) {
                bits[i] = bits[i - 1];
            }
            bits[0] = nextBit;

            writeIntention = false;
            eraseIntention = false;
            dataInputHighIntention = false;
            xorInputHighIntention = false;

            // --- Update Gate Timer Logic --- (remains the same)
            float gateLengthRatio = params[GATE_LENGTH_PARAM].getValue();
            bool isMaxLength = (gateLengthRatio >= 0.99f);
            for (int i = 0; i < NUM_STEPS; ++i) {
                if (bits[i]) {
                    if (isMaxLength) {
                        gateEndTime[i] = moduleElapsedTime + estimatedClockInterval;
                    } else {
                        float gateDuration = std::max(MIN_GATE_DURATION_SECONDS, estimatedClockInterval * gateLengthRatio);
                        gateEndTime[i] = moduleElapsedTime + gateDuration;
                    }
                }
            }
        }  // End of clocked_this_frame

        // --- Update Individual Bit Outputs (Based on Gate Timers) & Lights ---
        // (Bit output and step light logic remains the same)
        float gateLengthRatioCheck = params[GATE_LENGTH_PARAM].getValue();
        bool isMaxLengthCheck = (gateLengthRatioCheck >= 0.99f);
        for (int i = 0; i < NUM_STEPS; ++i) {
            bool isGateOn = (moduleElapsedTime < gateEndTime[i]);
            if (bits[i] && isMaxLengthCheck) {
                isGateOn = true;
                gateEndTime[i] = std::max(gateEndTime[i], moduleElapsedTime + args.sampleTime * 2.0);
            }
            float bitVoltage = isGateOn ? GATE_VOLTAGE : 0.0f;
            int outputId = -1;
            switch (i) {
                case 0:
                    outputId = BIT_1_OUTPUT;
                    break;  // ... cases 1-6 ...
                case 1:
                    outputId = BIT_2_OUTPUT;
                    break;
                case 2:
                    outputId = BIT_3_OUTPUT;
                    break;
                case 3:
                    outputId = BIT_4_OUTPUT;
                    break;
                case 4:
                    outputId = BIT_5_OUTPUT;
                    break;
                case 5:
                    outputId = BIT_6_OUTPUT;
                    break;
                case 6:
                    outputId = BIT_7_OUTPUT;
                    break;
                case 7:
                    outputId = BIT_8_OUTPUT;
                    break;
            }
            if (outputId != -1) {
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

        // --- NEW: Calculate and Output 8-Bit DAC ---
        float dacRawValue = calculate8BitDACRaw();  // Value 0 to 255
        // Scale to -DAC_BIPOLAR_VOLTAGE to +DAC_BIPOLAR_VOLTAGE
        // (dacRawValue / 255.0) maps 0..255 to 0..1
        // * (2.0 * DAC_BIPOLAR_VOLTAGE) maps it to 0..(2 * V)
        // - DAC_BIPOLAR_VOLTAGE shifts it to -V..+V
        float dacBipolarValue = (dacRawValue / 255.0f) * (2.0f * DAC_BIPOLAR_VOLTAGE) - DAC_BIPOLAR_VOLTAGE;
        float dacAttn = params[DAC_ATTENUVERTER_PARAM].getValue();
        float finalDacOutput = dacBipolarValue * dacAttn;
        outputs[DAC_OUTPUT].setVoltage(finalDacOutput);

        // --- NEW: Update Button Press Light ---
        bool writePressed = params[WRITE_BUTTON_PARAM].getValue() > 0.f;
        bool erasePressed = params[ERASE_BUTTON_PARAM].getValue() > 0.f;
        bool resetPressed = params[RESET_BUTTON_PARAM].getValue() > 0.f;
        lights[BUTTON_PRESS_LIGHT].setBrightness((writePressed || erasePressed || resetPressed) ? 1.0f : 0.0f);
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();

        json_t* valuesJ = json_array();
        for (int i = 0; i < 8; i++) {
            json_array_insert_new(valuesJ, i, json_real(bits[i]));
        }
        json_object_set_new(rootJ, "values", valuesJ);

        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* valuesJ = json_object_get(rootJ, "values");
        if (valuesJ) {
            for (int i = 0; i < 8; i++) {
                json_t* valueJ = json_array_get(valuesJ, i);
                if (valueJ)
                    bits[i] = json_number_value(valueJ);
            }
        }
    }
};

// --- Module Widget (GUI) ---
struct CognitiveShiftWidget : ModuleWidget {
    CognitiveShiftWidget(CognitiveShift* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/CognitiveShift.svg"), asset::plugin(pluginInstance, "res/CognitiveShift-dark.svg")));  // Make sure panel exists

        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Panel layout constants (Keep existing ones)
        float col1 = 22.5f;
        float col2 = 67.5f;
        float col3 = 112.5f;
        float col4 = 157.5f;

        // Row 1: Clock Rate, CV, Attn, Output + Light (Keep position)

        // addChild(createLightCentered<MediumLight<GreenLight>>(Vec(col4, 18), module, CognitiveShift::CLOCK_LIGHT));

        // Row 2: Clock In, Gate Length (Keep position)

        addParam(createParamCentered<RoundBlackKnob>(Vec(col1, 53.5f), module, CognitiveShift::CLOCK_RATE_PARAM));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col2, 53.5f), module, CognitiveShift::CLOCK_RATE_CV_INPUT));
        addParam(createParamCentered<Trimpot>(Vec(col3, 53.5f), module, CognitiveShift::CLOCK_RATE_CV_ATTENUVERTER_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(col4, 53.5f), module, CognitiveShift::GATE_LENGTH_PARAM));

        // Row 3: Data In, XOR In, Write/Erase/Reset Buttons (Keep positions) + NEW Light
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col1, 153.5f), module, CognitiveShift::CLOCK_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col2, 153.5f), module, CognitiveShift::DATA_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(col3, 153.5f), module, CognitiveShift::XOR_INPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col4, 153.5f), module, CognitiveShift::CLOCK_OUTPUT));

        addChild(createLightCentered<LargeFresnelLight<RedLight>>(Vec(col1, 103.5f), module, CognitiveShift::BUTTON_PRESS_LIGHT));
        addParam(createParamCentered<VCVButton>(Vec(col2, 103.5f), module, CognitiveShift::WRITE_BUTTON_PARAM));
        addParam(createParamCentered<VCVButton>(Vec(col3, 103.5f), module, CognitiveShift::ERASE_BUTTON_PARAM));
        addParam(createParamCentered<VCVButton>(Vec(col4, 103.5f), module, CognitiveShift::RESET_BUTTON_PARAM));

        // Row 4 & 5: Step Lights (Keep positions)
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

        // Row 6: R2R Outputs (Keep positions)
        float r2r_out_y = 231.29;
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col1, r2r_out_y), module, CognitiveShift::R2R_1_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col2, r2r_out_y), module, CognitiveShift::R2R_2_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col3, r2r_out_y), module, CognitiveShift::R2R_3_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(col4, r2r_out_y), module, CognitiveShift::DAC_OUTPUT));

        // Row 7: R2R Attenuators (Keep positions)
        float r2r_attn_y = 203.81;
        addParam(createParamCentered<Trimpot>(Vec(col1, r2r_attn_y), module, CognitiveShift::R2R_1_ATTN_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col2, r2r_attn_y), module, CognitiveShift::R2R_2_ATTN_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col3, r2r_attn_y), module, CognitiveShift::R2R_3_ATTN_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(col4, r2r_attn_y), module, CognitiveShift::DAC_ATTENUVERTER_PARAM));

        // Row 8 & 9: Individual Bit Outputs (Keep positions)
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
};

// --- Plugin Registration --- (Make sure your plugin slug and model slug are correct)
Model* modelCognitiveShift = createModel<CognitiveShift, CognitiveShiftWidget>("CognitiveShift");  // Use a unique model slug