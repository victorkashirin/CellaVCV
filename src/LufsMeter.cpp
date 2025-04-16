#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "plugin.hpp"

// Include the dependency header
#include "ebur128.h"

using namespace rack;

struct LufsMeterWidget;  // Forward declaration

const float ALMOST_NEGATIVE_INFINITY = -99.0f;
const float VOLTAGE_SCALE = 0.1f;  // Scale +/-10V to +/-1.0f

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
    static const size_t PROCESSING_BLOCK_FRAMES = 256;

    // How often to update the display (reduces GUI overhead)
    // Update roughly 15 times per second (e.g., 48000Hz / 15Hz ≈ 3200 samples)
    // Make it a multiple of PROCESSING_BLOCK_FRAMES if possible
    static const int DISPLAY_UPDATE_INTERVAL_FRAMES = 3072;  // 12 * 256

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

    // --- Values tracked manually for PSR calculation ---
    float maxShortTermLufs = -INFINITY;
    float maxTruePeakL = -INFINITY;
    float maxTruePeakR = -INFINITY;

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
    }

    ~LufsMeter() override {
        if (ebur128_handle) {
            // Optionally process any remaining samples in the buffer before destroying
            // processBlockBuffer();
            ebur128_destroy(&ebur128_handle);
        }
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
        if (!ebur128_handle) return;

        int err;
        double loudness_value;
        double peak_value;

        err = ebur128_loudness_momentary(ebur128_handle, &loudness_value);
        momentaryLufs = (err == EBUR128_SUCCESS) ? (float)loudness_value : -INFINITY;

        err = ebur128_loudness_shortterm(ebur128_handle, &loudness_value);
        if (err == EBUR128_SUCCESS) {
            float currentShortTerm = (float)loudness_value;
            shortTermLufs = currentShortTerm;
            // Update Max Short Term LUFS
            if (currentShortTerm > maxShortTermLufs && currentShortTerm > ALMOST_NEGATIVE_INFINITY) {
                maxShortTermLufs = currentShortTerm;
            }
        } else {
            shortTermLufs = -INFINITY;
        }

        err = ebur128_loudness_global(ebur128_handle, &loudness_value);
        if (err == EBUR128_SUCCESS) {
            integratedLufs = (float)loudness_value;
        }  // Else: Keep old value

        err = ebur128_loudness_range(ebur128_handle, &loudness_value);
        if (err == EBUR128_SUCCESS) {
            loudnessRange = (float)loudness_value;
        }  // Else: Keep old value

        // --- Update True Peak and Max Peaks ---
        if (currentInputChannels >= 1) {
            err = ebur128_true_peak(ebur128_handle, 0, &peak_value);
            if (err == EBUR128_SUCCESS) {
                float currentPeakL = (float)peak_value;
                if (currentPeakL > maxTruePeakL && currentPeakL > ALMOST_NEGATIVE_INFINITY) {
                    maxTruePeakL = currentPeakL;
                }
            }
        }
        if (currentInputChannels == 2) {
            err = ebur128_true_peak(ebur128_handle, 1, &peak_value);
            if (err == EBUR128_SUCCESS) {
                float currentPeakR = (float)peak_value;
                if (currentPeakR > maxTruePeakR && currentPeakR > ALMOST_NEGATIVE_INFINITY) {
                    maxTruePeakR = currentPeakR;
                }
            }
        }

        // --- Calculate PSR ---
        float overallMaxTruePeak = std::fmax(maxTruePeakL, maxTruePeakR);
        if (overallMaxTruePeak > ALMOST_NEGATIVE_INFINITY && maxShortTermLufs > ALMOST_NEGATIVE_INFINITY) {
            psrValue = overallMaxTruePeak - maxShortTermLufs;
        } else {
            psrValue = -INFINITY;  // Not enough data
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
                    EBUR128_MODE_LRA | EBUR128_MODE_TRUE_PEAK);

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

        // Reset ALL displayed values and tracked maximums
        momentaryLufs = -INFINITY;
        shortTermLufs = -INFINITY;
        integratedLufs = -INFINITY;
        loudnessRange = -INFINITY;
        psrValue = -INFINITY;
        maxShortTermLufs = -INFINITY;
        maxTruePeakL = -INFINITY;
        maxTruePeakR = -INFINITY;
    }

    void onReset(const ResetEvent& e) override {
        Module::onReset(e);
        resetMeter();
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        Module::onSampleRateChange(e);
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
        maxShortTermLufs = -INFINITY;
        maxTruePeakL = -INFINITY;
        maxTruePeakR = -INFINITY;
        // libebur128 state is reset via onSampleRateChange/constructor during load.
    }
};

//-----------------------------------------------------------------------------
// Helper Functions and Widgets for UI (Mostly Unchanged)
//-----------------------------------------------------------------------------

// Generic number formatter (handles LUFS, LU, dB)
static std::string formatValue(float value, const char* unit = "") {
    if (value <= ALMOST_NEGATIVE_INFINITY || std::isinf(value) || std::isnan(value)) {
        return "-inf";
    }
    char buf[25];
    snprintf(buf, sizeof(buf), "%.1f%s", value, unit);
    return std::string(buf);
}

