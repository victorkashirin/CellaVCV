#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "components.hpp"
#include "plugin.hpp"

// Include the dependency header
#include "ebur128.h"

using namespace rack;

struct LoudnessMeterWidget;  // Forward declaration

const float ALMOST_NEGATIVE_INFINITY = -99.0f;
const float VOLTAGE_SCALE = 0.1f;  // Scale +/-10V to +/-1.0f
const float LOG_EPSILON = 1e-10f;  // Or adjust as needed

struct LoudnessMeter : engine::Module {
    enum ParamIds {
        RESET_PARAM,
        TARGET_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        AUDIO_INPUT_L,
        AUDIO_INPUT_R,
        RESET_INPUT,
        NUM_INPUTS
    };
    enum OutputIds { NUM_OUTPUTS };
    enum LightIds { NUM_LIGHTS };

    // --- Configuration ---
    // Process samples in blocks of this size before feeding to libebur128
    // Powers of 2 are often efficient. 256 samples at 48kHz is ~5.3ms.
    static const size_t PROCESSING_BLOCK_FRAMES = 2048;

    // --- libebur128 State ---
    ebur128_state* ebur128_handle = nullptr;
    size_t currentInputChannels = 0;  // 0: inactive, 1: mono, 2: stereo

    // --- Internal Buffering ---
    std::vector<float> processingBuffer;
    size_t bufferPosition = 0;

    // --- Measured Values (Displayed) ---
    float momentaryLufs = -INFINITY;
    float shortTermLufs = -INFINITY;
    float integratedLufs = -INFINITY;
    float loudnessRange = -INFINITY;
    float loudnessRangeLow = -INFINITY;
    float loudnessRangeHigh = -INFINITY;
    float psrValue = -INFINITY;
    float plrValue = -INFINITY;
    float truePeakMax = -INFINITY;
    float truePeakSlidingMax = -INFINITY;
    // Param
    float targetLoudness = -23.f;

    // --- Values tracked manually for PSR calculation ---
    float maxShortTermLufs = -INFINITY;
    float maxMomentaryLufs = -INFINITY;
    float maxTruePeakL = -INFINITY;
    float maxTruePeakR = -INFINITY;

    // Settings for peak history
    const float PEAK_HISTORY_SECONDS = 2.5f;
    size_t peakHistorySizeChunks = 0;

    // History buffer for chunk peaks (in dBTP)
    std::deque<float> chunkPeakHistoryDB;

    // --- Control & Timing ---
    dsp::SchmittTrigger resetTrigger;
    dsp::SchmittTrigger resetPortTrigger;
    bool shortTermEnabled = true;

    LoudnessMeter() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(AUDIO_INPUT_L, "Audio L / Mono");
        configInput(AUDIO_INPUT_R, "Audio R");
        configInput(RESET_INPUT, "Reset");
        configParam(TARGET_PARAM, -36.f, 0.f, -23.f, "Target loudness", " LUFS");
        configParam(RESET_PARAM, 0.f, 1.f, 0.f, "Reset");

        processingBuffer.resize(PROCESSING_BLOCK_FRAMES * 2);  // Max 2 channels

