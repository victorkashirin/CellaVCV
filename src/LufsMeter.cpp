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

struct LufsMeterWidget;  // Forward declaration

const float ALMOST_NEGATIVE_INFINITY = -99.0f;
const float VOLTAGE_SCALE = 0.1f;  // Scale +/-10V to +/-1.0f
const float LOG_EPSILON = 1e-10f;  // Or adjust as needed

//-----------------------------------------------------------------------------
// Module Logic: LufsMeter (Corrected with Buffering)
//-----------------------------------------------------------------------------
struct LufsMeter : engine::Module {
    enum ParamIds { RESET_PARAM,
                    NUM_PARAMS };
    enum InputIds { AUDIO_INPUT_L,
                    AUDIO_INPUT_R,
                    NUM_INPUTS };
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
    float psrValue = -INFINITY;
    float plrValue = -INFINITY;
    float truePeakMax = -INFINITY;
    float truePeakSlidingMax = -INFINITY;

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
    dsp::ClockDivider displayUpdateDivider;

    LufsMeter() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(AUDIO_INPUT_L, "Audio L / Mono");
        configInput(AUDIO_INPUT_R, "Audio R");
        configParam(RESET_PARAM, 0.f, 1.f, 0.f, "Reset Integrated, LRA, PSR");

        // Allocate buffer based on max channels (stereo)
        processingBuffer.resize(PROCESSING_BLOCK_FRAMES * 2);  // Max 2 channels

        displayUpdateDivider.setDivision(DISPLAY_UPDATE_INTERVAL_FRAMES);

        // Initialize state
        resetMeter();
        calculate_history_size();
    }

    ~LufsMeter() override {
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
            std::cerr << "LufsMeter: Error adding frames: " << err << std::endl;
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

        err = ebur128_loudness_range(ebur128_handle, &loudnessValue);
        if (err == EBUR128_SUCCESS) {
            loudnessRange = (float)loudnessValue;
        }  // Else: Keep old value

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
        // Note: You still have maxTruePeakL, maxTruePeakR, and maxShortTermLufs if you
        // want to display those historical maximums separately elsewhere.
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
                    EBUR128_MODE_LRA | EBUR128_MODE_SAMPLE_PEAK | EBUR128_MODE_TRUE_PEAK);

            if (!ebur128_handle) {
                std::cerr << "LufsMeter: Failed to re-initialize ebur128\n";
                currentInputChannels = 0;
            } else {
                currentInputChannels = channels;
            }
        } else {
            currentInputChannels = 0;
            ebur128_handle = nullptr;
        }

        // Clear internal buffer state
        bufferPosition = 0;
        // std::fill(processingBuffer.begin(), processingBuffer.end(), 0.f); // Optional: zero out buffer
        chunkPeakHistoryDB.clear();

        // Reset ALL displayed values and tracked maximums
        momentaryLufs = -INFINITY;
        shortTermLufs = -INFINITY;
        integratedLufs = -INFINITY;
        loudnessRange = -INFINITY;
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

    // **Corrected Process Method (Sample-by-Sample)**
    void process(const ProcessArgs& args) override {
        // --- 1. Check for Manual Reset ---
        if (resetTrigger.process(params[RESET_PARAM].getValue())) {
            resetMeter();
            // No return here, allow processing to continue from fresh state if needed
        }

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

        // --- 4. Process Buffer When Full ---
        if (bufferPosition >= PROCESSING_BLOCK_FRAMES) {
            processBlockBuffer();
            // Note: updateLoudnessValues() is called inside processBlockBuffer
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
        // libebur128 state is reset via onSampleRateChange/constructor during load.
    }
};

//-----------------------------------------------------------------------------
// Helper Functions and Widgets for UI (Mostly Unchanged)
//-----------------------------------------------------------------------------

// Generic number formatter (handles LUFS, LU, dB)
static std::string formatValue(float value) {
    if (value <= ALMOST_NEGATIVE_INFINITY || std::isinf(value) || std::isnan(value)) {
        return "-inf";
    }
    char buf[25];
    snprintf(buf, sizeof(buf), "%.1f", value);
    return std::string(buf);
}

