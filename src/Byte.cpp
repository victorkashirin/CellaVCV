#include "ByteBeatParser.hpp"
#include "components.hpp"
#include "plugin.hpp"

struct Byte : Module {
    enum ParamIds {
        FREQ_PARAM,
        RUN_PARAM,
        RESET_PARAM,
        A_PARAM,
        B_PARAM,
        C_PARAM,
        BIT_PARAM,
        A_CV_PARAM,
        B_CV_PARAM,
        C_CV_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        A_INPUT,
        B_INPUT,
        C_INPUT,
        RUN_INPUT,
        RESET_INPUT,
        CLOCK_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        OUT_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds {
        RUN_LIGHT,
        RESET_LIGHT,
        EDIT_LIGHT,
        ERROR_LIGHT,
        NUM_LIGHTS
    };

    std::string text;
    bool multiline = false;
    bool running = true;
    bool badInput = false;
    bool changed = false;
    uint32_t t = 0;
    float phase = 0.f;
    float output = 0.f;

    dsp::SchmittTrigger clockTrigger;
    float clockFreq = 1.f;
    dsp::Timer clockTimer;

    int outputLevelType = 0;
    float levels[4][2] = {
        {-2.5f, 2.5f},
        {-5.f, 5.f},
        {0.f, 5.f},
        {0.f, 10.f}};

    dsp::BooleanTrigger runButtonTrigger;
    dsp::BooleanTrigger resetButtonTrigger;

    dsp::SchmittTrigger runTrigger;
    dsp::SchmittTrigger resetTrigger;

    void onReset() override {
        text = "";
        t = 0;
        phase = 0.f;
        clockFreq = 2.f;
        clockTimer.reset();
    }

    std::string getStringWithoutSpacesAndNewlines(const std::string& str) {
        std::string result;
        result.reserve(str.size());  // Optional: Reserve space to improve performance
        std::copy_if(str.begin(), str.end(), std::back_inserter(result), [](char c) {
            return c != ' ' && c != '\n' && c != '\r';
        });
        return result;
    }

    void updateString(const std::string& newText) {
        std::string processed = getStringWithoutSpacesAndNewlines(newText);
        text = processed.c_str();
        // DEBUG("Module received text: %s", processed.c_str());
        badInput = false;
        changed = false;
    }

    Byte() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(A_INPUT, "Param <a> CV");
        configInput(B_INPUT, "Param <b> CV");
        configInput(C_INPUT, "Param <c> CV");

        configInput(CLOCK_INPUT, "Clock");
        configInput(RUN_INPUT, "Run");
        configInput(RESET_INPUT, "Reset");

        configButton(RUN_PARAM, "Run");
        configButton(RESET_PARAM, "Reset");

        struct FrequencyQuantity : ParamQuantity {
            float getDisplayValue() override {
                Byte* module = reinterpret_cast<Byte*>(this->module);
                if (module->clockFreq == 2.f) {
                    unit = " Hz";
                    displayMultiplier = 1.f;
                } else {
                    unit = "x";
                    displayMultiplier = 1 / 2.f;
                }
                return ParamQuantity::getDisplayValue();
            }
        };

        configParam<FrequencyQuantity>(FREQ_PARAM, 4.f, 14.428491035f, 12.965784f, "Frequency", " Hz", 2, 1);
        configParam(A_PARAM, 0.f, 128.f, 1.f, "Param <a>");
        configParam(B_PARAM, 0.f, 128.f, 1.f, "Param <b>");
        configParam(C_PARAM, 0.f, 128.f, 1.f, "Param <c>");
        configParam(BIT_PARAM, 1.f, 12.f, 8.f, "Bit Depth");
        paramQuantities[A_PARAM]->snapEnabled = true;
        paramQuantities[B_PARAM]->snapEnabled = true;
        paramQuantities[C_PARAM]->snapEnabled = true;
        paramQuantities[BIT_PARAM]->snapEnabled = true;

        configParam(A_CV_PARAM, 0.f, 1.f, 0.f, "Param <a> CV");
        configParam(B_CV_PARAM, 0.f, 1.f, 0.f, "Param <b> CV");
        configParam(C_CV_PARAM, 0.f, 1.f, 0.f, "Param <c> CV");