        // Initialize state
        resetMeter();
        calculate_history_size();
    }

    ~LoudnessMeter() override {
        if (ebur128_handle) {
            // Process any remaining samples in the buffer before destroying
            processBlockBuffer();
            ebur128_destroy(&ebur128_handle);
        }
    }

    void calculate_history_size() {
        int chunkSizeFrames = PROCESSING_BLOCK_FRAMES;
        // to ebur128_add_frames_*
        float sampleRate = APP->engine->getSampleRate();
        if (sampleRate > 0 && chunkSizeFrames > 0) {
            peakHistorySizeChunks = static_cast<size_t>(
                std::ceil(PEAK_HISTORY_SECONDS * sampleRate / (float)chunkSizeFrames));
            // Ensure a minimum history size if calculation is very small
            if (peakHistorySizeChunks < 5) peakHistorySizeChunks = 5;
        } else {
            peakHistorySizeChunks = 100;  // Default if sample rate/chunk size invalid
        }
        // Optional: Trim deque if history size changed significantly
        while (chunkPeakHistoryDB.size() > peakHistorySizeChunks) {
            chunkPeakHistoryDB.pop_front();
        }
    }

    float get_max_from_deque(const std::deque<float>& dq) {
        if (dq.empty()) {
            return -INFINITY;
        }
        float maxVal = -INFINITY;
        bool foundValid = false;
        for (float val : dq) {
            if (val > maxVal) {
                maxVal = val;
                foundValid = true;
            }
        }
        // If only -inf values were present, return -inf
        return foundValid ? maxVal : -INFINITY;
    }

    // Function to process the accumulated buffer
    void processBlockBuffer() {
        if (!ebur128_handle || bufferPosition == 0) {
            return;
        }

        // Number of frames actually collected
        size_t framesToProcess = bufferPosition;

        // Pass the collected frames to libebur128
        int err = ebur128_add_frames_float(ebur128_handle, processingBuffer.data(), framesToProcess);

        bufferPosition = 0;

        if (err != EBUR128_SUCCESS) {
            DEBUG("LoudnessMeter: Error adding frames to ebur128: %d", err);
            resetMeter();
            return;
        }

        // --- Update Meter Readings ---
        updateLoudnessValues();
    }

    void updateLoudnessValues() {
        if (!ebur128_handle || peakHistorySizeChunks == 0) return;

        int err;
        double loudnessValue;
        double peakValue;

        err = ebur128_loudness_momentary(ebur128_handle, &loudnessValue);
        if (err == EBUR128_SUCCESS) {
            momentaryLufs = (float)loudnessValue;  // Store CURRENT short-term LUFS
            if (momentaryLufs > maxMomentaryLufs && momentaryLufs > ALMOST_NEGATIVE_INFINITY) {
                maxMomentaryLufs = momentaryLufs;
            }
        } else {
            momentaryLufs = -INFINITY;
        }

        if (shortTermEnabled) {
            err = ebur128_loudness_shortterm(ebur128_handle, &loudnessValue);
            if (err == EBUR128_SUCCESS) {
                shortTermLufs = (float)loudnessValue;  // Store CURRENT short-term LUFS
                // Update Max Short Term LUFS
                if (shortTermLufs > maxShortTermLufs && shortTermLufs > ALMOST_NEGATIVE_INFINITY) {
                    maxShortTermLufs = shortTermLufs;
                }
            } else {
                shortTermLufs = -INFINITY;
            }
        } else {
            shortTermLufs = -INFINITY;
            maxShortTermLufs = -INFINITY;
        }

        err = ebur128_loudness_global(ebur128_handle, &loudnessValue);
        if (err == EBUR128_SUCCESS) {
            integratedLufs = (float)loudnessValue;
        }

        double low_en, high_en;
        err = ebur128_loudness_range_ext(ebur128_handle, &loudnessValue, &low_en, &high_en);
        if (err == EBUR128_SUCCESS) {
            loudnessRange = (float)loudnessValue;
            loudnessRangeLow = (float)low_en;
            loudnessRangeHigh = (float)high_en;
        }

        // --- Get Current True Peak Values ---
        float currentPeakL = -INFINITY;
        float currentPeakR = -INFINITY;

        if (currentInputChannels >= 1) {
            err = ebur128_prev_true_peak(ebur128_handle, 0, &peakValue);
            if (err == EBUR128_SUCCESS) {
                float linearAmp = (float)peakValue;  // Store CURRENT peak L
                if (linearAmp > LOG_EPSILON) {
                    currentPeakL = 20.0f * std::log10(linearAmp);
                }

                // Update Max True Peak L
                if (currentPeakL > maxTruePeakL && currentPeakL > ALMOST_NEGATIVE_INFINITY) {
                    maxTruePeakL = currentPeakL;
                }
            }
        }
        if (currentInputChannels == 2) {
            err = ebur128_prev_true_peak(ebur128_handle, 1, &peakValue);
            if (err == EBUR128_SUCCESS) {
                float linearAmp = (float)peakValue;  // Store CURRENT peak R
                if (linearAmp > LOG_EPSILON) {
                    currentPeakR = 20.0f * std::log10(linearAmp);
                }
                // Update Max True Peak R
                if (currentPeakR > maxTruePeakR && currentPeakR > ALMOST_NEGATIVE_INFINITY) {
                    maxTruePeakR = currentPeakR;
                }
            }
        } else {
            // If mono, use Left peak for Right peak as well for max calculation
            currentPeakR = currentPeakL;
        }

        // --- Correctly Calculate PSR ---
        // Use the maximum of the CURRENT peaks from this update cycle
        float currentMaxTruePeak = std::fmax(currentPeakL, currentPeakR);
        chunkPeakHistoryDB.push_back(currentMaxTruePeak);

        // Remove oldest entry if history buffer is full
        while (chunkPeakHistoryDB.size() > peakHistorySizeChunks) {
            chunkPeakHistoryDB.pop_front();
        }

        // --- Find Max Peak over the History Window ---
        float peakOverWindowDB = get_max_from_deque(chunkPeakHistoryDB);

        // Check if both current short-term and current max peak are valid
        if (peakOverWindowDB > ALMOST_NEGATIVE_INFINITY && shortTermLufs > ALMOST_NEGATIVE_INFINITY) {
            // PSR = Current Max True Peak - Current Short Term Loudness
            psrValue = peakOverWindowDB - shortTermLufs;
        } else {
            psrValue = -INFINITY;  // Not enough valid data for current PSR
        }

        truePeakMax = std::fmax(maxTruePeakL, maxTruePeakR);
        truePeakSlidingMax = peakOverWindowDB;

        if (truePeakMax > ALMOST_NEGATIVE_INFINITY && integratedLufs > ALMOST_NEGATIVE_INFINITY) {
            // Calculate PLR (Peak to Loudness Ratio)
            plrValue = truePeakMax - integratedLufs;
        } else {
            plrValue = -INFINITY;  // Not enough valid data for current PLR
        }
    }

    void resetMeter() {
        processBlockBuffer();

        if (ebur128_handle) {
            ebur128_destroy(&ebur128_handle);
            ebur128_handle = nullptr;
        }

        size_t channels = inputs[AUDIO_INPUT_R].isConnected() ? 2 : (inputs[AUDIO_INPUT_L].isConnected() ? 1 : 0);
        float sampleRate = APP->engine->getSampleRate();

        if (channels > 0 && sampleRate > 0) {
            ebur128_handle = ebur128_init(
                channels,
                (size_t)sampleRate,
                EBUR128_MODE_M | EBUR128_MODE_S |
                    EBUR128_MODE_I | EBUR128_MODE_LRA |
                    EBUR128_MODE_HISTOGRAM | EBUR128_MODE_TRUE_PEAK);

            if (!ebur128_handle) {
                DEBUG("LoudnessMeter: Failed to re-initialize ebur128");
                currentInputChannels = 0;
            } else {
                currentInputChannels = channels;
            }
        } else {
            currentInputChannels = 0;
            ebur128_handle = nullptr;
        }

        bufferPosition = 0;
        chunkPeakHistoryDB.clear();

        momentaryLufs = -INFINITY;
        shortTermLufs = -INFINITY;
        integratedLufs = -INFINITY;
        loudnessRange = -INFINITY;
        loudnessRangeLow = -INFINITY;
        loudnessRangeHigh = -INFINITY;
        psrValue = -INFINITY;
        plrValue = -INFINITY;
        maxShortTermLufs = -INFINITY;
        maxMomentaryLufs = -INFINITY;
        maxTruePeakL = -INFINITY;
        maxTruePeakR = -INFINITY;
        truePeakMax = -INFINITY;
        truePeakSlidingMax = -INFINITY;
    }

    void onReset(const ResetEvent& e) override {
        Module::onReset(e);
        resetMeter();
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        Module::onSampleRateChange(e);
        calculate_history_size();
        resetMeter();
    }

    void process(const ProcessArgs& args) override {
        if (resetTrigger.process(params[RESET_PARAM].getValue()) || resetPortTrigger.process(inputs[RESET_INPUT].getVoltage())) {
            resetMeter();
        }

        targetLoudness = params[TARGET_PARAM].getValue();
        size_t connectedChannels = inputs[AUDIO_INPUT_R].isConnected() ? 2 : (inputs[AUDIO_INPUT_L].isConnected() ? 1 : 0);
        if (connectedChannels != currentInputChannels) {
            resetMeter();
        }

        // If no input or init failed, bail out for this sample
        if (!ebur128_handle || currentInputChannels == 0) {
            momentaryLufs = -INFINITY;
            shortTermLufs = -INFINITY;
            // Keep integrated unless reset
            loudnessRange = -INFINITY;
            loudnessRangeHigh = -INFINITY;
            loudnessRangeLow = -INFINITY;
            psrValue = -INFINITY;
            plrValue = -INFINITY;
            truePeakMax = -INFINITY;
            truePeakSlidingMax = -INFINITY;
            maxShortTermLufs = -INFINITY;

            return;
        }

        // --- 3. Get Current Sample(s) and Add to Internal Buffer ---
        float left = inputs[AUDIO_INPUT_L].getVoltage() * VOLTAGE_SCALE;
        float right = (currentInputChannels == 2) ? inputs[AUDIO_INPUT_R].getVoltage() * VOLTAGE_SCALE : left;  // Use left for mono/unconnected right

        // Add sample(s) to the processing buffer (interleaved)
        if (bufferPosition < PROCESSING_BLOCK_FRAMES) {
            size_t bufferBaseIndex = bufferPosition * currentInputChannels;
            processingBuffer[bufferBaseIndex] = left;
            if (currentInputChannels == 2) {
                processingBuffer[bufferBaseIndex + 1] = right;
            }
            bufferPosition++;
        }

        if (bufferPosition >= PROCESSING_BLOCK_FRAMES) {
            processBlockBuffer();
        }
    }

    // --- 8. Data Persistence ---
    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "integratedLufs", json_real(integratedLufs));
        json_object_set_new(rootJ, "shortTermEnabled", json_boolean(shortTermEnabled));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* lufsJ = json_object_get(rootJ, "integratedLufs");
        if (lufsJ) {
            integratedLufs = json_real_value(lufsJ);
        } else {
            integratedLufs = -INFINITY;
        }
        json_t* shortTermEnabledJ = json_object_get(rootJ, "shortTermEnabled");
        if (shortTermEnabledJ) shortTermEnabled = json_boolean_value(shortTermEnabledJ);
        // Ensure other derived values are reset on load, they will recalculate.
        bufferPosition = 0;  // Clear any partially filled buffer from save state
        momentaryLufs = -INFINITY;
        shortTermLufs = -INFINITY;
        loudnessRange = -INFINITY;
        psrValue = -INFINITY;
        plrValue = -INFINITY;
        maxShortTermLufs = -INFINITY;
        maxMomentaryLufs = -INFINITY;
        maxTruePeakL = -INFINITY;
        maxTruePeakR = -INFINITY;
    }
};

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
    std::shared_ptr<Font> font;
    std::shared_ptr<Font> font2;
    NVGcolor valueColor = nvgRGB(0xf5, 0xf5, 0xdc);
    NVGcolor redColor = nvgRGB(0xc0, 0x39, 0x2b);
    NVGcolor labelColor = nvgRGB(0x1a, 0xa7, 0xff);
    std::string label;
    float* momentaryValuePtr = nullptr;
    float* lowerRangeValuePtr = nullptr;
    float* upperRangeValuePtr = nullptr;
    float* targetValuePtr = nullptr;
    std::string unit = "";

    LoudnessBarWidget() {
        font = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/JetBrainsMono-Medium.ttf"));
        font2 = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/SofiaSansExtraCondensed-Regular.ttf"));
    }

    void drawLevelMarks(NVGcontext* vg, float x, float y, std::string label) {
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
        if (layer != 1)
            return;
        if (!font || !font2) return;

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
};

