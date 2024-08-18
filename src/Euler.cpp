#include "components.hpp"
#include "plugin.hpp"

struct Euler : Module {
    enum ParamId {
        FREQ_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        SIGNAL_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        SLOPE_OUTPUT,
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
    dsp::ClockDivider lightDivider;

    Euler() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(FREQ_PARAM, -8.f, 4.f, 1.f, "Frequency", " Hz", 2, 1);
        configInput(SIGNAL_INPUT, "Sampling signal");
        configOutput(SLOPE_OUTPUT, "Normalised angle of the slope");
        lightDivider.setDivision(16);
    }

    void process(const ProcessArgs &args) override {
        double voltage = inputs[SIGNAL_INPUT].getVoltage();
        double pitch = params[FREQ_PARAM].getValue();
        double freq = std::pow(2.f, pitch);

        // sampling window affects precision on low frequencies
        int window = (int)clamp(2.f / freq, 1, 1024);

        if (step % window == 0) {
            double diff = voltage - previousVoltage;
            double angle = std::atan2(diff, window * args.sampleTime * 31.5 * freq) * 180 / M_PI;
            currentValue = 10.f * angle / 90.f;
            outputs[SLOPE_OUTPUT].setVoltage(currentValue);
            previousVoltage = voltage;
        } else {
            outputs[SLOPE_OUTPUT].setVoltage(currentValue);
        }

        step += 1;
        step = step % window;

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
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Euler.svg")));

        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        addChild(createLightCentered<LargeFresnelLight<GreenRedLight>>(Vec(30, 35), module, Euler::SIG_LIGHT_POS));

        addParam(createParamCentered<RoundBlackKnob>(Vec(30, 104.36), module, Euler::FREQ_PARAM));

        addInput(createInputCentered<PJ301MPort>(Vec(30, 231.9), module, Euler::SIGNAL_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(Vec(30, 330.01), module, Euler::SLOPE_OUTPUT));
    }
};

Model *modelEuler = createModel<Euler, EulerWidget>("Euler");