        configOutput(OUT_OUTPUT, "Audio");
    }

    int getReading(int paramIndex, int inputIndex, int paramCVIndex) {
        float maxValue = 128.f;
        float reading = params[paramIndex].getValue();
        if (inputs[inputIndex].isConnected()) {
            reading += params[paramCVIndex].getValue() * maxValue * inputs[inputIndex].getVoltage() / 10.f;
        }
        return (int)clamp(reading, 0.f, maxValue);
    }

    void process(const ProcessArgs& args) override {
        bool runButtonTriggered = runButtonTrigger.process(params[RUN_PARAM].getValue());
        bool runTriggered = runTrigger.process(inputs[RUN_INPUT].getVoltage(), 0.1f, 2.f);
        if (runButtonTriggered || runTriggered) {
            running ^= true;
        }

        bool resetButtonTriggered = resetButtonTrigger.process(params[RESET_PARAM].getValue());
        bool resetTriggered = resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 2.f);
        bool reset = (resetButtonTriggered || resetTriggered);

        // // Clock
        if (inputs[CLOCK_INPUT].isConnected()) {
            clockTimer.process(args.sampleTime);

            if (clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 2.f)) {
                float clockFreq = 1.f / clockTimer.getTime();
                clockTimer.reset();
                if (0.001f <= clockFreq && clockFreq <= 1000.f) {
                    this->clockFreq = clockFreq;
                }
            }
        } else {
            // Default frequency when clock is unpatched
            clockFreq = 2.f;
        }

        if (reset) {
            t = 0;
        }

        if (running) {
            float pitch = params[FREQ_PARAM].getValue();
            float freq = clockFreq / 2.f * dsp::exp2_taylor5(pitch);
            phase += args.sampleTime * freq;
            if (phase >= 1.f) {
                phase -= 1.f;
                t++;

                if (!text.empty() && !badInput) {
                    try {
                        int n = (int)params[BIT_PARAM].getValue();
                        int resolution = (1 << n) - 1;
                        BytebeatParser parser(text);
                        int a = getReading(A_PARAM, A_INPUT, A_CV_PARAM);
                        int b = getReading(B_PARAM, B_INPUT, B_CV_PARAM);
                        int c = getReading(C_PARAM, C_INPUT, C_CV_PARAM);
                        int res = parser.parseAndEvaluate(t, a, b, c);

                        res = res & resolution;
                        float out = res / (float)resolution;

                        float minV = levels[outputLevelType][0];
                        float maxV = levels[outputLevelType][1];

                        output = out * (maxV - minV) + minV;
                    } catch (const std::exception& e) {
                        badInput = true;
                        DEBUG("Exception caught: %s", e.what());
                    }
                } else {
                    output = 0.f;
                }
            }
        } else {
            output = 0.f;
        }
        outputs[OUT_OUTPUT].setVoltage(output);
        lights[RUN_LIGHT].setBrightness(running);
        lights[EDIT_LIGHT].setBrightness(changed * 0.2);
        lights[ERROR_LIGHT].setBrightness(badInput * 0.2);
        lights[RESET_LIGHT].setSmoothBrightness(reset, args.sampleTime);
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "text", json_stringn(text.c_str(), text.size()));
        json_object_set_new(rootJ, "outputLevelType", json_integer(outputLevelType));
        json_object_set_new(rootJ, "multiline", json_boolean(multiline));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* textJ = json_object_get(rootJ, "text");
        if (textJ)
            text = json_string_value(textJ);

        json_t* outputLevelTypeJ = json_object_get(rootJ, "outputLevelType");
        if (outputLevelTypeJ)
            outputLevelType = json_integer_value(outputLevelTypeJ);

        json_t* multilineJ = json_object_get(rootJ, "multiline");
        if (multilineJ)
            multiline = json_boolean_value(multilineJ);
    }
};

struct ByteTextField : LedDisplayTextField {
    Byte* module;

    void step() override {
        LedDisplayTextField::step();
        if (module) {
            multiline = module->multiline;
        }
    }

    void onChange(const ChangeEvent& e) override {
        if (module) {
            module->changed = true;
            module->badInput = false;
        }
    }

