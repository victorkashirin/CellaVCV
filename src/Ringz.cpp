#include "plugin.hpp"

struct RingzModule : Module {
    enum ParamIds {
        FREQ_PARAM,
        DECAY_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        IN_INPUT,
        FREQ_INPUT,
        DECAY_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        OUT_OUTPUT,
        NUM_OUTPUTS
    };

    float y1 = 0.f;
    float y2 = 0.f;
    float b1 = 0.f;
    float b2 = 0.f;
    float freq = -1.f;
    float decayTime = 0.f;
    dsp::SchmittTrigger trigger;

    RingzModule() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS);
        configParam(FREQ_PARAM, 20.f, 8000.f, 440.f, "Frequency");
        configParam(DECAY_PARAM, 0.1f, 5.f, 1.f, "Decay Time");
    }

    void process(const ProcessArgs& args) override {
        // Input and parameter handling
        float input = inputs[IN_INPUT].getVoltage();
        float paramFreq = params[FREQ_PARAM].getValue();
        float paramDecay = params[DECAY_PARAM].getValue();
        float inputFreq = inputs[FREQ_INPUT].isConnected() ? inputs[FREQ_INPUT].getVoltage() : paramFreq;
        float inputDecay = inputs[DECAY_INPUT].isConnected() ? inputs[DECAY_INPUT].getVoltage() : paramDecay;

        // Frequency and decay time are updated if changed
        if (inputFreq != freq || inputDecay != decayTime) {
            updateCoefficients(inputFreq, inputDecay, args.sampleRate);
            freq = inputFreq;
            decayTime = inputDecay;
        }

        float amplitudeCompensation = calculateAmplitudeCompensation(freq, args.sampleRate);

        // Process the input signal using the difference equation
        float y0 = input + b1 * y1 + b2 * y2;
        float output = clamp(0.5f * amplitudeCompensation * (y0 - y2), -10.f, 10.f);
        outputs[OUT_OUTPUT].setVoltage(output);

        // Update history variables
        y2 = y1;
        y1 = y0;

        // Remove denormalized values (zapgremlins equivalent)
        y1 = zapgremlins(y1);
        y2 = zapgremlins(y2);
    }

    void updateCoefficients(float freq, float decay, float sampleRate) {
        double ffreq = 2.0 * M_PI * freq / sampleRate;
        double R = decay == 0.f ? 0.f : exp(log(0.001f) / (decay * sampleRate));
        double twoR = 2.0 * R;
        double R2 = R * R;
        double cost = (twoR * cos(ffreq)) / (1.0 + R2);
        b1 = twoR * cost;
        b2 = -R2;
    }

    float zapgremlins(float x) {
        float absx = std::abs(x);
        return (absx > 1e-15 && absx < 1e15) ? x : 0.f;
    }

    // Function to calculate amplitude compensation based on frequency
    float calculateAmplitudeCompensation(float freq, float sampleRate) {
        // Normalize frequency to a range between 0 and 1 (0 being low, 1 being the Nyquist frequency)
        float normalizedFreq = freq / (sampleRate / 2.f);  // freq normalized to the Nyquist frequency

        // Apply a logarithmic scaling for compensation
        // Compensation factor increases more smoothly as frequency increases
        float compensation = 0.06f / std::sqrt(normalizedFreq + 0.01f);  // Log-like scale with small offset to avoid division by zero

        return compensation;
    }
};

struct RingzWidget : ModuleWidget {
    RingzWidget(RingzModule* module) {
        setModule(module);

        // Set panel for the module
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Klank.svg")));

        // Add screws at the top and bottom
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Frequency knob
        addParam(createParam<RoundLargeBlackKnob>(Vec(28, 87), module, RingzModule::FREQ_PARAM));

        // Decay time knob
        addParam(createParam<RoundLargeBlackKnob>(Vec(28, 157), module, RingzModule::DECAY_PARAM));

        // Input port (for audio input)
        addInput(createInput<PJ301MPort>(Vec(33, 220), module, RingzModule::IN_INPUT));

        // Frequency modulation (CV) input port
        addInput(createInput<PJ301MPort>(Vec(33, 270), module, RingzModule::FREQ_INPUT));

        // Decay time modulation (CV) input port
        addInput(createInput<PJ301MPort>(Vec(33, 320), module, RingzModule::DECAY_INPUT));

        // Output port
        addOutput(createOutput<PJ301MPort>(Vec(66, 320), module, RingzModule::OUT_OUTPUT));
    }
};

// Define the model
Model* modelRingz = createModel<RingzModule, RingzWidget>("Ringz");
