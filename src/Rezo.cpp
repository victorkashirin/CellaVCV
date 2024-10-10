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
        TONE_PARAM,
        GAIN_PARAM,
        MIX_PARAM,
        DECAY_CV_PARAM,
        TONE_CV_PARAM,
        GAIN_CV_PARAM,
        MIX_CV_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        IN_INPUT,  // Incoming signal
        PITCH1_INPUT,
        PITCH2_INPUT,
        PITCH3_INPUT,
        PITCH4_INPUT,
        DECAY_INPUT,
        TONE_INPUT,
        GAIN_INPUT,
        MIX_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        OUT_OUTPUT,  // Resonated signal output
        WET_OUTPUT,
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
    std::vector<float> prevDelayOutput;
    std::vector<float> currentDelaySamples;
    std::vector<float> targetDelaySamples;

    int bufferSize = 4 * 4096;
    float interpolationSpeed = 0.01f;  // Speed of interpolation

    float sampleRate = 44100.f;

    RezoModule() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(PITCH1_PARAM, -54.f, 54.f, 0.f, "Frequency 1", " Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
        configParam(AMP1_PARAM, 0.0, 2.0, 0.5f, "Amplitude 1", " dB", -10, 20);
        configParam(PITCH2_PARAM, -54.f, 54.f, 0.f, "Frequency 2", " Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
        configParam(AMP2_PARAM, 0.0, 2.0, 0.5f, "Amplitude 2", " dB", -10, 20);
        configParam(PITCH3_PARAM, -54.f, 54.f, 0.f, "Frequency 3", " Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
        configParam(AMP3_PARAM, 0.0, 2.0, 0.5f, "Amplitude 3", " dB", -10, 20);
        configParam(PITCH4_PARAM, -54.f, 54.f, 0.f, "Frequency 4", " Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
        configParam(AMP4_PARAM, 0.0, 2.0, 0.5f, "Amplitude 4", " dB", -10, 20);
        configParam(DECAY_PARAM, 0.0f, 1.f, 0.9f, "Decay");
        configParam(TONE_PARAM, 0.f, 1.f, 0.5f, "Tone", "%", 0, 200, -100);
        configParam(GAIN_PARAM, 0.0, 2.0, 1.0f, "Gain", " dB", -10, 20);
        configParam(MIX_PARAM, 0.f, 1.f, 0.f, "Mix");

        configParam(DECAY_CV_PARAM, -1.f, 1.f, 0.f, "Decay CV", "%", 0, 100);
        configParam(TONE_CV_PARAM, -1.f, 1.f, 0.f, "Decay CV", "%", 0, 100);
        configParam(GAIN_CV_PARAM, -1.f, 1.f, 0.f, "Decay CV", "%", 0, 100);
        configParam(MIX_CV_PARAM, -1.f, 1.f, 0.f, "Decay CV", "%", 0, 100);

        configInput(PITCH1_INPUT, "1V/octave pitch");
        configInput(PITCH2_INPUT, "1V/octave pitch");
        configInput(PITCH2_INPUT, "1V/octave pitch");
        configInput(PITCH2_INPUT, "1V/octave pitch");
    }

    void onSampleRateChange() override {
        sampleRate = APP->engine->getSampleRate();
        delayBuffers.resize(4);
        lowPassFilter.resize(4);
        highPassFilter.resize(4);
        delayIndices.resize(4, 0);
        currentDelaySamples.resize(4, 0.f);
        prevDelayOutput.resize(4, 0.f);
        targetDelaySamples.resize(4, 0.f);
        bufferSize = (int)(sampleRate * 0.1);

        for (auto& buffer : delayBuffers) {
            buffer.resize((int)bufferSize);
            std::fill(buffer.begin(), buffer.end(), 0.f);
        }
    }

    // Function to read from the delay buffer
    float readDelayBuffer(int bufferIndex, float delayTimeSamples) {
        float readIndex = delayIndices[bufferIndex] - delayTimeSamples;
        if (readIndex < 0)
            readIndex += bufferSize;
        int index0 = (int)readIndex;
        int index1 = (index0 + 1) % bufferSize;
        float frac = readIndex - index0;
        float delayedSample = (1.f - frac) * delayBuffers[bufferIndex][index0] + frac * delayBuffers[bufferIndex][index1];
        return delayedSample;
    }

    float readDelayBufferOld(int bufferIndex, float delayTimeSamples) {
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

        float mix = params[MIX_PARAM].getValue() + inputs[MIX_INPUT].getVoltage() / 10.f * params[MIX_CV_PARAM].getValue();
        mix = clamp(mix, 0.f, 1.f);

        float gain = params[GAIN_PARAM].getValue() + inputs[GAIN_INPUT].getVoltage() / 10.f * params[GAIN_CV_PARAM].getValue();
        gain = clamp(gain, 0.f, 2.f);

        float color = params[TONE_PARAM].getValue() + inputs[TONE_INPUT].getVoltage() / 10.f * params[TONE_CV_PARAM].getValue();
        color = clamp(color, 0.f, 1.f);
        float colorFreq = std::pow(100.f, 2.f * color - 1.f);

        float feedback = params[DECAY_PARAM].getValue() + inputs[DECAY_INPUT].getVoltage() / 10.f * params[DECAY_CV_PARAM].getValue();
        feedback = clamp(feedback, 0.f, 1.f);
        feedback = powf(feedback, 0.2f);
        feedback = rescale(feedback, 0.f, 1.f, 0.7f, 0.995f);

        float filt = 0.0f;  // 0.0 = lowpass, 1.0 = highpass

        float sumOutput = 0.f;
        outputs[WET_OUTPUT].setChannels(4);  // Set the polyphony for the wet output

        // Get pitch control (convert semitones to frequency)
        for (int i = 0; i < 4; i++) {
            float amp = params[AMP1_PARAM + i * 2].getValue();
            float pitch = params[PITCH1_PARAM + i * 2].getValue() / 12.f + inputs[PITCH1_INPUT + i].getVoltage();
            float targetFrequency = dsp::FREQ_C4 * std::pow(2.0f, pitch);  // Convert to frequency in Hz

            // Calculate target delay time based on the pitch (1 / frequency gives period in seconds)
            float targetDelayTime = 1.0f / targetFrequency;
            targetDelaySamples[i] = targetDelayTime * sampleRate;

            // Interpolate the delay length smoothly
            currentDelaySamples[i] += (targetDelaySamples[i] - currentDelaySamples[i]) * interpolationSpeed;

            // Fetch the feedback signal from the delay buffer
            float delayOutput = readDelayBuffer(i, currentDelaySamples[i]);

            // clasic

            float filteredOutput = (filt >= 0.5f) ? (delayOutput + prevDelayOutput[i]) * 0.5f : delayOutput;

            // with hight
            // float filteredOutput = delayOutput;

            prevDelayOutput[i] = delayOutput;
            delayOutput = filteredOutput * feedback;

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
            writeDelayBuffer(i, output);
            sumOutput += delayOutput * amp;
            outputs[WET_OUTPUT].setVoltage(delayOutput, i);  // Polyphonic wet signal on channel i
        }
        sumOutput = clamp(gain * sumOutput, -10.f, 10.f);
        outputs[OUT_OUTPUT].setVoltage(crossfade(input, sumOutput, mix));  // Scale the output voltage
    }
};

// User interface (Widget)
struct RezoModuleWidget : ModuleWidget {
    RezoModuleWidget(RezoModule* module) {
        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Rezo.svg")));

        float xOffset = 22.5;
        float yOffset = 53.5;
        float xSpacing = 45;
        float ySpacing = 50;

        for (int i = 0; i < 4; i++) {
            addParam(createParamCentered<RoundBlackKnob>(Vec(xOffset + i * xSpacing, yOffset), module, RezoModule::PITCH1_PARAM + i * 2));
            addParam(createParamCentered<RoundBlackKnob>(Vec(xOffset + i * xSpacing, yOffset + ySpacing), module, RezoModule::AMP1_PARAM + i * 2));
        }

        for (int i = 0; i < 4; i++) {
            addInput(createInputCentered<ThemedPJ301MPort>(Vec(xOffset + i * xSpacing, 280), module, RezoModule::PITCH1_INPUT + i));
        }

        // Decay knob
        addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 153.5), module, RezoModule::DECAY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 153.5), module, RezoModule::TONE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(112.5, 153.5), module, RezoModule::GAIN_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(157.5, 153.5), module, RezoModule::MIX_PARAM));

        addParam(createParamCentered<Trimpot>(Vec(22.5, 203.81), module, RezoModule::DECAY_CV_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(67.5, 203.81), module, RezoModule::TONE_CV_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(112.5, 203.81), module, RezoModule::GAIN_CV_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(157.5, 203.81), module, RezoModule::MIX_CV_PARAM));

        // Input signal
        addInput(createInputCentered<PJ301MPort>(Vec(22.5, 329.25), module, RezoModule::IN_INPUT));

        addInput(createInputCentered<PJ301MPort>(Vec(22.5, 230.28), module, RezoModule::DECAY_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(67.5, 230.28), module, RezoModule::TONE_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(112.5, 230.28), module, RezoModule::GAIN_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(157.5, 230.28), module, RezoModule::MIX_INPUT));

        // Output signal
        addOutput(createOutputCentered<PJ301MPort>(Vec(112.5, 329.25), module, RezoModule::WET_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(Vec(157.5, 329.25), module, RezoModule::OUT_OUTPUT));
    }
};

// Define the model
Model* modelRezo = createModel<RezoModule, RezoModuleWidget>("Rezo");