// Custom display widget (reusable)
struct ValueDisplayWidget : TransparentWidget {
    LufsMeter* module = nullptr;
    std::shared_ptr<Font> font;
    NVGcolor textColor = nvgRGB(0x00, 0xbf, 0xff);   // Light text
    NVGcolor valueColor = nvgRGB(0xff, 0xff, 0xff);  // Light text
    NVGcolor labelColor = nvgRGB(0x00, 0xbf, 0xff);  // Dimmer label
    std::string label;
    float* valuePtr = nullptr;  // Pointer to M, S, I, LRA, PSR value in the module
    std::string unit = "";      // Unit string (e.g., " LU", " dB")

    ValueDisplayWidget() {
        // font = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/Iosevka-Regular.ttf"));
        font = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/JetBrainsMono-Medium.ttf"));
        if (!font) {
            font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));  // Fallback font
        }
    }

    void draw(const DrawArgs& args) override {
        if (!module || !valuePtr || !font) return;

        nvgFontFaceId(args.vg, font->handle);
        float middleY = box.size.y * 0.5f;

        // nvgStrokeColor(args.vg, valueColor);
        // nvgStrokeWidth(args.vg, 0.2f);
        // nvgBeginPath(args.vg);
        // nvgMoveTo(args.vg, 0, 0);
        // nvgLineTo(args.vg, box.size.x, 0);
        // nvgLineTo(args.vg, box.size.x, box.size.y);
        // nvgLineTo(args.vg, 0, box.size.y);
        // nvgLineTo(args.vg, 0, 0);

        // nvgMoveTo(args.vg, 0, box.size.y * 0.5);
        // nvgLineTo(args.vg, box.size.x, box.size.y * 0.5);

        // nvgStroke(args.vg);

        // Draw Value (Right-aligned)
        nvgFontSize(args.vg, 21);  // Adjusted size to fit more displays
        nvgFillColor(args.vg, valueColor);
        nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
        std::string text = formatValue(*valuePtr);
        nvgText(args.vg, box.size.x - 40, middleY, text.c_str(), NULL);

        nvgFontSize(args.vg, 14);  // Adjusted size to fit more displays
        nvgFillColor(args.vg, valueColor);
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
        nvgText(args.vg, box.size.x - 43, middleY - 1.5, unit.c_str(), NULL);

        // Draw Label (Left-aligned)
        nvgFontSize(args.vg, 13);  // Smaller size for label
        nvgFillColor(args.vg, labelColor);
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_CENTER);
        nvgText(args.vg, 0.5 * box.size.x, 0.7 * box.size.y - 2, label.c_str(), NULL);

        // float lineY = middleY - 2;  // Adjust this offset as needed

        // nvgStrokeColor(args.vg, valueColor);  // Use a desired color for the line
        // nvgStrokeWidth(args.vg, 0.5f);
        // nvgBeginPath(args.vg);
        // nvgMoveTo(args.vg, 0, lineY);           // Start at left edge, at lineY
        // nvgLineTo(args.vg, box.size.x, lineY);  // Go to right edge, at lineY
        // nvgStroke(args.vg);
    }
};

