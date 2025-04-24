#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "LoudnessMeterEngine.hpp"
#include "components.hpp"
#include "plugin.hpp"

using namespace rack;

struct LoudnessWidget;  // Forward declaration

// Generic number formatter (handles LUFS, LU, dB)
static std::string formatValue(float value) {
    if (value <= ALMOST_NEGATIVE_INFINITY || std::isinf(value) || std::isnan(value)) {
        return "-inf";
    }
    char buf[25];
    snprintf(buf, sizeof(buf), "%.1f", value);
    return std::string(buf);
}

struct ValueDisplaySmallWidget : TransparentWidget {
    std::shared_ptr<Font> font;
    std::shared_ptr<Font> font2;
    NVGcolor valueColor = nvgRGB(0xf5, 0xf5, 0xdc);
    // NVGcolor labelColor = nvgRGB(0x1a, 0xa7, 0xff);
    NVGcolor labelColor = nvgRGB(95, 190, 250);
    // NVGcolor labelColor = nvgRGB(121, 202, 252);
    NVGcolor redColor = nvgRGB(0xc0, 0x39, 0x2b);
    std::string label;
    float* valuePtr = nullptr;
    float* maxValuePtr = nullptr;
    std::string unit = "";

    ValueDisplaySmallWidget() {
        font = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/JetBrainsMono-Medium.ttf"));
        font2 = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/SofiaSansExtraCondensed-Regular.ttf"));
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1)
            return;
        if (!font || !font2) return;

        float middleY = box.size.y - 11.f;

        // --- Determine Value String ---
        std::string valueText = "-inf";
        bool drawDash = false;
        bool clipping = false;

        if (valuePtr) {
            float currentValue = *valuePtr;
            bool cond1 = currentValue <= ALMOST_NEGATIVE_INFINITY || std::isinf(currentValue) || std::isnan(currentValue);
            bool cond2 = (label == "LR") && currentValue <= 0.0f;
            bool cond3 = (label == "TPMAX") && currentValue >= -0.5f;
            bool cond4 = maxValuePtr && !std::isnan(currentValue) && (label == "M") && currentValue >= *maxValuePtr;

            if (cond1 || cond2) {
                drawDash = true;
            } else {
                // Format the valid number
                valueText = formatValue(currentValue);
                if (cond3 | cond4) {
                    clipping = true;
                }
            }
        } else {
            drawDash = true;
            valueText = "";
        }

        // --- Draw Value ---
        nvgFontFaceId(args.vg, font->handle);
        if (drawDash) {
            nvgStrokeColor(args.vg, valueColor);
            nvgStrokeWidth(args.vg, 1.9f);
            nvgBeginPath(args.vg);
            nvgMoveTo(args.vg, box.size.x - 55, middleY - 6.3f);
            nvgLineTo(args.vg, box.size.x - 70, middleY - 6.3f);
            nvgStroke(args.vg);
        } else {
            nvgFontSize(args.vg, 25);
            nvgFillColor(args.vg, clipping ? redColor : valueColor);
            nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE);
            nvgText(args.vg, box.size.x - 35, middleY, valueText.c_str(), NULL);
        }

        // --- Draw Unit ---
        nvgFontSize(args.vg, 14);
        nvgFillColor(args.vg, valueColor);
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
        nvgText(args.vg, box.size.x - 32.f, middleY, unit.c_str(), NULL);

        // --- Draw Label ---
        nvgFontFaceId(args.vg, font2->handle);
        nvgFontSize(args.vg, 21);
        nvgFillColor(args.vg, labelColor);
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);
        nvgText(args.vg, 23, middleY, label.c_str(), NULL);
    }
};

struct TargetQuantity : Quantity {
    LoudnessMeter* _module;

    TargetQuantity(LoudnessMeter* m) : _module(m) {}

    void setValue(float value) override {
        value = clamp(value, getMinValue(), getMaxValue());
        if (_module) {
            _module->params[1].setValue(std::ceil(value * 10.0) / 10.0);
        }
    }

    float getValue() override {
        if (_module) {
            return _module->params[1].getValue();
        }
        return getDefaultValue();
    }

    float getMinValue() override { return -36.0f; }
    float getMaxValue() override { return 0.0f; }
    float getDefaultValue() override { return LoudnessMeter::defaultTarget; }
    std::string getLabel() override { return "Target loudness"; }
    std::string getUnit() override { return " LUFS"; }
};

template <class Q>
struct LoudnessSlider : ui::Slider {
    LoudnessSlider(LoudnessMeter* module) {
        quantity = new Q(module);
        box.size.x = 200.0f;
    }
    virtual ~LoudnessSlider() {
        delete quantity;
    }
};

template <class Q>
struct LoudnessSliderMenuItem : MenuItem {
    LoudnessMeter* _module;

    LoudnessSliderMenuItem(LoudnessMeter* m, const char* label) : _module(m) {
        this->text = label;
        this->rightText = "▸";
    }

    Menu* createChildMenu() override {
        Menu* menu = new Menu;
        menu->addChild(new LoudnessSlider<Q>(_module));
        return menu;
    }
};

