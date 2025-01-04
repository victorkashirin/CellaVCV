#include "ByteBeatParser.hpp"
#include "components.hpp"
#include "plugin.hpp"

struct Byte : Module {
    enum ParamIds {
        CLOCK_PARAM,
        RUN_PARAM,
        RESET_PARAM,
        A_PARAM,
        B_PARAM,
        C_PARAM,
        A_CV_PARAM,
        B_CV_PARAM,
        C_CV_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        T_INPUT,
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
    bool multiline = true;
    bool running = true;
    bool dirty = false;
    bool badInput = false;
    bool changed = false;
    uint32_t t = 0;
    float output = 0.f;
    int clockDivision = 4;

    int outputLevelType = 0;
    float levels[4][2] = {
        {-2.5f, 2.5f},
        {-5.f, 5.f},
        {0.f, 5.f},
        {0.f, 10.f}};

    dsp::ClockDivider clock;

    dsp::BooleanTrigger runButtonTrigger;
    dsp::BooleanTrigger resetButtonTrigger;

    dsp::SchmittTrigger runTrigger;
    dsp::SchmittTrigger resetTrigger;

    void onReset() override {
        text = "";
        dirty = true;
        t = 0;
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
        DEBUG("Module received text: %s", processed.c_str());
        // t = 0;
        badInput = false;
        changed = false;
    }

    Byte() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(T_INPUT, "Time");
        configInput(A_INPUT, "Param <a> CV");
        configInput(B_INPUT, "Param <b> CV");
        configInput(C_INPUT, "Param <c> CV");

        configInput(CLOCK_INPUT, "Clock");
        configInput(RUN_INPUT, "Run");
        configInput(RESET_INPUT, "Reset");

        configButton(RUN_PARAM, "Run");
        configButton(RESET_PARAM, "Reset");

        configParam(CLOCK_PARAM, 2.f, 2000.f, 4.f, "Clock division");
        paramQuantities[CLOCK_PARAM]->snapEnabled = true;
        configParam(A_PARAM, 0.f, 128.f, 64.f, "Param <a>");
        configParam(B_PARAM, 0.f, 128.f, 64.f, "Param <b>");
        configParam(C_PARAM, 0.f, 128.f, 64.f, "Param <c>");
        paramQuantities[A_PARAM]->snapEnabled = true;
        paramQuantities[B_PARAM]->snapEnabled = true;
        paramQuantities[C_PARAM]->snapEnabled = true;

        configParam(A_CV_PARAM, 0.f, 1.f, 0.f, "Param <a> CV");
        configParam(B_CV_PARAM, 0.f, 1.f, 0.f, "Param <b> CV");
        configParam(C_CV_PARAM, 0.f, 1.f, 0.f, "Param <c> CV");

        configOutput(OUT_OUTPUT, "Audio");
        clock.setDivision(clockDivision);
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

        if (reset) {
            t = 0;
        }

        if (running && clock.process()) {
            t++;

            if (clockDivision != (int)params[CLOCK_PARAM].getValue()) {
                clockDivision = (int)params[CLOCK_PARAM].getValue();
                clock.setDivision(clockDivision);
            }

            if (!text.empty() && !badInput) {
                try {
                    BytebeatParser parser(text);
                    int a = (int)params[A_PARAM].getValue();
                    int b = (int)params[B_PARAM].getValue();
                    int c = (int)params[C_PARAM].getValue();
                    int res = parser.parseAndEvaluate(t, a, b, c);
                    res = res & 0xff;
                    float out = res / 255.f;

                    float minV = levels[outputLevelType][0];
                    float maxV = levels[outputLevelType][1];

                    output = out * (maxV - minV) + minV;

                    // output = out * 5.f - 2.5f;
                    // badInput = false;
                    // output = out * 5.f;
                } catch (const std::exception& e) {
                    badInput = true;
                    DEBUG("Exception caught: %s", e.what());
                }
            } else {
                output = 0.f;
            }
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
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* textJ = json_object_get(rootJ, "text");
        if (textJ)
            text = json_string_value(textJ);

        json_t* outputLevelTypeJ = json_object_get(rootJ, "outputLevelType");
        if (outputLevelTypeJ)
            outputLevelType = json_integer_value(outputLevelTypeJ);

        dirty = true;
    }
};

struct ByteTextField : LedDisplayTextField {
    Byte* module;

    void step() override {
        LedDisplayTextField::step();
        multiline = module->multiline;
        if (module && module->dirty) {
            setText(module->text);
            module->dirty = false;
        }
    }

    void onChange(const ChangeEvent& e) override {
        if (module) {
            module->changed = true;
            module->badInput = false;
        }
    }

    // Capture keyboard events for copy-paste and Enter key
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

    // Custom action to handle Enter keypress
    void onSubmit() {
        // Define what happens when Enter is pressed, e.g. submit the text
        std::string enteredText = getText();
        if (!enteredText.empty()) {
            if (module)
                module->updateString(enteredText);

            // You can trigger some event or handle the entered text
            // For example, print it to the Rack log
            DEBUG("Enter pressed! Submitted text: %s", enteredText.c_str());
        }
    }
};

struct ByteDisplay : LedDisplay {
    void setModule(Byte* module) {
        ByteTextField* textField = createWidget<ByteTextField>(Vec(0, 0));
        textField->fontPath = asset::plugin(pluginInstance, "res/fonts/JetBrainsMono-Medium.ttf");
        textField->box.size = box.size;
        textField->multiline = module->multiline;
        textField->module = module;
        addChild(textField);
    }
};

struct ByteWidget : ModuleWidget {
    ByteWidget(Byte* module) {
        setModule(module);

        setPanel(createPanel(asset::plugin(pluginInstance, "res/Byte.svg"), asset::plugin(pluginInstance, "res/Byte-dark.svg")));

        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        ByteDisplay* notesDisplay = createWidget<ByteDisplay>(Vec(0, 26));
        notesDisplay->box.size = Vec(225, 150);
        notesDisplay->setModule(module);
        addChild(notesDisplay);

        addChild(createLightCentered<LargeFresnelLight<YellowLight>>(Vec(112.5, 329.25), module, Byte::EDIT_LIGHT));
        addChild(createLightCentered<LargeFresnelLight<RedLight>>(Vec(67.5, 329.25), module, Byte::ERROR_LIGHT));

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(22.5, 329.25), module, Byte::CLOCK_INPUT));

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(22.5, 280.01), module, Byte::RUN_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(67.5, 280.01), module, Byte::A_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(112.5, 280.01), module, Byte::B_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(157.5, 280.01), module, Byte::C_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(202.5, 280.01), module, Byte::RESET_INPUT));

        addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 203.79), module, Byte::CLOCK_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 203.79), module, Byte::A_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(112.5, 203.79), module, Byte::B_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(157.5, 203.79), module, Byte::C_PARAM));

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