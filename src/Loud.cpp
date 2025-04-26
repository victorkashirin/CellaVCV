#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "LoudnessMeterEngine.hpp"
#include "components.hpp"
#include "plugin.hpp"

using namespace rack;

struct LoudWidget;  // Forward declaration

// Generic number formatter (handles LUFS, LU, dB)
static std::string formatValue(float value) {
    if (value <= ALMOST_NEGATIVE_INFINITY || std::isinf(value) || std::isnan(value)) {
        return "-inf";
    }
    char buf[25];
    snprintf(buf, sizeof(buf), "%.1f", value);
    return std::string(buf);
}

struct ValueDisplayTinyWidget : Widget {
    std::shared_ptr<Font> font;
    std::shared_ptr<Font> font2;
    NVGcolor valueColor = nvgRGB(0xf5, 0xf5, 0xdc);
    NVGcolor labelColor = nvgRGB(95, 190, 250);
    NVGcolor redColor = nvgRGB(0xc0, 0x39, 0x2b);
    std::string label;
    float* valuePtr = nullptr;
    float* maxValuePtr = nullptr;
    std::string unit = "";

    Tooltip* tooltip = nullptr;  // Pointer to the active tooltip

    void cleanupTooltip() {
        if (tooltip) {
            tooltip->requestDelete();
            tooltip = nullptr;
        }
    }

    ValueDisplayTinyWidget() {
        font = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/JetBrainsMono-Medium.ttf"));
        font2 = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/SofiaSansExtraCondensed-Regular.ttf"));
    }

    // --- Destructor for Cleanup ---
    ~ValueDisplayTinyWidget() override {
        cleanupTooltip();
    }

    void onEnter(const EnterEvent& e) override {
        Widget::onEnter(e);
    }

    void onHover(const event::Hover& e) override {
        Widget::onHover(e);
        if (e.isPropagating() && !unit.empty()) {
            e.consume(this);
            if (tooltip) {
                tooltip->setPosition(e.pos.plus(math::Vec(10, 15)));
            } else {
                tooltip = new Tooltip;
                tooltip->text = unit;
                tooltip->setPosition(e.pos.plus(math::Vec(10, 15)));
                APP->scene->addChild(tooltip);
            }
        }
    }

    void onLeave(const event::Leave& e) override {
        Widget::onLeave(e);
        cleanupTooltip();
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1)
            return;
        if (!font || !font2) return;

        float middleY = box.size.y - 9.f;

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
            nvgStrokeWidth(args.vg, 1.53f);
            nvgBeginPath(args.vg);
            nvgMoveTo(args.vg, box.size.x - 50, middleY - 5.2f);
            nvgLineTo(args.vg, box.size.x - 70, middleY - 5.2f);
            nvgStroke(args.vg);
        } else {
            nvgFontSize(args.vg, 21);
            nvgFillColor(args.vg, clipping ? redColor : valueColor);
            nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE);
            nvgText(args.vg, box.size.x - 37, middleY, valueText.c_str(), NULL);
        }

        // --- Draw Label ---
        nvgFontFaceId(args.vg, font2->handle);
        nvgFontSize(args.vg, 16);
        nvgFillColor(args.vg, labelColor);
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);
        nvgText(args.vg, box.size.x - 19.f, middleY - 1, label.c_str(), NULL);

        // nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
        // nvgText(args.vg, box.size.x - 30.f, middleY - 1, label.c_str(), NULL);
    }
};