struct LufsMeterWidget : ModuleWidget {
    LufsMeterWidget(LufsMeter* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/LufsMeter.svg"), asset::plugin(pluginInstance, "res/LufsMeter-dark.svg")));

        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        LedDisplay* ledDisplay = createWidget<LedDisplay>(Vec(0, 26));
        ledDisplay->box.size = Vec(225, 280);
        addChild(ledDisplay);

        // Define positions and sizes directly in pixels
        // Assuming these were the intended pixel values previously passed to mm2px
        float displayHeightPx = 70.f;
        float displayMarginYPx = 15.f;
        float yStep = displayHeightPx;
        float yStart = 26.f;
        float displayY_M_Px = yStart;
        float displayY_S = yStart + 1 * yStep;
        float displayY_I = yStart + 2 * yStep;
        float displayY_LRA = yStart + 3 * yStep;
        float displayY_PSR = yStart + 4 * yStep;
        float displayY_PLR = yStart + 5 * yStep;
        float displayY_MMAX = yStart + 6 * yStep;
        float displayY_SMAX = yStart + 7 * yStep;
        float displayY_TPM = yStart + 8 * yStep;
        float displayX_Px = 25.f;
        // displayWidthPx uses box.size.x which is already in pixels
        float displayWidthPx = 100;  // Use displayX_Px for margin on both sides

        float inputYPx = 329.25;

        // Use pixel coordinates directly in Vec()
        // box.size.x is already in pixels
        addInput(createInputCentered<PJ301MPort>(Vec(22.5f, inputYPx), module, LufsMeter::AUDIO_INPUT_L));
        addInput(createInputCentered<PJ301MPort>(Vec(67.5f, inputYPx), module, LufsMeter::AUDIO_INPUT_R));
        addParam(createParamCentered<VCVButton>(Vec(112.5f, inputYPx), module, LufsMeter::RESET_PARAM));

        // Use the calculated pixel values for position and size
        if (module) {
            // ValueDisplayWidget* momentaryDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px, displayY_M_Px));
            // momentaryDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
            // momentaryDisplay->module = module;
            // momentaryDisplay->valuePtr = &module->momentaryLufs;
            // momentaryDisplay->label = "MOMENTARY";
            // momentaryDisplay->unit = " LUFS";
            // addChild(momentaryDisplay);

            ValueDisplayWidget* shortTermDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px, yStart));
            shortTermDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
            shortTermDisplay->module = module;
            shortTermDisplay->valuePtr = &module->shortTermLufs;
            shortTermDisplay->label = "SHORT TERM";
            shortTermDisplay->unit = " LUFS";
            addChild(shortTermDisplay);

            ValueDisplayWidget* integratedDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px + displayWidthPx, yStart));
            integratedDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
            integratedDisplay->module = module;
            integratedDisplay->valuePtr = &module->integratedLufs;
            integratedDisplay->label = "INTEGRATED";
            integratedDisplay->unit = " LUFS";
            addChild(integratedDisplay);

            ValueDisplayWidget* lraDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px, yStart + 2 * yStep));
            lraDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
            lraDisplay->module = module;
            lraDisplay->valuePtr = &module->loudnessRange;
            lraDisplay->label = "LOUDNESS RANGE";
            lraDisplay->unit = " LU";
            addChild(lraDisplay);

            ValueDisplayWidget* psrDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px, yStart + 1 * yStep));
            psrDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
            psrDisplay->module = module;
            psrDisplay->valuePtr = &module->psrValue;
            psrDisplay->label = "DYNAMICS (PSR)";
            psrDisplay->unit = " LU";
            addChild(psrDisplay);

            ValueDisplayWidget* plrDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px + displayWidthPx, yStart + 1 * yStep));
            plrDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
            plrDisplay->module = module;
            plrDisplay->valuePtr = &module->plrValue;
            plrDisplay->label = "AVG. DYN. (PLR)";
            plrDisplay->unit = " LU";
            addChild(plrDisplay);

            ValueDisplayWidget* mMaxDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px + displayWidthPx, yStart + 2 * yStep));
            mMaxDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
            mMaxDisplay->module = module;
            mMaxDisplay->valuePtr = &module->maxMomentaryLufs;
            mMaxDisplay->label = "MOMENTARY MAX";
            mMaxDisplay->unit = " LUFS";
            addChild(mMaxDisplay);

            ValueDisplayWidget* sMaxDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px, yStart + 3 * yStep));
            sMaxDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
            sMaxDisplay->module = module;
            sMaxDisplay->valuePtr = &module->maxShortTermLufs;
            sMaxDisplay->label = "SHORT TERM MAX";
            sMaxDisplay->unit = " LUFS";
            addChild(sMaxDisplay);

            ValueDisplayWidget* tpmDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px + displayWidthPx, yStart + 3 * yStep));
            tpmDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
            tpmDisplay->module = module;
            tpmDisplay->valuePtr = &module->truePeakMax;
            tpmDisplay->label = "TRUE PEAK MAX";
            tpmDisplay->unit = " dB";
            addChild(tpmDisplay);
        }
    }
};
//-----------------------------------------------------------------------------
// Plugin Registration (in YourPlugin.cpp)
//-----------------------------------------------------------------------------
Model* modelLufsMeter = createModel<LufsMeter, LufsMeterWidget>("LufsMeter");