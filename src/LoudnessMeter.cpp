#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "LoudnessMeterEngine.hpp"
#include "components.hpp"
#include "plugin.hpp"

using namespace rack;

struct LoudnessMeterWidget;  // Forward declaration

// Generic number formatter (handles LUFS, LU, dB)
static std::string formatValue(float value) {
    if (value <= ALMOST_NEGATIVE_INFINITY || std::isinf(value) || std::isnan(value)) {
        return "-inf";
    }
    char buf[25];
    snprintf(buf, sizeof(buf), "%.1f", value);
    return std::string(buf);
}

struct LoudnessBarWidget : TransparentWidget {
    NVGcolor valueColor = nvgRGB(0xf5, 0xf5, 0xdc);
    NVGcolor redColor = nvgRGB(0xc0, 0x39, 0x2b);
    NVGcolor labelColor = nvgRGB(95, 190, 250);
    std::string label;
    float* momentaryValuePtr = nullptr;
    float* lowerRangeValuePtr = nullptr;
    float* upperRangeValuePtr = nullptr;
    float* targetValuePtr = nullptr;
    std::string unit = "";

    void drawLevelMarks(NVGcontext* vg, float x, float y, std::string label) {
        std::shared_ptr<Font> font = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/JetBrainsMono-Medium.ttf"));
        float markWidth = 4.f;
        float margin = 8.f;
        nvgStrokeColor(vg, valueColor);
        nvgStrokeWidth(vg, 0.4f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, x - margin, y);
        nvgLineTo(vg, x - margin - markWidth, y);
        nvgMoveTo(vg, x + margin, y);
        nvgLineTo(vg, x + margin + markWidth, y);
        nvgStroke(vg);
        nvgFontFaceId(vg, font->handle);
        nvgFontSize(vg, 9);
        nvgFillColor(vg, valueColor);
        nvgTextAlign(vg, NVG_ALIGN_MIDDLE | NVG_ALIGN_RIGHT);
        nvgText(vg, x - margin - markWidth - 3, y, label.c_str(), NULL);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        std::shared_ptr<Font> font = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/JetBrainsMono-Medium.ttf"));
        std::shared_ptr<Font> font2 = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/SofiaSansExtraCondensed-Regular.ttf"));
        if (layer == 1 && font && font2) {
            float marksStep = 3.8f;
            float marginBottom = 40.f;
            float barWidth = 12.f;

            // --- Draw Static Level Marks ---
            drawLevelMarks(args.vg, box.size.x * 0.5, 12, "0");
            drawLevelMarks(args.vg, box.size.x * 0.5, 12 + 3 * marksStep, "-3");
            drawLevelMarks(args.vg, box.size.x * 0.5, 12 + 6 * marksStep, "-6");
            drawLevelMarks(args.vg, box.size.x * 0.5, 12 + 9 * marksStep, "-9");
            drawLevelMarks(args.vg, box.size.x * 0.5, 12 + 18 * marksStep, "-18");
            drawLevelMarks(args.vg, box.size.x * 0.5, 12 + 27 * marksStep, "-27");
            drawLevelMarks(args.vg, box.size.x * 0.5, 12 + 36 * marksStep, "-36");
            drawLevelMarks(args.vg, box.size.x * 0.5, 12 + 45 * marksStep, "-45");
            drawLevelMarks(args.vg, box.size.x * 0.5, 12 + 54 * marksStep, "-54");

            bool canDrawDynamic = momentaryValuePtr && lowerRangeValuePtr && upperRangeValuePtr && targetValuePtr;

            if (canDrawDynamic) {
                float value = *momentaryValuePtr;
                float upperValue = *upperRangeValuePtr;
                float lowerValue = *lowerRangeValuePtr;
                float targetValue = *targetValuePtr;

                if (value <= ALMOST_NEGATIVE_INFINITY || std::isinf(value) || std::isnan(value)) {
                    value = -60.f;
                }
                if (std::isnan(upperValue)) upperValue = -60.f;
                if (std::isnan(lowerValue)) lowerValue = -60.f;
                if (std::isnan(targetValue)) targetValue = -23.f;

                // Clamp values to the displayable range [-60, 0]
                value = clamp(value, -60.f, 0.f);
                upperValue = clamp(upperValue, -60.f, 0.f);
                lowerValue = clamp(lowerValue, -60.f, 0.f);
                targetValue = clamp(targetValue, -60.f, 0.f);

                drawLevelMarks(args.vg, box.size.x * 0.5, 12 + (-targetValue) * marksStep, "");

                // Calculate bar geometry based on value and target
                float overshoot = value - targetValue;
                float room = (overshoot <= 0) ? 60.f + value : 60.f + targetValue;
                float barHeight = room / 60.0f * 228.f;

                if (barHeight <= 0.0) barHeight = 1.f;
                float yOffset = box.size.y - barHeight - marginBottom;

                // Draw main bar segment
                nvgBeginPath(args.vg);
                nvgRect(args.vg, 0.5 * (box.size.x - barWidth), yOffset, barWidth, barHeight);
                nvgFillColor(args.vg, valueColor);
                nvgFill(args.vg);
                nvgClosePath(args.vg);

                // Draw overshoot segment
                if (overshoot > 0.0) {
                    float overshootHeight = overshoot / 60.0f * 228.f;
                    float yOffsetOvershoot = yOffset - overshootHeight;
                    nvgBeginPath(args.vg);
                    nvgRect(args.vg, 0.5 * (box.size.x - barWidth), yOffsetOvershoot, barWidth, overshootHeight);
                    nvgFillColor(args.vg, redColor);
                    nvgFill(args.vg);
                    nvgClosePath(args.vg);
                }

                // Draw Loudness Range indicators
                float upperYPos = 12 + (-upperValue) * marksStep;
                float lowerYPos = 12 + (-lowerValue) * marksStep;

                if (upperYPos != lowerYPos) {
                    nvgStrokeColor(args.vg, nvgRGB(0xdd, 0xdd, 0xdd));
                    nvgStrokeWidth(args.vg, 0.7f);
                    nvgBeginPath(args.vg);
                    nvgMoveTo(args.vg, box.size.x * 0.5 + 14, upperYPos);
                    nvgLineTo(args.vg, box.size.x * 0.5 + 16, upperYPos);
                    nvgLineTo(args.vg, box.size.x * 0.5 + 16, lowerYPos);
                    nvgLineTo(args.vg, box.size.x * 0.5 + 14, lowerYPos);
                    nvgStroke(args.vg);
                }
            } else {
                float defaultBarHeight = 1.f;
                float defaultYOffset = box.size.y - defaultBarHeight - marginBottom;
                nvgBeginPath(args.vg);
                nvgRect(args.vg, 0.5 * (box.size.x - barWidth), defaultYOffset, barWidth, defaultBarHeight);
                nvgFillColor(args.vg, valueColor);
                nvgFill(args.vg);
                nvgClosePath(args.vg);
            }

            // --- Draw Unit and Label
            nvgFontFaceId(args.vg, font->handle);
            nvgFontSize(args.vg, 14);
            nvgFillColor(args.vg, valueColor);
            nvgTextAlign(args.vg, NVG_ALIGN_TOP | NVG_ALIGN_CENTER);
            nvgText(args.vg, box.size.x * 0.5, box.size.y - 35, unit.c_str(), NULL);

            nvgFontFaceId(args.vg, font2->handle);
            nvgFontSize(args.vg, 14);
            nvgFillColor(args.vg, labelColor);
            nvgTextAlign(args.vg, NVG_ALIGN_BASELINE | NVG_ALIGN_CENTER);
            nvgText(args.vg, box.size.x * 0.5, box.size.y - 11, label.c_str(), NULL);
        }
        Widget::drawLayer(args, layer);
    }
};

