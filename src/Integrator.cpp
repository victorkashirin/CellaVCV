#include "components.hpp"
#include "plugin.hpp"

struct DecayTimeQuantity : rack::ParamQuantity {
    const float minTime = 0.001f;  // 1 ms
    const float maxTime = 1000.f;  // 1000 s
    const float infinityPlaceholder = std::numeric_limits<float>::infinity();
    const float epsilon = 0.001f;  // Threshold below which value is considered "off"

    // Map the knob's internal 0..1 value to the time constant
    // Static allows calling without an instance, useful in process()
    static float getLeakTimeConstant(float value, float minT, float maxT, float eps) {
        if (value < eps) {
            return std::numeric_limits<float>::infinity();
        }
        // Logarithmic mapping: value=eps -> maxTime, value=1 -> minTime
        // Normalize value to range slightly > 0 to 1 for calculation clarity if eps is used
        // Or adjust the mapping range directly:
        float logMin = std::log10(minT);
        float logMax = std::log10(maxT);
        // Map value in [eps, 1] to exponent range [logMax, logMin]
        // When value = eps, factor = 0 -> logMax
        // When value = 1, factor = 1 -> logMin
        float factor = (value - eps) / (1.f - eps);
        float timeLog = logMax + factor * (logMin - logMax);
        return std::pow(10.f, timeLog);
    }

    // Override how the value is displayed on the panel
    std::string getDisplayValueString() override {
        float knobValue = getValue();  // Gets the internal 0..1 value
        float time = getLeakTimeConstant(knobValue, minTime, maxTime, epsilon);

        if (std::isinf(time)) {
            return "Off";  // Or "Inf" or "∞"
        } else if (time >= 1.f) {
            return rack::string::f("%.1f s", time);  // Show seconds
        } else if (time >= 0.001f) {
            return rack::string::f("%.1f ms", time * 1000.f);  // Show milliseconds
        } else {
            return rack::string::f("%.1f µs", time * 1000000.f);  // Show microseconds for very small values
        }
    }

    // Provide access to the static calculation method if needed elsewhere
    float getTimeConstantForValue(float value) const {
        return getLeakTimeConstant(value, minTime, maxTime, epsilon);
    }
};

struct Integrator : rack::Module {
    /* ───────────── ENUMS ─────────────────────────────── */
    enum ParamIds {
        RATE_PARAM,
        LEAK_PARAM,
        RANGE_PARAM,
        CLIP_PARAM,
        RESET_PARAM,
        INIT_BUTTON_PARAM,
        INIT_PARAM,
        GAIN_CV_PARAM,
        LEAK_CV_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        IN_INPUT,
        GATE_INPUT,
        GAIN_CV_INPUT,
        LEAK_CV_INPUT,
        RESET_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        OUT_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds {
        OUT_POS_LIGHT,
        OUT_NEG_LIGHT,
        NUM_LIGHTS
    };

    /* ───────────── STATE ─────────────────────────────── */
    float y = 0.f;  // integrator state

    dsp::SchmittTrigger resetButtonTrigger;
    dsp::SchmittTrigger resetInputTrigger;
    dsp::ClockDivider lightDivider;

    Integrator() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

        configParam(RATE_PARAM, -4.f, 4.f, 0.f, "Integration rate", "x");
        configParam<DecayTimeQuantity>(LEAK_PARAM, 0.f, 1.f, 0.f, "Leak time");

        configParam(INIT_PARAM, 0.f, 10.f, 5.f, "Init value", "V");
        configButton(INIT_BUTTON_PARAM, "Init");

        configSwitch(RANGE_PARAM, 0.f, 2.f, 1.f, "Range", {"CV (slow)", "LO", "HI (audio)"});

        configParam(GAIN_CV_PARAM, -1.f, 1.f, 0.f, "Rate CV", "%", 0.f, 100.f);
        configParam(LEAK_CV_PARAM, -1.f, 1.f, 0.f, "Leak CV", "%", 0.f, 100.f);

        configSwitch(CLIP_PARAM, 0.f, 1.f, 1.f, "Clip", {"Fold", "Clip"});
        configButton(RESET_PARAM, "Reset");

        configInput(GAIN_CV_INPUT, "Rate CV");
        configInput(LEAK_CV_INPUT, "Leak CV");
        configInput(IN_INPUT, "Signal");
        configInput(GATE_INPUT, "Integrator Gate");
        configInput(RESET_INPUT, "Reset Trigger");
        configOutput(OUT_OUTPUT, "Result");

        lightDivider.setDivision(16);
    }

    float fold(float x, float a, float b) {
        float result = x;
        if (x > b) {
            result = 2 * b - x;
        } else if (x < a) {
            result = 2 * a - x;
        }
        if (result < a || result > b) return fold(result, a, b);
        return result;
    }