    // Capture keyboard events for Enter key
    void onSelectKey(const SelectKeyEvent& e) override {
        if (e.action == GLFW_PRESS && (e.key == GLFW_KEY_ENTER || e.key == GLFW_KEY_KP_ENTER)) {
            if (module && module->multiline) {
                bool shift_key = ((e.mods & RACK_MOD_MASK) & GLFW_MOD_SHIFT);
                if (shift_key) {
                    onSubmit();
                    e.consume(this);
                }
            } else {
                onSubmit();
                e.consume(this);
            }
        }

        if (!e.getTarget())
            TextField::onSelectKey(e);
    }

    void onSubmit() {
        std::string enteredText = getText();

        if (module)
            module->updateString(enteredText);

        // DEBUG("Enter pressed! Submitted text: %s", enteredText.c_str());
    }
};

struct ByteDisplay : LedDisplay {
    void setModule(Byte* module) {
        ByteTextField* textField = createWidget<ByteTextField>(Vec(0, 0));
        textField->fontPath = asset::plugin(pluginInstance, "res/fonts/JetBrainsMono-Medium.ttf");
        textField->box.size = box.size;

        if (module) {
            textField->module = module;
            textField->multiline = module->multiline;
            textField->setText(module->text);
        }
        addChild(textField);
    }
};

struct ByteWidget : ModuleWidget {
    ByteWidget(Byte* module) {
        setModule(module);

        setPanel(createPanel(asset::plugin(pluginInstance, "res/Byte.svg"), asset::plugin(pluginInstance, "res/Byte-dark.svg")));

        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        ByteDisplay* byteDisplay = createWidget<ByteDisplay>(Vec(0, 26));
        byteDisplay->box.size = Vec(225, 150);
        byteDisplay->setModule(module);
        addChild(byteDisplay);

        if (module) module->changed = false;

        addChild(createLightCentered<LargeFresnelLight<YellowLight>>(Vec(157.5, 329.25), module, Byte::EDIT_LIGHT));
        addChild(createLightCentered<LargeFresnelLight<RedLight>>(Vec(67.5, 329.25), module, Byte::ERROR_LIGHT));

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(22.5, 329.25), module, Byte::CLOCK_INPUT));

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(22.5, 280.01), module, Byte::RUN_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(67.5, 280.01), module, Byte::A_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(112.5, 280.01), module, Byte::B_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(157.5, 280.01), module, Byte::C_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(202.5, 280.01), module, Byte::RESET_INPUT));

        addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 203.79), module, Byte::FREQ_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 203.79), module, Byte::A_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(112.5, 203.79), module, Byte::B_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(157.5, 203.79), module, Byte::C_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(202.5, 203.79), module, Byte::BIT_PARAM));

        addParam(createParamCentered<Trimpot>(Vec(67.5, 252.5), module, Byte::A_CV_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(112.5, 252.5), module, Byte::B_CV_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(157.5, 252.5), module, Byte::C_CV_PARAM));

        addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<WhiteLight>>>(Vec(22.5, 252.5), module, Byte::RUN_PARAM, Byte::RUN_LIGHT));
        addParam(createLightParamCentered<VCVLightBezel<WhiteLight>>(Vec(202.5, 252.5), module, Byte::RESET_PARAM, Byte::RESET_LIGHT));

        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(202.5, 329.25), module, Byte::OUT_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        Byte* module = dynamic_cast<Byte*>(this->module);
        assert(module);
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexPtrSubmenuItem("Output Range",
                                                 {"-2.5V..2.5V", "-5V..5V", "0..5V", "0..10V"},
                                                 &module->outputLevelType));

        menu->addChild(new MenuSeparator);
        menu->addChild(createBoolPtrMenuItem("Multiline", "", &module->multiline));
    }
};

Model* modelByte = createModel<Byte, ByteWidget>("Byte");

// Recipes
// (t % 163 > 100 ? t : t>>3 + t<<14)|(t>>4)  at clock 90

// t<<2|(t<<(2<<t)+2)>>(t>>3)|t>>2 at cloock 563

// 2<<t+a|t<<1|t<<b|a<<b>>t at clock 85