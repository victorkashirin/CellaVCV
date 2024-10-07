#include <vector>

#include "plugin.hpp"

template <typename T>
static T clip(T x) {
    // return std::tanh(x);
    // Pade approximant of tanh
    x = clamp(x, -3.f, 3.f);
    return x * (27 + x * x) / (27 + 9 * x * x);
}

struct RezoModule : Module {
    enum ParamIds {
        PITCH1_PARAM,
        AMP1_PARAM,
        PITCH2_PARAM,
        AMP2_PARAM,
        PITCH3_PARAM,
        AMP3_PARAM,
        PITCH4_PARAM,
        AMP4_PARAM,
        DECAY_PARAM,
        MIX_PARAM,  // Cutoff frequency for the filter
        TONE_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        IN_INPUT,  // Incoming signal
        NUM_INPUTS
    };
    enum OutputIds {
        OUT_OUTPUT,  // Resonated signal output
        NUM_OUTPUTS
    };
    enum LightIds {
        NUM_LIGHTS
    };

    // Define the filter types
    enum FilterTypes {
        FILTER_LP,  // Low-pass filter
        FILTER_HP,  // High-pass filter
        NUM_FILTERS
    };

    // Variables for filters
    std::vector<dsp::RCFilter> lowPassFilter;
    std::vector<dsp::RCFilter> highPassFilter;

    std::vector<std::vector<float>> delayBuffers;
    std::vector<int> delayIndices;
    std::vector<float> currentDelaySamples;
    std::vector<float> targetDelaySamples;

    int bufferSize = 4 * 4096;
    int writeIndex = 0;    // Index where new samples will be written
    int delaySamples = 0;  // Current delay length in samples

    // Variables for delay time interpolation
    // float currentDelaySamples = 0.0f;   // Current delay time in samples
    // float targetDelaySamples = 0.0f;    // Target delay time in samples
    float interpolationSpeed = 0.001f;  // Speed of interpolation

    float sampleRate = 44100.f;