// Custom display widget (reusable)
struct ValueDisplayWidget : TransparentWidget {
    // ... (Keep the previous ValueDisplayWidget implementation) ...
    LufsMeter* module = nullptr;
    std::shared_ptr<Font> font;
    NVGcolor textColor = nvgRGB(0xdf, 0xdf, 0xdf);   // Light text
    NVGcolor labelColor = nvgRGB(0x90, 0x90, 0x90);  // Dimmer label
    std::string label;
    float* valuePtr = nullptr;  // Pointer to M, S, I, LRA, PSR value in the module
    std::string unit = "";      // Unit string (e.g., " LU", " dB")

    ValueDisplayWidget() {
        font = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/Segment7.ttf"));  // Make sure you have this font
        if (!font) {
            font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));  // Fallback font
        }
    }

    void draw(const DrawArgs& args) override {
        if (!module || !valuePtr || !font) return;

        nvgFontFaceId(args.vg, font->handle);
        float middleY = box.size.y * 0.5f;

        // Draw Value (Right-aligned)
        nvgFontSize(args.vg, 16);  // Adjusted size to fit more displays
        nvgFillColor(args.vg, textColor);
        nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        std::string text = formatValue(*valuePtr, unit.c_str());
        nvgText(args.vg, box.size.x - 3, middleY, text.c_str(), NULL);

        // Draw Label (Left-aligned)
        nvgFontSize(args.vg, 10);  // Smaller size for label
        nvgFillColor(args.vg, labelColor);
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(args.vg, 3, middleY, label.c_str(), NULL);
    }
};

struct LufsMeterWidget : ModuleWidget {
    LufsMeterWidget(LufsMeter* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/LufsMeterPanel.svg")));

        // Screws remain the same, using Rack constants (which are in pixels)
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Define positions and sizes directly in pixels
        // Assuming these were the intended pixel values previously passed to mm2px
        float displayHeightPx = 16.f;
        float displayMarginYPx = 2.f;
        float displayY_M_Px = 10.f;
        float displayY_S_Px = displayY_M_Px + displayHeightPx + displayMarginYPx;
        float displayY_I_Px = displayY_S_Px + displayHeightPx + displayMarginYPx;
        float displayY_LRA_Px = displayY_I_Px + displayHeightPx + displayMarginYPx;
        float displayY_PSR_Px = displayY_LRA_Px + displayHeightPx + displayMarginYPx;
        float displayX_Px = 3.f;
        // displayWidthPx uses box.size.x which is already in pixels
        float displayWidthPx = box.size.x - (2 * displayX_Px);  // Use displayX_Px for margin on both sides

        float inputYPx = 112.f;
        float resetYPx = 95.f;

        // Use pixel coordinates directly in Vec()
        // box.size.x is already in pixels
        addInput(createInputCentered<PJ301MPort>(Vec(box.size.x * 0.25f, inputYPx), module, LufsMeter::AUDIO_INPUT_L));
        addInput(createInputCentered<PJ301MPort>(Vec(box.size.x * 0.75f, inputYPx), module, LufsMeter::AUDIO_INPUT_R));
        addParam(createParamCentered<VCVButton>(Vec(box.size.x / 2.f, resetYPx), module, LufsMeter::RESET_PARAM));

        // Use the calculated pixel values for position and size
        if (module) {
            ValueDisplayWidget* momentaryDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px, displayY_M_Px));
            momentaryDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
            momentaryDisplay->module = module;
            momentaryDisplay->valuePtr = &module->momentaryLufs;
            momentaryDisplay->label = "M";
            momentaryDisplay->unit = " LUFS";
            addChild(momentaryDisplay);

            ValueDisplayWidget* shortTermDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px, displayY_S_Px));
            shortTermDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
            shortTermDisplay->module = module;
            shortTermDisplay->valuePtr = &module->shortTermLufs;
            shortTermDisplay->label = "S";
            shortTermDisplay->unit = " LUFS";
            addChild(shortTermDisplay);

            ValueDisplayWidget* integratedDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px, displayY_I_Px));
            integratedDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
            integratedDisplay->module = module;
            integratedDisplay->valuePtr = &module->integratedLufs;
            integratedDisplay->label = "I";
            integratedDisplay->unit = " LUFS";
            addChild(integratedDisplay);

            ValueDisplayWidget* lraDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px, displayY_LRA_Px));
            lraDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
            lraDisplay->module = module;
            lraDisplay->valuePtr = &module->loudnessRange;
            lraDisplay->label = "LRA";
            lraDisplay->unit = " LU";
            addChild(lraDisplay);

            ValueDisplayWidget* psrDisplay = createWidget<ValueDisplayWidget>(Vec(displayX_Px, displayY_PSR_Px));
            psrDisplay->box.size = Vec(displayWidthPx, displayHeightPx);
            psrDisplay->module = module;
            psrDisplay->valuePtr = &module->psrValue;
            psrDisplay->label = "PSR";
            psrDisplay->unit = " dB";
            addChild(psrDisplay);
        }
    }
};
//-----------------------------------------------------------------------------
// Plugin Registration (in YourPlugin.cpp)
//-----------------------------------------------------------------------------
Model* modelLufsMeter = createModel<LufsMeter, LufsMeterWidget>("LufsMeter");