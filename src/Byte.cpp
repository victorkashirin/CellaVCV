#include "ByteBeatParser.hpp"
#include "components.hpp"
#include "plugin.hpp"

struct Byte : Module {
    enum ParamIds {
        CLOCK_PARAM,
        A_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        T_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        OUT_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds {
        NUM_LIGHTS
    };

    std::string text;
    bool dirty = false;
    uint32_t t = 0;
    float output = 0.f;
    int clockDivision = 4;

    dsp::ClockDivider clock;

    void onReset() override {
        text = "";
        dirty = true;
    }

    void updateString(const std::string& newText) {
        DEBUG("Module received text: %s", newText.c_str());
        text = newText.c_str();
    }

    Byte() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(T_INPUT, "Time");

        configParam(CLOCK_PARAM, 2.f, 2000.f, 4.f, "Clock division");
        paramQuantities[CLOCK_PARAM]->snapEnabled = true;
        configParam(A_PARAM, 0.f, 128.f, 64.f, "Param <a>");
        paramQuantities[A_PARAM]->snapEnabled = true;

        configOutput(OUT_OUTPUT, "Audio");
        clock.setDivision(clockDivision);
    }

    void process(const ProcessArgs& args) override {
        if (clock.process()) {
            t++;

            if (clockDivision != (int)params[CLOCK_PARAM].getValue()) {
                clockDivision = (int)params[CLOCK_PARAM].getValue();
                clock.setDivision(clockDivision);
            }

            if (!text.empty()) {
                try {
                    BytebeatParser parser(text);
                    int a = (int)params[A_PARAM].getValue();
                    int res = parser.parseAndEvaluate(t, a);
                    res = res & 0xff;
                    float out = res / 255.f;
                    output = out * 5.f - 2.5f;
                } catch (const std::exception& e) {
                    DEBUG("Exception caught: %s", e.what());
                }

                // uint8_t res = ((t >> 10) & 42) * t;
                // float out = res / 255.f;
                // output = out * 5.f - 2.5f;

            } else {
                output = 0.f;
            }
        }
        outputs[OUT_OUTPUT].setVoltage(output);
    }
    void fromJson(json_t* rootJ) override {
        Module::fromJson(rootJ);
        // In <1.0, module used "text" property at root level.
        json_t* textJ = json_object_get(rootJ, "text");
        if (textJ)
            text = json_string_value(textJ);
        dirty = true;
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "text", json_stringn(text.c_str(), text.size()));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* textJ = json_object_get(rootJ, "text");
        if (textJ)
            text = json_string_value(textJ);
        dirty = true;
    }
};

struct ByteTextField : LedDisplayTextField {
    Byte* module;

    void step() override {
        LedDisplayTextField::step();
        if (module && module->dirty) {
            setText(module->text);
            module->dirty = false;
        }
    }

    void onChange(const ChangeEvent& e) override {
        // if (module)
        // module->text = getText();
    }

    // Capture keyboard events for copy-paste and Enter key
    void onSelectKey(const SelectKeyEvent& e) override {
        if (e.action == GLFW_PRESS && (e.key == GLFW_KEY_ENTER || e.key == GLFW_KEY_KP_ENTER)) {
            onEnterPressed();
            e.consume(this);
        }

        if (!e.getTarget())
            TextField::onSelectKey(e);
    }

    // Custom action to handle Enter keypress
    void onEnterPressed() {
        // Define what happens when Enter is pressed, e.g. submit the text
        std::string enteredText = getText();
        if (!enteredText.empty()) {
            if (module)
                module->updateString(enteredText);

            // You can trigger some event or handle the entered text
            // For example, print it to the Rack log
            DEBUG("Enter pressed! Submitted text: %s", enteredText.c_str());

            // Optionally clear the text field after submission
            // setText("");
        }
    }
};

struct ByteDisplay : LedDisplay {
    void setModule(Byte* module) {
        ByteTextField* textField = createWidget<ByteTextField>(Vec(0, 0));
        textField->fontPath = asset::plugin(pluginInstance, "res/fonts/JetBrainsMono-Medium.ttf");
        textField->box.size = box.size;
        textField->multiline = false;
        textField->module = module;
        addChild(textField);
    }
};

struct ByteWidget : ModuleWidget {
    ByteWidget(Byte* module) {
        setModule(module);

        setPanel(createPanel(asset::plugin(pluginInstance, "res/Byte.svg")));

        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        ByteDisplay* notesDisplay = createWidget<ByteDisplay>(Vec(0, 26));
        notesDisplay->box.size = Vec(240, 200);
        notesDisplay->setModule(module);
        addChild(notesDisplay);

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(30, 329.25), module, Byte::OUT_OUTPUT));

        addParam(createParamCentered<RoundBlackKnob>(Vec(30, 280), module, Byte::CLOCK_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(75, 280), module, Byte::A_PARAM));

        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(210, 329.25), module, Byte::OUT_OUTPUT));
    }
};

Model* modelByte = createModel<Byte, ByteWidget>("Byte");