struct LoudWidget : ModuleWidget {
    LoudWidget(LoudnessMeter* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Loud.svg"), asset::plugin(pluginInstance, "res/Loud-dark.svg")));

        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        LedDisplay* ledDisplay = createWidget<LedDisplay>(Vec(0, 26));
        ledDisplay->box.size = Vec(90, 230);
        addChild(ledDisplay);

        float displayHeightPx = 25.f;
        float yStep = displayHeightPx;
        float yStart = 256.f;
        float displayX_Px = 0.f;
        float displayWidthPx = 90.f;
        float inputYPx = 329.28;
        float inputYPx2 = 280.1f;

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(22.5f, inputYPx), module, LoudnessMeter::AUDIO_INPUT_L));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(67.5f, inputYPx), module, LoudnessMeter::AUDIO_INPUT_R));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(22.5f, inputYPx2), module, LoudnessMeter::RESET_INPUT));
        addParam(createParamCentered<VCVButton>(Vec(67.5f, inputYPx2), module, LoudnessMeter::RESET_PARAM));

        float off = 2.f;

        ValueDisplayTinyWidget* momentaryDisplay = createWidget<ValueDisplayTinyWidget>(Vec(displayX_Px, yStart - 9 * yStep));
        momentaryDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            momentaryDisplay->valuePtr = &module->momentaryLufs;
            momentaryDisplay->maxValuePtr = &module->targetLoudness;
        }
        momentaryDisplay->label = "M";
        momentaryDisplay->unit = "Momentary, LUFS";
        addChild(momentaryDisplay);

        ValueDisplayTinyWidget* shortTermDisplay = createWidget<ValueDisplayTinyWidget>(Vec(displayX_Px, yStart - 8 * yStep - off));
        shortTermDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            shortTermDisplay->valuePtr = &module->shortTermLufs;
        }
        shortTermDisplay->label = "S";
        shortTermDisplay->unit = "Short-term, LUFS";
        addChild(shortTermDisplay);

        ValueDisplayTinyWidget* integratedDisplay = createWidget<ValueDisplayTinyWidget>(Vec(displayX_Px, yStart - 7 * yStep - 2 * off));
        integratedDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            integratedDisplay->valuePtr = &module->integratedLufs;
        }
        integratedDisplay->label = "I";
        integratedDisplay->unit = "Integrated, LUFS";
        addChild(integratedDisplay);

        ValueDisplayTinyWidget* lraDisplay = createWidget<ValueDisplayTinyWidget>(Vec(displayX_Px, yStart - 6 * yStep + off));
        lraDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            lraDisplay->valuePtr = &module->loudnessRange;
        }
        lraDisplay->label = "LR";
        lraDisplay->unit = "Loudness range, LU";
        addChild(lraDisplay);

        ValueDisplayTinyWidget* psrDisplay = createWidget<ValueDisplayTinyWidget>(Vec(displayX_Px, yStart - 5 * yStep));
        psrDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            psrDisplay->valuePtr = &module->psrValue;
        }
        psrDisplay->label = "PSR";
        psrDisplay->unit = "Dynamics, LU";
        addChild(psrDisplay);

        ValueDisplayTinyWidget* plrDisplay = createWidget<ValueDisplayTinyWidget>(Vec(displayX_Px, yStart - 4 * yStep - off));
        plrDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            plrDisplay->valuePtr = &module->plrValue;
        }
        plrDisplay->label = "PLR";
        plrDisplay->unit = "Average dynamics, LU";
        addChild(plrDisplay);

        ValueDisplayTinyWidget* mMaxDisplay = createWidget<ValueDisplayTinyWidget>(Vec(displayX_Px, yStart - 3 * yStep + 2 * off));
        mMaxDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            mMaxDisplay->valuePtr = &module->maxMomentaryLufs;
        }
        mMaxDisplay->label = "MMAX";
        mMaxDisplay->unit = "Momentary max, LUFS";
        addChild(mMaxDisplay);

        ValueDisplayTinyWidget* sMaxDisplay = createWidget<ValueDisplayTinyWidget>(Vec(displayX_Px, yStart - 2 * yStep + off));
        sMaxDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            sMaxDisplay->valuePtr = &module->maxShortTermLufs;
        }
        sMaxDisplay->label = "SMAX";
        sMaxDisplay->unit = "Short-term max, LUFS";
        addChild(sMaxDisplay);

        ValueDisplayTinyWidget* tpmDisplay = createWidget<ValueDisplayTinyWidget>(Vec(displayX_Px, yStart - yStep));
        tpmDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            tpmDisplay->valuePtr = &module->truePeakMax;
        }
        tpmDisplay->label = "TPMAX";
        tpmDisplay->unit = "True peak max, dBTP";
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

Model* modelLoud = createModel<LoudnessMeter, LoudWidget>("Loud");