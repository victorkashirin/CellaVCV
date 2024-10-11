#include <vector>

#include "components.hpp"
#include "plugin.hpp"

struct Resonators : Module {
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
        COLOR_PARAM,
        GAIN_PARAM,
        MIX_PARAM,
        DECAY_CV_PARAM,
        COLOR_CV_PARAM,
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
        COLOR_INPUT,
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

    Resonators() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(PITCH1_PARAM, -54.f, 54.f, 0.f, "Frequency I", " Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
        configParam(AMP1_PARAM, 0.0, 1.0, 0.5f, "Gain I", " dB", -10, 20);
        configParam(PITCH2_PARAM, -54.f, 54.f, 0.f, "Frequency II", " Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
        configParam(AMP2_PARAM, 0.0, 1.0, 0.5f, "Gain II", " dB", -10, 20);
        configParam(PITCH3_PARAM, -54.f, 54.f, 0.f, "Frequency III", " Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
        configParam(AMP3_PARAM, 0.0, 1.0, 0.5f, "Gain III", " dB", -10, 20);
        configParam(PITCH4_PARAM, -54.f, 54.f, 0.f, "Frequency IV", " Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
        configParam(AMP4_PARAM, 0.0, 1.0, 0.5f, "Gain IV", " dB", -10, 20);
        configParam(DECAY_PARAM, 0.0f, 1.f, 0.9f, "Decay");
        configParam(COLOR_PARAM, 0.f, 1.f, 0.5f, "Color", "%", 0, 200, -100);
        configParam(GAIN_PARAM, 0.0, 1.0, 0.5f, "Gain", " dB", -10, 20);
        configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Mix");

        configParam(DECAY_CV_PARAM, -1.f, 1.f, 0.f, "Decay CV", "%", 0, 100);
        configParam(COLOR_CV_PARAM, -1.f, 1.f, 0.f, "Color CV", "%", 0, 100);
        configParam(GAIN_CV_PARAM, -1.f, 1.f, 0.f, "Gain CV", "%", 0, 100);
        configParam(MIX_CV_PARAM, -1.f, 1.f, 0.f, "Mix CV", "%", 0, 100);

        configInput(PITCH1_INPUT, "I 1V/octave pitch");
        configInput(PITCH2_INPUT, "II 1V/octave pitch");
        configInput(PITCH3_INPUT, "III 1V/octave pitch");
        configInput(PITCH4_INPUT, "IV 1V/octave pitch");

        configInput(IN_INPUT, "Audio");
        configInput(DECAY_INPUT, "Decay");
        configInput(COLOR_INPUT, "Color");
        configInput(GAIN_INPUT, "Gain");
        configInput(MIX_INPUT, "Mix");

        configOutput(WET_OUTPUT, "Polyphonic wet signal");
        configOutput(OUT_OUTPUT, "Audio");

        configBypass(IN_INPUT, OUT_OUTPUT);
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
        bufferSize = (int)(sampleRate * 0.1);  // buffer sufficient for 10Hz as lowest frequency

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
        gain = clamp(gain, 0.f, 1.f);

        float color = params[COLOR_PARAM].getValue() + inputs[COLOR_INPUT].getVoltage() / 10.f * params[COLOR_CV_PARAM].getValue();
        color = clamp(color, 0.f, 1.f);
        float colorFreq = std::pow(100.f, 2.f * color - 1.f);

        float feedback = params[DECAY_PARAM].getValue() + inputs[DECAY_INPUT].getVoltage() / 10.f * params[DECAY_CV_PARAM].getValue();
        feedback = clamp(feedback, 0.f, 1.f);
        feedback = powf(feedback, 0.2f);
        feedback = rescale(feedback, 0.f, 1.f, 0.7f, 0.995f);

        float sumOutput = 0.f;
        outputs[WET_OUTPUT].setChannels(4);  // Set the polyphony for the wet output

        // Get pitch control (convert semitones to frequency)
        for (int i = 0; i < 4; i++) {
            float amp = params[AMP1_PARAM + i * 2].getValue();
            float pitch = params[PITCH1_PARAM + i * 2].getValue() / 12.f + inputs[PITCH1_INPUT + i].getVoltage();
            pitch = clamp(pitch, -4.5f, 4.5f);
            float targetFrequency = dsp::FREQ_C4 * std::pow(2.0f, pitch);  // Convert to frequency in Hz

            // Calculate target delay time based on the pitch (1 / frequency gives period in seconds)
            float targetDelayTime = 1.0f / targetFrequency;
            targetDelaySamples[i] = targetDelayTime * sampleRate;

            // Interpolate the delay length smoothly
            currentDelaySamples[i] += (targetDelaySamples[i] - currentDelaySamples[i]) * interpolationSpeed;

            // Fetch the feedback signal from the delay buffer
            float delayOutput = readDelayBuffer(i, currentDelaySamples[i]);
            float filteredOutput = 0.5 * (delayOutput + prevDelayOutput[i]);

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

            float output = input + delayOutput;

            // Write the result back into the delay buffer
            writeDelayBuffer(i, output);
            sumOutput += delayOutput * amp;
            outputs[WET_OUTPUT].setVoltage(delayOutput * amp, i);  // Polyphonic wet signal on channel i
        }
        sumOutput = gain * sumOutput;
        outputs[OUT_OUTPUT].setVoltage(crossfade(input, sumOutput, mix));  // Scale the output voltage
    }
};

struct ResonatorsWidget : ModuleWidget {
    ResonatorsWidget(Resonators* module) {
        setModule(module);

        setPanel(createPanel(asset::plugin(pluginInstance, "res/Resonators.svg"), asset::plugin(pluginInstance, "res/Resonators-dark.svg")));

        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        float xOffset = 22.5;
        float yOffset = 53.5;
        float xSpacing = 45;
        float ySpacing = 50;

        for (int i = 0; i < 4; i++) {
            addParam(createParamCentered<RoundBlackKnob>(Vec(xOffset + i * xSpacing, yOffset), module, Resonators::PITCH1_PARAM + i * 2));
            addParam(createParamCentered<RoundBlackKnob>(Vec(xOffset + i * xSpacing, yOffset + ySpacing), module, Resonators::AMP1_PARAM + i * 2));
        }

        for (int i = 0; i < 4; i++) {
            addInput(createInputCentered<ThemedPJ301MPort>(Vec(xOffset + i * xSpacing, 280), module, Resonators::PITCH1_INPUT + i));
        }

        // Decay knob
        addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 153.5), module, Resonators::DECAY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 153.5), module, Resonators::COLOR_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(112.5, 153.5), module, Resonators::GAIN_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(157.5, 153.5), module, Resonators::MIX_PARAM));

        addParam(createParamCentered<Trimpot>(Vec(22.5, 203.81), module, Resonators::DECAY_CV_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(67.5, 203.81), module, Resonators::COLOR_CV_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(112.5, 203.81), module, Resonators::GAIN_CV_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(157.5, 203.81), module, Resonators::MIX_CV_PARAM));

        // Input signal
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(22.5, 329.25), module, Resonators::IN_INPUT));

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(22.5, 230.28), module, Resonators::DECAY_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(67.5, 230.28), module, Resonators::COLOR_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(112.5, 230.28), module, Resonators::GAIN_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(157.5, 230.28), module, Resonators::MIX_INPUT));

        // Output signal
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(112.5, 329.25), module, Resonators::WET_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(157.5, 329.25), module, Resonators::OUT_OUTPUT));
    }
};

// Define the model
Model* modelRezo = createModel<Resonators, ResonatorsWidget>("Resonators");
