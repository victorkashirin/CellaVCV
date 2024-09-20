#include "plugin.hpp"

struct Byte : Module {
    std::string text;
    bool dirty = false;

    void onReset() override {
        text = "";
        dirty = true;
    }

    void updateString(const std::string& newText) {
        DEBUG("Module received text: %s", newText.c_str());
    }

    void
    fromJson(json_t* rootJ) override {
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
        if (module)
            module->text = getText();
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
        // setPanel(createPanel(asset::plugin(pluginInstance, "res/Byte.svg"), asset::plugin(pluginInstance, "res/Byte.svg")));
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Byte.svg")));

        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        ByteDisplay* notesDisplay = createWidget<ByteDisplay>(mm2px(Vec(0.0, 12.869)));
        notesDisplay->box.size = mm2px(Vec(81.28, 105.059));
        notesDisplay->setModule(module);
        addChild(notesDisplay);
    }
};

Model* modelByte = createModel<Byte, ByteWidget>("Byte");