struct ValueDisplayWidget : TransparentWidget {
    std::shared_ptr<Font> font;
    std::shared_ptr<Font> font2;
    NVGcolor valueColor = nvgRGB(0xf5, 0xf5, 0xdc);
    NVGcolor labelColor = nvgRGB(0x1a, 0xa7, 0xff);
    NVGcolor redColor = nvgRGB(0xc0, 0x39, 0x2b);
    std::string label;
    float* valuePtr = nullptr;
    std::string unit = "";

    ValueDisplayWidget() {
        font = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/JetBrainsMono-Medium.ttf"));
        font2 = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/SofiaSansExtraCondensed-Regular.ttf"));
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1)
            return;
        if (!font || !font2) return;

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

        ValueDisplayWidget* shortTermDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px, yStart));
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

        ValueDisplayWidget* lraDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px, yStart + 2 * yStep));
        lraDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            lraDisplay->valuePtr = &module->loudnessRange;
        }
        lraDisplay->label = "LOUDNESS RANGE";
        lraDisplay->unit = "LU";
        addChild(lraDisplay);

        ValueDisplayWidget* psrDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px, yStart + 1 * yStep));
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

        ValueDisplayWidget* mMaxDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px, yStart + 3 * yStep));
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
        tpmDisplay->unit = "dB";
        addChild(tpmDisplay);
    }

    void appendContextMenu(Menu* menu) override {
        LoudnessMeter* module = dynamic_cast<LoudnessMeter*>(this->module);
        assert(module);
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Settings"));
        menu->addChild(createIndexPtrSubmenuItem("Short-Term Loudness",
                                                 {"Disabled",
                                                  "Enabled"},
                                                 &module->shortTermEnabled));
    }
};

Model* modelLoudnessMeter = createModel<LoudnessMeter, LoudnessMeterWidget>("LoudnessMeter");