#include "components.hpp"
#include "plugin.hpp"

struct Euler : Module {
    enum ParamId {
        FREQ_PARAM,
        SMOOTH_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        SIGNAL_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        SLOPE_OUTPUT,
        SLOPE_ABS_OUTPUT,
        SLOPE_POS_OUTPUT,
        SLOPE_NEG_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        SIG_LIGHT_POS,
        SIG_LIGHT_NEG,
        LIGHTS_LEN
    };

    double previousVoltage = 0.f;
    double currentValue = 0.f;
    int step = 0;
    int smoothStep = 0;
    dsp::ClockDivider lightDivider;
    float *steps;
    float average = 0.f;
    int _sampleRate;

    float calculateMovingAverage(const float *steps, float average, int size, int step, int windowLength) {
        return average + (steps[step % size] - steps[((step - windowLength + 1 + size) % size)]) / windowLength;
    }

    float pos(float signal) {
        if (signal > 0.f) return signal;
        return 0.f;
    }

    float neg(float signal) {
        if (signal < 0.f) return -signal;
        return 0.f;
    }

    Euler() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(FREQ_PARAM, -8.f, 4.f, 1.f, "Frequency", " Hz", 2, 1);
        configParam(SMOOTH_PARAM, 0.f, 1.f, 0.f, "Smooth", " Seconds");
        configInput(SIGNAL_INPUT, "Sampling signal");
        configOutput(SLOPE_OUTPUT, "Normalised angle of the slope");
        configOutput(SLOPE_ABS_OUTPUT, "Absolute angle of the slope");
        configOutput(SLOPE_POS_OUTPUT, "Positive part of the angle");
        configOutput(SLOPE_NEG_OUTPUT, "Negative part of the angle");
        lightDivider.setDivision(16);
        _sampleRate = (int)APP->engine->getSampleRate();
        steps = new float[_sampleRate];
    }

    void onSampleRateChange() override {
        delete[] steps;
        _sampleRate = (int)APP->engine->getSampleRate();
        steps = new float[_sampleRate];
        average = 0.f;
        smoothStep = 0;
        step = 0;
    }

    void process(const ProcessArgs &args) override {
        double voltage = inputs[SIGNAL_INPUT].getVoltage();
        double pitch = params[FREQ_PARAM].getValue();
        double smooth = params[SMOOTH_PARAM].getValue();
        double freq = std::pow(2.f, pitch);

        steps[smoothStep] = voltage;
        if (smooth * _sampleRate >= 1.f) {
            average = calculateMovingAverage(steps, average, _sampleRate, smoothStep, (int)(smooth * _sampleRate));
            voltage = average;
        }

        // sampling window affects precision on low frequencies
        int window = (int)clamp(2.f / freq, 1, 1024);

        if (step % window == 0) {
            double diff = voltage - previousVoltage;
            double angle = std::atan2(diff, window * args.sampleTime * 31.5 * freq) * 180 / M_PI;
            currentValue = 10.f * angle / 90.f;
            previousVoltage = voltage;
        }

        outputs[SLOPE_OUTPUT].setVoltage(currentValue);
        outputs[SLOPE_ABS_OUTPUT].setVoltage(std::abs(currentValue));
        outputs[SLOPE_POS_OUTPUT].setVoltage(pos(currentValue));
        outputs[SLOPE_NEG_OUTPUT].setVoltage(neg(currentValue));

        step += 1;
        step = step % window;

        smoothStep += 1;
        smoothStep = smoothStep % _sampleRate;

        // LIGHT
        if (lightDivider.process()) {
            float lightTime = args.sampleTime * lightDivider.getDivision();
            lights[SIG_LIGHT_POS].setBrightnessSmooth(fmaxf(0.0f, currentValue / 10.f), lightTime);
            lights[SIG_LIGHT_NEG].setBrightnessSmooth(fmaxf(0.0f, -currentValue / 10.f), lightTime);
        }
    }
};

struct EulerWidget : ModuleWidget {
    EulerWidget(Euler *module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Euler.svg"), asset::plugin(pluginInstance, "res/Euler-dark.svg")));

        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        addChild(createLightCentered<LargeFresnelLight<GreenRedLight>>(Vec(37.5, 35), module, Euler::SIG_LIGHT_POS));

        addParam(createParamCentered<RoundBlackKnob>(Vec(37.5, 104.36), module, Euler::FREQ_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(37.5, 154.33), module, Euler::SMOOTH_PARAM));

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(37.5, 231.9), module, Euler::SIGNAL_INPUT));

        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(21, 280.01), module, Euler::SLOPE_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(54, 280.01), module, Euler::SLOPE_ABS_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(21, 330.01), module, Euler::SLOPE_POS_OUTPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(54, 330.01), module, Euler::SLOPE_NEG_OUTPUT));
    }
};

Model *modelEuler = createModel<Euler, EulerWidget>("Euler");