struct ValueDisplayWidget : TransparentWidget {
    NVGcolor valueColor = nvgRGB(0xf5, 0xf5, 0xdc);
    NVGcolor labelColor = nvgRGB(95, 190, 250);
    NVGcolor redColor = nvgRGB(0xc0, 0x39, 0x2b);
    std::string label;
    float* valuePtr = nullptr;
    std::string unit = "";

    void drawLayer(const DrawArgs& args, int layer) override {
        std::shared_ptr<Font> font = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/JetBrainsMono-Medium.ttf"));
        std::shared_ptr<Font> font2 = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/SofiaSansExtraCondensed-Regular.ttf"));
        if (layer == 1 && font && font2) {
            float middleY = box.size.y * 0.5f;

            // --- Determine Value String ---
            std::string valueText = "-inf";
            bool drawDash = false;
            bool clipping = false;

            if (valuePtr) {
                float currentValue = *valuePtr;
                bool cond1 = currentValue <= ALMOST_NEGATIVE_INFINITY || std::isinf(currentValue) || std::isnan(currentValue);
                bool cond2 = (label == "LOUDNESS RANGE") && currentValue <= 0.0f;
                bool cond3 = (label == "TRUE PEAK MAX") && currentValue >= -0.5f;

                if (cond1 || cond2) {
                    drawDash = true;
                } else {
                    // Format the valid number
                    valueText = formatValue(currentValue);
                    if (cond3) {
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
                nvgStrokeWidth(args.vg, 2.1f);
                nvgBeginPath(args.vg);
                nvgMoveTo(args.vg, box.size.x - 9.5, middleY - 13.0);
                nvgLineTo(args.vg, box.size.x - 29.5, middleY - 13.0);
                nvgStroke(args.vg);
            } else {
                nvgFontSize(args.vg, 32);
                nvgFillColor(args.vg, clipping ? redColor : valueColor);
                nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE);
                nvgText(args.vg, box.size.x - 9.5, middleY - 5.0, valueText.c_str(), NULL);
            }

            // --- Draw Unit ---
            nvgFontSize(args.vg, 14);
            nvgFillColor(args.vg, valueColor);
            nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
            nvgText(args.vg, box.size.x - 9.5, middleY, unit.c_str(), NULL);

            // --- Draw Label ---
            nvgFontFaceId(args.vg, font2->handle);
            nvgFontSize(args.vg, 14);
            nvgFillColor(args.vg, labelColor);
            nvgTextAlign(args.vg, NVG_ALIGN_BASELINE | NVG_ALIGN_RIGHT);
            nvgText(args.vg, box.size.x - 9.5, box.size.y - 11, label.c_str(), NULL);
        }
        Widget::drawLayer(args, layer);
    }
};

struct LoudnessMeterWidget : ModuleWidget {
    LoudnessMeterWidget(LoudnessMeter* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/LoudnessMeter.svg"), asset::plugin(pluginInstance, "res/LoudnessMeter-dark.svg")));

        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        LedDisplay* ledDisplay = createWidget<LedDisplay>(Vec(0, 26));
        ledDisplay->box.size = Vec(225, 280);
        addChild(ledDisplay);

        float displayHeightPx = 70.f;
        float yStep = displayHeightPx;
        float yStart = 26.f;
        float displayX_Px = 45.f;
        float displayWidthPx = 90;
        float inputYPx = 329.25;

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(22.5f, inputYPx), module, LoudnessMeter::AUDIO_INPUT_L));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(67.5f, inputYPx), module, LoudnessMeter::AUDIO_INPUT_R));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(112.5f, inputYPx), module, LoudnessMeter::RESET_INPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(196.760f, 361.120f), module, LoudnessMeter::OVERSHOOT_OUTPUT));
        addParam(createParamCentered<VCVButton>(Vec(157.5f, inputYPx), module, LoudnessMeter::RESET_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(Vec(202.5f, inputYPx), module, LoudnessMeter::TARGET_PARAM));

        LoudnessBarWidget* momentaryDisplay = createWidget<LoudnessBarWidget>(Vec(10, yStart));
        momentaryDisplay->box.size = Vec(45, 280);
        momentaryDisplay->label = "M";
        momentaryDisplay->unit = "LUFS";
        if (module) {
            momentaryDisplay->momentaryValuePtr = &module->momentaryLufs;
            momentaryDisplay->upperRangeValuePtr = &module->loudnessRangeHigh;
            momentaryDisplay->lowerRangeValuePtr = &module->loudnessRangeLow;
            momentaryDisplay->targetValuePtr = &module->targetLoudness;
        }
        addChild(momentaryDisplay);

        ValueDisplayWidget* shortTermDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px + 5.f, yStart));
        shortTermDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            shortTermDisplay->valuePtr = &module->shortTermLufs;
        }
        shortTermDisplay->label = "SHORT TERM";
        shortTermDisplay->unit = "LUFS";
        addChild(shortTermDisplay);

        ValueDisplayWidget* integratedDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px + displayWidthPx, yStart));
        integratedDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            integratedDisplay->valuePtr = &module->integratedLufs;
        }
        integratedDisplay->label = "INTEGRATED";
        integratedDisplay->unit = "LUFS";
        addChild(integratedDisplay);

        ValueDisplayWidget* lraDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px + 5.f, yStart + 2 * yStep));
        lraDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            lraDisplay->valuePtr = &module->loudnessRange;
        }
        lraDisplay->label = "LOUDNESS RANGE";
        lraDisplay->unit = "LU";
        addChild(lraDisplay);

        ValueDisplayWidget* psrDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px + 5.f, yStart + 1 * yStep));
        psrDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            psrDisplay->valuePtr = &module->psrValue;
        }
        psrDisplay->label = "DYNAMICS (PSR)";
        psrDisplay->unit = "LU";
        addChild(psrDisplay);

        ValueDisplayWidget* plrDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px + displayWidthPx, yStart + 1 * yStep));
        plrDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            plrDisplay->valuePtr = &module->plrValue;
        }
        plrDisplay->label = "AVG DYN (PLR)";
        plrDisplay->unit = "LU";
        addChild(plrDisplay);

        ValueDisplayWidget* mMaxDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px + 5.f, yStart + 3 * yStep));
        mMaxDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            mMaxDisplay->valuePtr = &module->maxMomentaryLufs;
        }
        mMaxDisplay->label = "MOMENTARY MAX";
        mMaxDisplay->unit = "LUFS";
        addChild(mMaxDisplay);

        ValueDisplayWidget* sMaxDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px + displayWidthPx, yStart + 3 * yStep));
        sMaxDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            sMaxDisplay->valuePtr = &module->maxShortTermLufs;
        }
        sMaxDisplay->label = "SHORT TERM MAX";
        sMaxDisplay->unit = "LUFS";
        addChild(sMaxDisplay);

        ValueDisplayWidget* tpmDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px + displayWidthPx, yStart + 2 * yStep));
        tpmDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            tpmDisplay->valuePtr = &module->truePeakMax;
        }
        tpmDisplay->label = "TRUE PEAK MAX";
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
    }
};

Model* modelLoudnessMeter = createModel<LoudnessMeter, LoudnessMeterWidget>("LoudnessMeter");