    /* ───────────── DSP ───────────────────────────────── */
    void process(const ProcessArgs& args) override {
        float v = inputs[IN_INPUT].getVoltageSum();
        const float dt = args.sampleTime;

        if (params[INIT_BUTTON_PARAM].getValue() > 0.f) {
            v += params[INIT_PARAM].getValue();
        }

        /* --- τ from 3-way range + fine knob -------------- */
        static const float baseTau[3] = {2.f, 0.25f, 0.03125f};  // CV, LO, HI
        int rangeSel = (int)std::round(params[RANGE_PARAM].getValue());
        rangeSel = clamp(rangeSel, 0, 2);

        float rate = params[RATE_PARAM].getValue();
        rate += (inputs[GAIN_CV_INPUT].isConnected()
                     ? params[GAIN_CV_PARAM].getValue() * inputs[GAIN_CV_INPUT].getVoltage() / 5.f
                     : 0.f);
        const float effectiveTau = baseTau[rangeSel] * std::pow(2.f, -rate);

        /* --- leak multiplier ----------------------------- */
        float leakPos = params[LEAK_PARAM].getValue();
        if (inputs[LEAK_CV_INPUT].isConnected())
            leakPos = clamp(leakPos +
                                params[LEAK_CV_PARAM].getValue() * inputs[LEAK_CV_INPUT].getVoltage() / 5.f,
                            0.f, 1.f);

        const float tauLeak = DecayTimeQuantity::getLeakTimeConstant(
            leakPos,
            0.001f,  // minTime
            1000.f,  // maxTime
            0.001f   // epsilon
        );

        float leakMul;
        if (std::isinf(tauLeak)) {  // treat near-zero as ideal
            leakMul = 1.f;
        } else {
            const float safeTauLeak = std::max(tauLeak, 1e-9f);
            leakMul = std::exp(-dt / safeTauLeak);  // exact decay multiplier
        }

        /* --- integrator step ----------------------------- */
        y = y * leakMul + v * (dt / effectiveTau);

        if (resetButtonTrigger.process(params[RESET_PARAM].getValue()) || resetInputTrigger.process(inputs[RESET_INPUT].getVoltage())) {
            y = 0.f;
        }

        /* ---- clipping ---- */
        const int clipSel = (int)std::round(params[CLIP_PARAM].getValue());
        if (clipSel == 1) {
            y = clamp(y, -10.f, 10.f);
        }

        float outputValue = y;
        if (clipSel == 0) {
            outputValue = fold(y, -10.f, 10.f);
        }
        outputs[OUT_OUTPUT].setVoltage(outputValue);

        if (lightDivider.process()) {
            float lightTime = args.sampleTime * lightDivider.getDivision();
            lights[OUT_POS_LIGHT].setBrightnessSmooth(fmaxf(0.0f, outputValue / 10.f), lightTime);
            lights[OUT_NEG_LIGHT].setBrightnessSmooth(fmaxf(0.0f, -outputValue / 10.f), lightTime);
        }
    }
};

/* ───────────── WIDGET ─────────────────────────────── */
struct IntegratorWidget : rack::ModuleWidget {
    IntegratorWidget(Integrator* m) {
        setModule(m);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Integrator.svg"), asset::plugin(pluginInstance, "res/Integrator-dark.svg")));

        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        addChild(createLightCentered<LargeFresnelLight<GreenRedLight>>(Vec(45.0, 35.0), module, Integrator::OUT_POS_LIGHT));

        addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 53.39), m, Integrator::RATE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 53.39), m, Integrator::LEAK_PARAM));

        addParam(createParamCentered<VCVButtonHuge>(Vec(22.5, 104.35), module, Integrator::INIT_BUTTON_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 104.35), module, Integrator::INIT_PARAM));

        addParam(createParamCentered<CKSSThree>(Vec(16.54, 162.66), m, Integrator::RANGE_PARAM));
        addParam(createParamCentered<CKSS>(Vec(54.74, 162.66), m, Integrator::CLIP_PARAM));

        addParam(createParamCentered<Trimpot>(Vec(15.f, 203.79), module, Integrator::GAIN_CV_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(75.f, 203.79), module, Integrator::LEAK_CV_PARAM));

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(15.f, 231.31), m, Integrator::GAIN_CV_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(45.f, 231.31), m, Integrator::GATE_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(75.f, 231.31), m, Integrator::LEAK_CV_INPUT));

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(22.5, 280.1), m, Integrator::RESET_INPUT));
        addParam(createParamCentered<VCVButton>(Vec(67.5, 280.1), m, Integrator::RESET_PARAM));

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(22.5, 329.25), m, Integrator::IN_INPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(67.5, 329.25), m, Integrator::OUT_OUTPUT));
    }
};

Model* modelIntegrator = rack::createModel<Integrator, IntegratorWidget>("Integrator");
