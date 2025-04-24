#pragma once  // Use include guards
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "plugin.hpp"

// Include the dependency header
#include "ebur128.h"

using namespace rack;

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
    static const float defaultTarget = -23.f;
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