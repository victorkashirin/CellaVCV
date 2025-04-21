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

//-----------------------------------------------------------------------------
// Module Logic: LoudnessMeter (Corrected with Buffering)
//-----------------------------------------------------------------------------
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

    // How often to update the display (reduces GUI overhead)
    // Update roughly 15 times per second (e.g., 48000Hz / 15Hz ≈ 3200 samples)
    // Make it a multiple of PROCESSING_BLOCK_FRAMES if possible
    static const int DISPLAY_UPDATE_INTERVAL_FRAMES = 4096;  // 12 * 256

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
    float targetLoudness = -23.f;  // Target loudness for PSR calculation

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
    dsp::ClockDivider displayUpdateDivider;

    LoudnessMeter() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(AUDIO_INPUT_L, "Audio L / Mono");
        configInput(AUDIO_INPUT_R, "Audio R");
        configInput(RESET_INPUT, "Reset");
        configParam(TARGET_PARAM, -36.f, 0.f, -23.f, "Target loudness", " LUFS");
        configParam(RESET_PARAM, 0.f, 1.f, 0.f, "Reset");

        // Allocate buffer based on max channels (stereo)
        processingBuffer.resize(PROCESSING_BLOCK_FRAMES * 2);  // Max 2 channels

        displayUpdateDivider.setDivision(DISPLAY_UPDATE_INTERVAL_FRAMES);

        // Initialize state
        resetMeter();
        calculate_history_size();
    }

    ~LoudnessMeter() override {
        if (ebur128_handle) {
            // Optionally process any remaining samples in the buffer before destroying
            // processBlockBuffer();
            ebur128_destroy(&ebur128_handle);
        }
    }

    void calculate_history_size() {
        // Assuming 'chunk_size_frames' is the number of frames you process in each call
        int chunk_size_frames = PROCESSING_BLOCK_FRAMES;  // Or get this from your engine
        // to ebur128_add_frames_*
        float sampleRate = APP->engine->getSampleRate();
        if (sampleRate > 0 && chunk_size_frames > 0) {
            peakHistorySizeChunks = static_cast<size_t>(
                std::ceil(PEAK_HISTORY_SECONDS * sampleRate / (float)chunk_size_frames));
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
        float max_val = -INFINITY;
        bool found_valid = false;
        for (float val : dq) {
            if (val > max_val) {
                max_val = val;
                found_valid = true;
            }
        }
        // If only -inf values were present, return -inf
        return found_valid ? max_val : -INFINITY;
    }

    // Function to process the accumulated buffer
    void processBlockBuffer() {
        if (!ebur128_handle || bufferPosition == 0) {
            return;  // Nothing to process or library not initialized
        }

        // Number of frames actually collected (might be less than full block if reset/stopped)
        size_t framesToProcess = bufferPosition;

        // Pass the collected frames to libebur128
        int err = ebur128_add_frames_float(ebur128_handle, processingBuffer.data(), framesToProcess);

        bufferPosition = 0;  // Reset buffer position for next block

        if (err != EBUR128_SUCCESS) {
            std::cerr << "LoudnessMeter: Error adding frames: " << err << std::endl;
            resetMeter();  // Simple error handling: reset everything
            return;
        }

        // --- Update Meter Readings (only after processing a block) ---
        updateLoudnessValues();
    }

    // Function to query libebur128 and update internal state variables
    void updateLoudnessValues() {
        if (!ebur128_handle || peakHistorySizeChunks == 0) return;

        int err;
        double loudnessValue;
        double peakValue;

        err = ebur128_loudness_momentary(ebur128_handle, &loudnessValue);
        if (err == EBUR128_SUCCESS) {
            momentaryLufs = (float)loudnessValue;  // Store CURRENT short-term LUFS
            // Update Max Short Term LUFS (Keep this for display if you want a separate max value)
            if (momentaryLufs > maxMomentaryLufs && momentaryLufs > ALMOST_NEGATIVE_INFINITY) {
                maxMomentaryLufs = momentaryLufs;
            }
        } else {
            momentaryLufs = -INFINITY;
        }

        err = ebur128_loudness_shortterm(ebur128_handle, &loudnessValue);
        if (err == EBUR128_SUCCESS) {
            shortTermLufs = (float)loudnessValue;  // Store CURRENT short-term LUFS
            // Update Max Short Term LUFS (Keep this for display if you want a separate max value)
            if (shortTermLufs > maxShortTermLufs && shortTermLufs > ALMOST_NEGATIVE_INFINITY) {
                maxShortTermLufs = shortTermLufs;
            }
        } else {
            shortTermLufs = -INFINITY;
        }

        err = ebur128_loudness_global(ebur128_handle, &loudnessValue);
        if (err == EBUR128_SUCCESS) {
            integratedLufs = (float)loudnessValue;
        }  // Else: Keep old value

        double low_en, high_en;
        err = ebur128_loudness_range_ext(ebur128_handle, &loudnessValue, &low_en, &high_en);
        // err = ebur128_loudness_range(ebur128_handle, &loudnessValue);
        if (err == EBUR128_SUCCESS) {
            loudnessRange = (float)loudnessValue;
            loudnessRangeLow = (float)low_en;
            loudnessRangeHigh = (float)high_en;
        }

        // --- Get Current True Peak Values ---
        float currentPeakL = -INFINITY;
        float currentPeakR = -INFINITY;  // Initialize for the current update cycle

        if (currentInputChannels >= 1) {
            err = ebur128_prev_true_peak(ebur128_handle, 0, &peakValue);
            if (err == EBUR128_SUCCESS) {
                float linear_amp = (float)peakValue;  // Store CURRENT peak L
                if (linear_amp > LOG_EPSILON) {
                    currentPeakL = 20.0f * std::log10(linear_amp);
                }

                // Update Max True Peak L (Keep this for display if you want a separate max value)
                if (currentPeakL > maxTruePeakL && currentPeakL > ALMOST_NEGATIVE_INFINITY) {
                    maxTruePeakL = currentPeakL;
                }
            }
        }
        if (currentInputChannels == 2) {
            err = ebur128_prev_true_peak(ebur128_handle, 1, &peakValue);
            if (err == EBUR128_SUCCESS) {
                float linear_amp = (float)peakValue;  // Store CURRENT peak L
                if (linear_amp > LOG_EPSILON) {
                    currentPeakR = 20.0f * std::log10(linear_amp);
                }
                // Update Max True Peak R (Keep this for display if you want a separate max value)
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

        truePeakMax = std::fmax(maxTruePeakL, maxTruePeakR) + 0.4f;
        truePeakSlidingMax = peakOverWindowDB;

        if (truePeakMax > ALMOST_NEGATIVE_INFINITY && integratedLufs > ALMOST_NEGATIVE_INFINITY) {
            // Calculate PLR (Peak to Loudness Ratio)
            plrValue = truePeakMax - integratedLufs;
        } else {
            plrValue = -INFINITY;  // Not enough valid data for current PLR
        }
    }

    // Reset meter state completely
    void resetMeter() {
        // Process any remaining samples before resetting state
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
                EBUR128_MODE_M | EBUR128_MODE_S | EBUR128_MODE_I |
                    EBUR128_MODE_LRA | EBUR128_MODE_HISTOGRAM | EBUR128_MODE_TRUE_PEAK);

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
        resetMeter();  // Re-initialize ebur128 and reset state
    }

    void process(const ProcessArgs& args) override {
        if (resetTrigger.process(params[RESET_PARAM].getValue()) || resetPortTrigger.process(inputs[RESET_INPUT].getVoltage())) {
            resetMeter();
        }

        targetLoudness = params[TARGET_PARAM].getValue();

        // --- 2. Check Connections and Re-initialize if Needed ---
        size_t connectedChannels = inputs[AUDIO_INPUT_R].isConnected() ? 2 : (inputs[AUDIO_INPUT_L].isConnected() ? 1 : 0);
        if (connectedChannels != currentInputChannels) {
            resetMeter();  // Re-initialize if channel count changed
        }

        // If no input or init failed, bail out for this sample
        if (!ebur128_handle || currentInputChannels == 0) {
            // Ensure display updates eventually show inactive state if connection is lost
            if (displayUpdateDivider.process()) {
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
            }
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

        // --- 5. Update Display Periodically ---
        // We update the display values less frequently than we process blocks
        // to reduce GUI overhead. The *internal* LUFS state is updated
        // every block, but the display reflects the state at these intervals.
        // This means momentary might lag slightly, but S, I, LRA, PSR are longer-term
        // and less affected by display update rate.
        if (displayUpdateDivider.process()) {
            // The actual displayed values are already updated within
            // updateLoudnessValues() when a block is processed.
            // This divider just controls how often the *widget* potentially redraws.
            // No explicit action needed here unless we wanted separate display variables.
        }
    }

    // --- 8. Data Persistence ---
    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "integratedLufs", json_real(integratedLufs));
        // Persisting LRA/PSR/max values is tricky as libebur128 state isn't saved.
        // Integrated is the most standard value to persist display-wise.
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* lufsJ = json_object_get(rootJ, "integratedLufs");
        if (lufsJ) {
            integratedLufs = json_real_value(lufsJ);
        } else {
            integratedLufs = -INFINITY;
        }
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

void drawDebugFrame(NVGcontext* vg, Rect box) {
    nvgStrokeColor(vg, nvgRGB(0xff, 0xff, 0xff));
    nvgStrokeWidth(vg, 0.2f);
    nvgBeginPath(vg);
    nvgMoveTo(vg, 0, 0);
    nvgLineTo(vg, box.size.x, 0);
    nvgLineTo(vg, box.size.x, box.size.y);
    nvgLineTo(vg, 0, box.size.y);
    nvgLineTo(vg, 0, 0);

    nvgMoveTo(vg, 0, box.size.y * 0.5);
    nvgLineTo(vg, box.size.x, box.size.y * 0.5);
    nvgStroke(vg);
}

struct LoudnessBarWidget : TransparentWidget {
    std::shared_ptr<Font> font;
    std::shared_ptr<Font> font2;
    NVGcolor textColor = nvgRGB(0x00, 0xbf, 0xff);
    NVGcolor valueColor = nvgRGB(0xff, 0xff, 0xff);
    NVGcolor redColor = nvgRGB(0xff, 0x00, 0x00);
    NVGcolor labelColor = nvgRGB(0x00, 0xbf, 0xff);
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
        nvgStrokeColor(vg, nvgRGB(0xff, 0xff, 0xff));
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

    void draw(const DrawArgs& args) override {
        // Check fonts first
        if (!font || !font2) return;

        float marksStep = 3.8f;
        float marginBottom = 40.f;
        float barWidth = 12.f;

        // --- Draw Static Level Marks (Always) ---
        drawLevelMarks(args.vg, box.size.x * 0.5, 12, "0");
        drawLevelMarks(args.vg, box.size.x * 0.5, 12 + 3 * marksStep, "-3");
        drawLevelMarks(args.vg, box.size.x * 0.5, 12 + 6 * marksStep, "-6");
        drawLevelMarks(args.vg, box.size.x * 0.5, 12 + 9 * marksStep, "-9");
        drawLevelMarks(args.vg, box.size.x * 0.5, 12 + 18 * marksStep, "-18");
        drawLevelMarks(args.vg, box.size.x * 0.5, 12 + 27 * marksStep, "-27");
        drawLevelMarks(args.vg, box.size.x * 0.5, 12 + 36 * marksStep, "-36");
        drawLevelMarks(args.vg, box.size.x * 0.5, 12 + 45 * marksStep, "-45");
        drawLevelMarks(args.vg, box.size.x * 0.5, 12 + 54 * marksStep, "-54");

        // --- Draw Dynamic Bar and Range (Conditional) ---
        // Check if module and all necessary pointers are valid
        bool canDrawDynamic = momentaryValuePtr && lowerRangeValuePtr && upperRangeValuePtr && targetValuePtr;

        if (canDrawDynamic) {
            float value = *momentaryValuePtr;
            float upperValue = *upperRangeValuePtr;
            float lowerValue = *lowerRangeValuePtr;
            float targetValue = *targetValuePtr;

            // Handle NaN/inf values coming from the module
            if (value <= ALMOST_NEGATIVE_INFINITY || std::isinf(value) || std::isnan(value)) {
                value = -60.f;  // Use minimum displayable value
            }
            // Use sensible defaults or minimums if range/target are NaN
            if (std::isnan(upperValue)) upperValue = -60.f;
            if (std::isnan(lowerValue)) lowerValue = -60.f;
            if (std::isnan(targetValue)) targetValue = 0.f;

            // Clamp values to the displayable range [-60, 0]
            value = clamp(value, -60.f, 0.f);
            upperValue = clamp(upperValue, -60.f, 0.f);
            lowerValue = clamp(lowerValue, -60.f, 0.f);
            targetValue = clamp(targetValue, -60.f, 0.f);  // Clamp target too for mark position

            // Draw target mark (even if value is low)
            drawLevelMarks(args.vg, box.size.x * 0.5, 12 + (-targetValue) * marksStep, "");

            // Calculate bar geometry based on value and target
            float overshoot = value - targetValue;
            float room = (overshoot <= 0) ? 60.f + value : 60.f + targetValue;
            float barHeight = room / 60.0f * 228.f;  // Max height corresponds to 60 LU range
            float yOffset = box.size.y - barHeight - marginBottom;

            if (barHeight <= 0.0) barHeight = 1.f;  // Ensure minimum visible height

            // Draw main bar segment (up to target)
            nvgBeginPath(args.vg);
            nvgRect(args.vg, 0.5 * (box.size.x - barWidth), yOffset, barWidth, barHeight);
            nvgFillColor(args.vg, valueColor);
            nvgFill(args.vg);
            nvgClosePath(args.vg);

            // Draw overshoot segment (if any)
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

            float upperYPos = 12 + (-upperValue) * marksStep;  // Map LUFS to Y
            float lowerYPos = 12 + (-lowerValue) * marksStep;  // Map LUFS to Y

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
            // --- Draw Default State for Bar (when module/pointers are null) ---
            // Option 1: Draw nothing dynamic (simplest)
            // Option 2: Draw a minimal bar at the bottom (-60 LUFS)
            float defaultBarHeight = 1.f;
            float defaultYOffset = box.size.y - defaultBarHeight - marginBottom;
            nvgBeginPath(args.vg);
            nvgRect(args.vg, 0.5 * (box.size.x - barWidth), defaultYOffset, barWidth, defaultBarHeight);
            nvgFillColor(args.vg, valueColor);  // Or a dimmed color like grey
            nvgFill(args.vg);
            nvgClosePath(args.vg);
            // Option 3: Draw a specific "inactive" symbol (more complex)
        }

        // --- Draw Unit and Label (Always) ---
        nvgFontFaceId(args.vg, font->handle);  // Use main font for unit
        nvgFontSize(args.vg, 14);
        nvgFillColor(args.vg, valueColor);
        nvgTextAlign(args.vg, NVG_ALIGN_TOP | NVG_ALIGN_CENTER);
        nvgText(args.vg, box.size.x * 0.5, box.size.y - 35, unit.c_str(), NULL);

        nvgFontFaceId(args.vg, font2->handle);  // Use secondary font for label
        nvgFontSize(args.vg, 14);
        nvgFillColor(args.vg, labelColor);
        nvgTextAlign(args.vg, NVG_ALIGN_BASELINE | NVG_ALIGN_CENTER);
        nvgText(args.vg, box.size.x * 0.5, box.size.y - 11, label.c_str(), NULL);
    }
};

// Custom display widget (reusable)
struct ValueDisplayWidget : TransparentWidget {
    std::shared_ptr<Font> font;
    std::shared_ptr<Font> font2;
    NVGcolor textColor = nvgRGB(0x00, 0xbf, 0xff);
    NVGcolor valueColor = nvgRGB(0xff, 0xff, 0xff);
    NVGcolor labelColor = nvgRGB(0x00, 0xbf, 0xff);
    std::string label;
    float* valuePtr = nullptr;
    std::string unit = "";

    ValueDisplayWidget() {
        font = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/JetBrainsMono-Medium.ttf"));
        font2 = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/SofiaSansExtraCondensed-Regular.ttf"));
    }

    void draw(const DrawArgs& args) override {
        // Check fonts first, as they are needed for everything
        if (!font || !font2) return;  // Cannot draw text without fonts

        float middleY = box.size.y * 0.5f;
        // drawDebugFrame(args.vg, box);

        // --- Determine Value String ---
        std::string valueText = "-inf";  // Default value string
        bool drawDash = false;           // Default state for drawing dash

        // Only try to read value if module and pointer are valid
        if (valuePtr) {
            float currentValue = *valuePtr;
            bool cond1 = currentValue <= ALMOST_NEGATIVE_INFINITY || std::isinf(currentValue) || std::isnan(currentValue);
            bool cond2 = (label == "LOUDNESS RANGE") && currentValue <= 0.0f;

            if (cond1 || cond2) {
                // Use the dash representation for these specific conditions
                drawDash = true;
            } else {
                // Format the valid number
                valueText = formatValue(currentValue);
            }
        } else {
            // Module or valuePtr is null, keep default "-inf" or decide on another default
            // If you want 0.0 instead of -inf:
            // valueText = formatValue(0.0f);
            // If you specifically want the dash for null module:
            drawDash = true;  // Or choose "-inf" text by not setting drawDash = true
            valueText = "";   // Don't draw text if drawing dash
        }

        // --- Draw Value ---
        nvgFontFaceId(args.vg, font->handle);
        if (drawDash) {
            nvgStrokeColor(args.vg, nvgRGB(0xff, 0xff, 0xff));
            nvgStrokeWidth(args.vg, 2.1f);
            nvgBeginPath(args.vg);
            nvgMoveTo(args.vg, box.size.x - 9.5, middleY - 13.0);
            nvgLineTo(args.vg, box.size.x - 29.5, middleY - 13.0);
            nvgStroke(args.vg);
        } else {
            nvgFontSize(args.vg, 32);
            nvgFillColor(args.vg, valueColor);
            nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE);
            nvgText(args.vg, box.size.x - 9.5, middleY - 5.0, valueText.c_str(), NULL);
        }

        // --- Draw Unit (Always) ---
        nvgFontSize(args.vg, 14);
        nvgFillColor(args.vg, valueColor);
        nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgText(args.vg, box.size.x - 9.5, middleY, unit.c_str(), NULL);

        // --- Draw Label (Always) ---
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

        // Use pixel coordinates directly in Vec()
        // box.size.x is already in pixels
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

        ValueDisplayWidget* mMaxDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px + displayWidthPx, yStart + 2 * yStep));
        mMaxDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            mMaxDisplay->valuePtr = &module->maxMomentaryLufs;
        }
        mMaxDisplay->label = "MOMENTARY MAX";
        mMaxDisplay->unit = "LUFS";
        addChild(mMaxDisplay);

        ValueDisplayWidget* sMaxDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px, yStart + 3 * yStep));
        sMaxDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            sMaxDisplay->valuePtr = &module->maxShortTermLufs;
        }
        sMaxDisplay->label = "SHORT TERM MAX";
        sMaxDisplay->unit = "LUFS";
        addChild(sMaxDisplay);

        ValueDisplayWidget* tpmDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px + displayWidthPx, yStart + 3 * yStep));
        tpmDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
        if (module) {
            tpmDisplay->valuePtr = &module->truePeakMax;
        }
        tpmDisplay->label = "TRUE PEAK MAX";
        tpmDisplay->unit = "dB";
        addChild(tpmDisplay);
    }
};

Model* modelLoudnessMeter = createModel<LoudnessMeter, LoudnessMeterWidget>("LoudnessMeter");