    RezoModule() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(PITCH1_PARAM, -54.f, 54.f, 0.f, "Frequency 1", " Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
        configParam(AMP1_PARAM, 0.0, 2.0, 1.f, "Amplitude 1", " dB", -10, 20);
        configParam(PITCH2_PARAM, -54.f, 54.f, 0.f, "Frequency 2", " Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
        configParam(AMP2_PARAM, 0.0, 2.0, 1.f, "Amplitude 2", " dB", -10, 20);
        configParam(PITCH3_PARAM, -54.f, 54.f, 0.f, "Frequency 3", " Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
        configParam(AMP3_PARAM, 0.0, 2.0, 1.f, "Amplitude 3", " dB", -10, 20);
        configParam(PITCH4_PARAM, -54.f, 54.f, 0.f, "Frequency 4", " Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
        configParam(AMP4_PARAM, 0.0, 2.0, 1.f, "Amplitude 4", " dB", -10, 20);
        configParam(DECAY_PARAM, 0.7f, 0.9999f, 0.9f, "Decay");
        configParam(TONE_PARAM, 0.f, 1.f, 0.5f, "Tone", "%", 0, 200, -100);
        configParam(MIX_PARAM, 0.f, 1.f, 0.f, "Mix");
    }

    void onSampleRateChange() override {
        sampleRate = APP->engine->getSampleRate();
        delayBuffers.resize(4);
        lowPassFilter.resize(4);
        highPassFilter.resize(4);
        delayIndices.resize(4, 0);
        currentDelaySamples.resize(4, 0.f);
        targetDelaySamples.resize(4, 0.f);
        bufferSize = (int)(sampleRate * 0.1);

        for (auto& buffer : delayBuffers) {
            buffer.resize((int)bufferSize);
            std::fill(buffer.begin(), buffer.end(), 0.f);
        }
    }

    // Function to read from the delay buffer
    float readDelayBuffer(int bufferIndex, float delayTimeSamples) {
        // Compute the read index using the delay time (circular buffer)
        int readIndex = delayIndices[bufferIndex] - (int)delayTimeSamples;
        if (readIndex < 0) {
            readIndex += bufferSize;  // Wrap around
        }
        return delayBuffers[bufferIndex][readIndex % bufferSize];
    }

    // Function to write to the delay buffer
    void writeDelayBuffer(int bufferIndex, float value) {
        int writeIndex = delayIndices[bufferIndex];
        delayBuffers[bufferIndex][writeIndex] = value;              // Write the new sample
        delayIndices[bufferIndex] = (writeIndex + 1) % bufferSize;  // Increment and wrap around
    }

    void process(const ProcessArgs& args) override {
        float input = inputs[IN_INPUT].getVoltage();
        float mix = params[MIX_PARAM].getValue();
        float color = params[TONE_PARAM].getValue();
        float colorFreq = std::pow(100.f, 2.f * color - 1.f);
        float feedback = params[DECAY_PARAM].getValue();

        float sumOutput = 0.f;

        // Get pitch control (convert semitones to frequency)
        for (int i = 0; i < 4; i++) {
            float amp = params[AMP1_PARAM + i * 2].getValue();
            float pitch = params[PITCH1_PARAM + i * 2].getValue();
            float targetFrequency = dsp::FREQ_C4 * std::pow(2.0f, pitch / 12.0f);  // Convert to frequency in Hz

            // Calculate target delay time based on the pitch (1 / frequency gives period in seconds)
            float targetDelayTime = 1.0f / targetFrequency;
            targetDelaySamples[i] = targetDelayTime * sampleRate;

            // Interpolate the delay length smoothly
            currentDelaySamples[i] += (targetDelaySamples[i] - currentDelaySamples[i]) * interpolationSpeed;

            // Fetch the feedback signal from the delay buffer
            float delayOutput = readDelayBuffer(i, currentDelaySamples[i]) * feedback;

            float lowpassFreq = clamp(20000.f * colorFreq, 20.f, 20000.f);
            lowPassFilter[i].setCutoffFreq(lowpassFreq / args.sampleRate);
            lowPassFilter[i].process(delayOutput);
            delayOutput = lowPassFilter[i].lowpass();

            float highpassFreq = clamp(20.f * colorFreq, 20.f, 20000.f);
            highPassFilter[i].setCutoff(highpassFreq / args.sampleRate);
            highPassFilter[i].process(delayOutput);
            delayOutput = highPassFilter[i].highpass();

            // Continuously feed the input signal into the delay line along with filtered feedback
            float output = input + delayOutput;

            // Write the result back into the delay buffer
            writeDelayBuffer(i, clip(output));
            sumOutput += delayOutput * amp;

            // Output the resonated signal
        }
        sumOutput = clamp(sumOutput, -10.f, 10.f);
        outputs[OUT_OUTPUT].setVoltage(crossfade(input, sumOutput, mix));  // Scale the output voltage
    }
};

// User interface (Widget)
struct RezoModuleWidget : ModuleWidget {
    RezoModuleWidget(RezoModule* module) {
        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Rezo.svg")));

        float xOffset = 30;
        float yOffset = 30;
        float xSpacing = 90;
        float ySpacing = 50;

        for (int i = 0; i < 4; i++) {
            addParam(createParamCentered<RoundBlackKnob>(Vec(xOffset + i * xSpacing, yOffset), module, RezoModule::PITCH1_PARAM + i * 2));
            addParam(createParamCentered<RoundBlackKnob>(Vec(xOffset + i * xSpacing, yOffset + ySpacing), module, RezoModule::AMP1_PARAM + i * 2));
        }

        // Pitch knob
        // addParam(createParamCentered<RoundBlackKnob>(Vec(40, 40), module, RezoModule::PITCH_PARAM));

        // Decay knob
        addParam(createParamCentered<RoundBlackKnob>(Vec(214.5, 281.3), module, RezoModule::DECAY_PARAM));

        // Cutoff frequency knob
        addParam(createParamCentered<RoundBlackKnob>(Vec(267.5, 281.3), module, RezoModule::TONE_PARAM));

        // Input signal
        addInput(createInputCentered<PJ301MPort>(Vec(52.5, 329.25), module, RezoModule::IN_INPUT));
        addParam(createParamCentered<RoundBlackKnob>(Vec(323.5, 281.3), module, RezoModule::MIX_PARAM));

        // Output signal
        addOutput(createOutputCentered<PJ301MPort>(Vec(323.5, 329.25), module, RezoModule::OUT_OUTPUT));
    }
};

// Define the model
Model* modelRezo = createModel<RezoModule, RezoModuleWidget>("Rezo");
