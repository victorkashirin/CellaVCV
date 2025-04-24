#pragma once  // Use include guards
#include <cmath>
#include <deque>  // Make sure deque is included
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

    // Enum for processing modes
    enum ProcessingMode {
        TRUE_AUTO = 0,    // True Mono/Stereo based on connections
        FORCE_MONO = 1,   // Always process as Mono (mix down if stereo input)
        FORCE_STEREO = 2  // Always process as Stereo (duplicate if mono input)
    };

    // --- Configuration ---
    static const size_t PROCESSING_BLOCK_FRAMES = 2048;

    // --- libebur128 State ---
    ebur128_state* ebur128_handle = nullptr;
    size_t currentInputChannels = 0;  // 0: inactive, 1: mono, 2: stereo (as initialized in ebur128)

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
    static constexpr float defaultTarget = -23.f;
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

    // --- Mode State ---
    int processingMode = TRUE_AUTO;
    int previousProcessingMode = -1;  // Initialize to invalid state to force initial check

    LoudnessMeter() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(AUDIO_INPUT_L, "Audio L / Mono");
        configInput(AUDIO_INPUT_R, "Audio R");
        configInput(RESET_INPUT, "Reset");
        configParam(TARGET_PARAM, -36.f, 0.f, -23.f, "Target loudness", " LUFS");
        configParam(RESET_PARAM, 0.f, 1.f, 0.f, "Reset");

        processingBuffer.resize(PROCESSING_BLOCK_FRAMES * 2);  // Max 2 channels needed for forced stereo

        // Initialize state
        calculate_history_size();
        resetMeter();
    }

    ~LoudnessMeter() override {
        if (ebur128_handle) {
            // Process any remaining samples in the buffer before destroying
            // Don't process here, resetMeter already handles this if called before destruction
            ebur128_destroy(&ebur128_handle);
        }
    }

    void calculate_history_size() {
        int chunkSizeFrames = PROCESSING_BLOCK_FRAMES;
        float sampleRate = APP->engine->getSampleRate();
        if (sampleRate > 0 && chunkSizeFrames > 0) {
            peakHistorySizeChunks = static_cast<size_t>(
                std::ceil(PEAK_HISTORY_SECONDS * sampleRate / (float)chunkSizeFrames));
            if (peakHistorySizeChunks < 5) peakHistorySizeChunks = 5;
        } else {
            peakHistorySizeChunks = 100;  // Default if sample rate/chunk size invalid
        }
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
        return foundValid ? maxVal : -INFINITY;
    }

    // Function to determine the number of channels ebur128 *should* be initialized with
    // based on the current mode and physical input connections.
    size_t calculateEffectiveChannels() {
        bool leftConnected = inputs[AUDIO_INPUT_L].isConnected();
        bool rightConnected = inputs[AUDIO_INPUT_R].isConnected();
        size_t channels = 0;

        switch (processingMode) {
            case TRUE_AUTO:
                if (leftConnected && rightConnected)
                    channels = 2;
                else if (leftConnected || rightConnected)
                    channels = 1;
                else
                    channels = 0;
                break;
            case FORCE_MONO:
                if (leftConnected || rightConnected)
                    channels = 1;
                else
                    channels = 0;
                break;
            case FORCE_STEREO:
                // Force stereo requires 2 channels if *any* input is connected
                if (leftConnected || rightConnected)
                    channels = 2;
                else
                    channels = 0;
                break;
        }
        return channels;
    }

    void processBlockBuffer() {
        if (!ebur128_handle || bufferPosition == 0) {
            return;
        }

        size_t framesToProcess = bufferPosition;
        int err = ebur128_add_frames_float(ebur128_handle, processingBuffer.data(), framesToProcess);
        bufferPosition = 0;

        if (err != EBUR128_SUCCESS) {
            DEBUG("LoudnessMeter: Error adding frames to ebur128: %d", err);
            resetMeter();
            return;
        }

        updateLoudnessValues();
    }

    void updateLoudnessValues() {
        if (!ebur128_handle || peakHistorySizeChunks == 0) return;

        int err;
        double loudnessValue;
        double peakValue;

        err = ebur128_loudness_momentary(ebur128_handle, &loudnessValue);
        momentaryLufs = (err == EBUR128_SUCCESS && loudnessValue > -70.0) ? (float)loudnessValue : -INFINITY;  // Use lib threshold
        if (momentaryLufs > maxMomentaryLufs && momentaryLufs > ALMOST_NEGATIVE_INFINITY) {
            maxMomentaryLufs = momentaryLufs;
        }

        if (shortTermEnabled) {
            err = ebur128_loudness_shortterm(ebur128_handle, &loudnessValue);
            shortTermLufs = (err == EBUR128_SUCCESS && loudnessValue > -70.0) ? (float)loudnessValue : -INFINITY;
            if (shortTermLufs > maxShortTermLufs && shortTermLufs > ALMOST_NEGATIVE_INFINITY) {
                maxShortTermLufs = shortTermLufs;
            }
        } else {
            shortTermLufs = -INFINITY;
            maxShortTermLufs = -INFINITY;  // Also reset max if disabled
        }

        err = ebur128_loudness_global(ebur128_handle, &loudnessValue);
        integratedLufs = (err == EBUR128_SUCCESS && loudnessValue > -70.0) ? (float)loudnessValue : -INFINITY;

        double low_en, high_en;
        err = ebur128_loudness_range_ext(ebur128_handle, &loudnessValue, &low_en, &high_en);
        if (err == EBUR128_SUCCESS) {
            loudnessRange = (float)loudnessValue;
            loudnessRangeLow = (float)low_en;
            loudnessRangeHigh = (float)high_en;
        } else {
            loudnessRange = -INFINITY;
            loudnessRangeLow = -INFINITY;
            loudnessRangeHigh = -INFINITY;
        }

        // --- Get Current True Peak Values (based on initialized channels) ---
        float currentPeakL = -INFINITY;
        float currentPeakR = -INFINITY;

        if (currentInputChannels >= 1) {                                  // Check if ebur128 is initialized for at least 1 channel
            err = ebur128_prev_true_peak(ebur128_handle, 0, &peakValue);  // Channel 0 (Left or Mono)
            if (err == EBUR128_SUCCESS) {
                float linearAmp = (float)peakValue;
                if (linearAmp > LOG_EPSILON) {
                    currentPeakL = 20.0f * std::log10(linearAmp);
                }
                if (currentPeakL > maxTruePeakL && currentPeakL > ALMOST_NEGATIVE_INFINITY) {
                    maxTruePeakL = currentPeakL;
                }
            }
        }
        if (currentInputChannels == 2) {                                  // Check if ebur128 is initialized for 2 channels
            err = ebur128_prev_true_peak(ebur128_handle, 1, &peakValue);  // Channel 1 (Right)
            if (err == EBUR128_SUCCESS) {
                float linearAmp = (float)peakValue;
                if (linearAmp > LOG_EPSILON) {
                    currentPeakR = 20.0f * std::log10(linearAmp);
                }
                if (currentPeakR > maxTruePeakR && currentPeakR > ALMOST_NEGATIVE_INFINITY) {
                    maxTruePeakR = currentPeakR;
                }
            }
        } else if (currentInputChannels == 1) {
            // If mono, the concept of a separate right peak doesn't exist in the analysis.
            // Max True Peak should reflect the single channel's peak.
            currentPeakR = currentPeakL;  // For calculating max below, but maxTruePeakR itself isn't directly relevant.
            maxTruePeakR = -INFINITY;     // Reset max R peak if we switched to mono
        }

        // --- PSR Calculation ---
        float currentMaxTruePeak = -INFINITY;
        if (currentInputChannels == 1) {
            currentMaxTruePeak = currentPeakL;
        } else if (currentInputChannels == 2) {
            currentMaxTruePeak = std::fmax(currentPeakL, currentPeakR);
        }

        if (currentMaxTruePeak > ALMOST_NEGATIVE_INFINITY) {  // Only add valid peaks
            chunkPeakHistoryDB.push_back(currentMaxTruePeak);
        }
        while (chunkPeakHistoryDB.size() > peakHistorySizeChunks) {
            chunkPeakHistoryDB.pop_front();
        }

        float peakOverWindowDB = get_max_from_deque(chunkPeakHistoryDB);
        truePeakSlidingMax = peakOverWindowDB;

        if (peakOverWindowDB > ALMOST_NEGATIVE_INFINITY && shortTermLufs > ALMOST_NEGATIVE_INFINITY) {
            psrValue = peakOverWindowDB - shortTermLufs;
        } else {
            psrValue = -INFINITY;
        }

        // Update overall max true peak
        maxTruePeakL = std::fmax(maxTruePeakL, currentPeakL);
        if (currentInputChannels == 2) {
            maxTruePeakR = std::fmax(maxTruePeakR, currentPeakR);
            truePeakMax = std::fmax(maxTruePeakL, maxTruePeakR);
        } else {
            truePeakMax = maxTruePeakL;  // Use only L max if mono
        }

        // --- PLR Calculation ---
        if (truePeakMax > ALMOST_NEGATIVE_INFINITY && integratedLufs > ALMOST_NEGATIVE_INFINITY) {
            plrValue = truePeakMax - integratedLufs;
        } else {
            plrValue = -INFINITY;
        }
    }

    void resetMeter() {
        // Process any remaining samples in the buffer before destroying the handle
        if (ebur128_handle && bufferPosition > 0) {
            processBlockBuffer();
        }

        if (ebur128_handle) {
            ebur128_destroy(&ebur128_handle);
            ebur128_handle = nullptr;
        }

        // Determine the effective number of channels for the *new* state
        size_t effectiveChannels = calculateEffectiveChannels();
        float sampleRate = APP->engine->getSampleRate();

        // 4. Initialize ebur128 with the new configuration
        if (effectiveChannels > 0 && sampleRate > 0) {
            ebur128_handle = ebur128_init(
                effectiveChannels,
                (size_t)sampleRate,
                EBUR128_MODE_M | EBUR128_MODE_S |
                    EBUR128_MODE_I | EBUR128_MODE_LRA |
                    EBUR128_MODE_HISTOGRAM | EBUR128_MODE_TRUE_PEAK);

            if (!ebur128_handle) {
                DEBUG("LoudnessMeter: Failed to re-initialize ebur128");
                currentInputChannels = 0;  // Mark as inactive if init failed
            } else {
                currentInputChannels = effectiveChannels;  // Store the successfully initialized channel count
            }
        } else {
            currentInputChannels = 0;  // Mark as inactive if no channels or invalid sample rate
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
        previousProcessingMode = processingMode;
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
        bool manualReset = resetTrigger.process(params[RESET_PARAM].getValue()) || resetPortTrigger.process(inputs[RESET_INPUT].getVoltage());
        if (manualReset) {
            resetMeter();
        }

        if (processingMode != previousProcessingMode) {
            resetMeter();
        }

        //  Check for Input Connection Changes Requiring Reset ---
        size_t expectedChannels = calculateEffectiveChannels();
        if (expectedChannels != currentInputChannels && !manualReset) {  // Avoid double reset if manual reset already happened
            resetMeter();
        }

        targetLoudness = params[TARGET_PARAM].getValue();

        // Handle No Input / Inactive State ---
        if (!ebur128_handle || currentInputChannels == 0) {
            momentaryLufs = -INFINITY;
            shortTermLufs = -INFINITY;
            loudnessRange = -INFINITY;
            loudnessRangeHigh = -INFINITY;
            loudnessRangeLow = -INFINITY;
            psrValue = -INFINITY;
            truePeakSlidingMax = -INFINITY;

            if (!chunkPeakHistoryDB.empty()) {
                chunkPeakHistoryDB.push_back(-INFINITY);  // Push -inf to represent silence
                while (chunkPeakHistoryDB.size() > peakHistorySizeChunks) {
                    chunkPeakHistoryDB.pop_front();
                }
                truePeakSlidingMax = get_max_from_deque(chunkPeakHistoryDB);
            }

            return;
        }

        // Get Input Samples and Prepare for Processing ---
        float rawLeft = inputs[AUDIO_INPUT_L].getVoltage() * VOLTAGE_SCALE;
        float rawRight = inputs[AUDIO_INPUT_R].getVoltage() * VOLTAGE_SCALE;
        bool leftConnected = inputs[AUDIO_INPUT_L].isConnected();
        bool rightConnected = inputs[AUDIO_INPUT_R].isConnected();

        float leftToProcess = 0.f;
        float rightToProcess = 0.f;  // Only used if currentInputChannels == 2

        // Determine the actual samples to feed based on the initialized channel count
        // and the rules of the current processing mode.
        if (currentInputChannels == 1) {
            // We need a single mono sample.
            // FORCE_MONO or TRUE_AUTO (when only one input is connected)
            if (leftConnected && rightConnected) {  // Mix down (Only happens in FORCE_MONO mode logically)
                leftToProcess = (rawLeft + rawRight) * 0.5f;
            } else if (leftConnected) {  // Only L connected
                leftToProcess = rawLeft;
            } else {  // Only R connected (or neither, but handled by inactive state)
                leftToProcess = rawRight;
            }
        } else {  // currentInputChannels == 2
            // We need two samples (L/R).
            // FORCE_STEREO or TRUE_AUTO (when both inputs are connected)
            if (leftConnected && rightConnected) {  // Normal stereo
                leftToProcess = rawLeft;
                rightToProcess = rawRight;
            } else if (leftConnected) {  // Only L connected, need stereo (FORCE_STEREO mode)
                leftToProcess = rawLeft;
                rightToProcess = rawLeft;  // Duplicate L -> R
            } else {                       // Only R connected, need stereo (FORCE_STEREO mode)
                leftToProcess = rawRight;  // Duplicate R -> L
                rightToProcess = rawRight;
            }
        }

        // Add Sample(s) to Internal Buffer ---
        if (bufferPosition < PROCESSING_BLOCK_FRAMES) {
            size_t bufferBaseIndex = bufferPosition * currentInputChannels;
            processingBuffer[bufferBaseIndex] = leftToProcess;
            if (currentInputChannels == 2) {
                processingBuffer[bufferBaseIndex + 1] = rightToProcess;
            }
            bufferPosition++;
        }

        // --- 8. Process Block Buffer if Full ---
        if (bufferPosition >= PROCESSING_BLOCK_FRAMES) {
            processBlockBuffer();
        }
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "integratedLufs", json_real(integratedLufs));
        json_object_set_new(rootJ, "processingMode", json_integer(processingMode));
        json_object_set_new(rootJ, "shortTermEnabled", json_boolean(shortTermEnabled));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* integratedLufsJ = json_object_get(rootJ, "integratedLufs");
        if (integratedLufsJ) integratedLufs = json_real_value(integratedLufsJ);

        json_t* processingModeJ = json_object_get(rootJ, "processingMode");
        if (processingModeJ) {
            processingMode = json_integer_value(processingModeJ);
        } else {
            processingMode = TRUE_AUTO;
        }
        previousProcessingMode = processingMode;  // Sync previous mode to loaded state

        json_t* shortTermEnabledJ = json_object_get(rootJ, "shortTermEnabled");
        if (shortTermEnabledJ) shortTermEnabled = json_boolean_value(shortTermEnabledJ);

        bufferPosition = 0;
        momentaryLufs = -INFINITY;
        shortTermLufs = -INFINITY;
        loudnessRange = -INFINITY;
        psrValue = -INFINITY;
        plrValue = -INFINITY;
        truePeakSlidingMax = -INFINITY;
    }
};