struct LoudnessWidget : ModuleWidget {
    LoudnessWidget(LoudnessMeter* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Loudness.svg"), asset::plugin(pluginInstance, "res/Loudness-dark.svg")));

        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        LedDisplay* ledDisplay = createWidget<LedDisplay>(Vec(0, 26));
        ledDisplay->box.size = Vec(135, 280);
        addChild(ledDisplay);

        float displayHeightPx = 30.35f;
        float yStep = displayHeightPx;
        float yStart = 306.f;
        float displayX_Px = 0.f;
        float displayWidthPx = 135.f;
        float inputYPx = 329.28;

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(22.5f, inputYPx), module, LoudnessMeter::AUDIO_INPUT_L));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(67.5f, inputYPx), module, LoudnessMeter::AUDIO_INPUT_R));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(112.5f, inputYPx), module, LoudnessMeter::RESET_INPUT));
        addParam(createParamCentered<VCVButton>(Vec(121.5f, 12.5f), module, LoudnessMeter::RESET_PARAM));

        float off = 3.f;

        ValueDisplaySmallWidget* momentaryDisplay = createWidget<ValueDisplaySmallWidget>(Vec(displayX_Px, yStart - 9 * yStep));
        momentaryDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            momentaryDisplay->valuePtr = &module->momentaryLufs;
            momentaryDisplay->maxValuePtr = &module->targetLoudness;
        }
        momentaryDisplay->label = "M";
        momentaryDisplay->unit = "LUFS";
        addChild(momentaryDisplay);

        ValueDisplaySmallWidget* shortTermDisplay = createWidget<ValueDisplaySmallWidget>(Vec(displayX_Px, yStart - 8 * yStep - off));
        shortTermDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            shortTermDisplay->valuePtr = &module->shortTermLufs;
        }
        shortTermDisplay->label = "S";
        shortTermDisplay->unit = "LUFS";
        addChild(shortTermDisplay);

        ValueDisplaySmallWidget* integratedDisplay = createWidget<ValueDisplaySmallWidget>(Vec(displayX_Px, yStart - 7 * yStep - 2 * off));
        integratedDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            integratedDisplay->valuePtr = &module->integratedLufs;
        }
        integratedDisplay->label = "I";
        integratedDisplay->unit = "LUFS";
        addChild(integratedDisplay);

        ValueDisplaySmallWidget* lraDisplay = createWidget<ValueDisplaySmallWidget>(Vec(displayX_Px, yStart - 6 * yStep + off));
        lraDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            lraDisplay->valuePtr = &module->loudnessRange;
        }
        lraDisplay->label = "LR";
        lraDisplay->unit = "LU";
        addChild(lraDisplay);

        ValueDisplaySmallWidget* psrDisplay = createWidget<ValueDisplaySmallWidget>(Vec(displayX_Px, yStart - 5 * yStep));
        psrDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            psrDisplay->valuePtr = &module->psrValue;
        }
        psrDisplay->label = "PSR";
        psrDisplay->unit = "LU";
        addChild(psrDisplay);

        ValueDisplaySmallWidget* plrDisplay = createWidget<ValueDisplaySmallWidget>(Vec(displayX_Px, yStart - 4 * yStep - off));
        plrDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            plrDisplay->valuePtr = &module->plrValue;
        }
        plrDisplay->label = "PLR";
        plrDisplay->unit = "LU";
        addChild(plrDisplay);

        ValueDisplaySmallWidget* mMaxDisplay = createWidget<ValueDisplaySmallWidget>(Vec(displayX_Px, yStart - 3 * yStep + 2 * off));
        mMaxDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            mMaxDisplay->valuePtr = &module->maxMomentaryLufs;
        }
        mMaxDisplay->label = "MMAX";
        mMaxDisplay->unit = "LUFS";
        addChild(mMaxDisplay);

        ValueDisplaySmallWidget* sMaxDisplay = createWidget<ValueDisplaySmallWidget>(Vec(displayX_Px, yStart - 2 * yStep + off));
        sMaxDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            sMaxDisplay->valuePtr = &module->maxShortTermLufs;
        }
        sMaxDisplay->label = "SMAX";
        sMaxDisplay->unit = "LUFS";
        addChild(sMaxDisplay);

        ValueDisplaySmallWidget* tpmDisplay = createWidget<ValueDisplaySmallWidget>(Vec(displayX_Px, yStart - yStep));
        tpmDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            tpmDisplay->valuePtr = &module->truePeakMax;
        }
        tpmDisplay->label = "TPMAX";
        tpmDisplay->unit = "dBTP";
        addChild(tpmDisplay);
    }

    void appendContextMenu(Menu* menu) override {
        LoudnessMeter* module = dynamic_cast<LoudnessMeter*>(this->module);
        assert(module);
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Settings"));
        menu->addChild(createIndexPtrSubmenuItem("Processing mode",
                                                 {"Auto",
                                                  "Mono",
                                                  "Stereo"},
                                                 &module->processingMode));
        menu->addChild(createIndexPtrSubmenuItem("Short-Term loudness",
                                                 {"Disabled",
                                                  "Enabled"},
                                                 &module->shortTermEnabled));
        menu->addChild(new LoudnessSliderMenuItem<TargetQuantity>(module, "Target loudness"));
    }
};

Model* modelLoudness = createModel<LoudnessMeter, LoudnessWidget>("Loudness");