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
        RATE_PARAM,  // fine time-constant trim −5 … +5 (≈ /32 … ×32)
        LEAK_PARAM,
        RANGE_PARAM,  // 3-pos switch: 0=CV,1=LO,2=HI
        CLIP_PARAM,   // 3-pos switch: 0=off,1=hard,2=soft
        RESET_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        IN_INPUT,
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
        NUM_LIGHTS
    };

    /* ───────────── STATE ─────────────────────────────── */
    float y = 0.f;  // integrator state

    dsp::SchmittTrigger resetButtonTrigger;
    dsp::SchmittTrigger resetInputTrigger;

    Integrator() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

        configParam(RATE_PARAM, -4.f, 4.f, 0.f, "Fine rate", "x", 2.f);
        configParam<DecayTimeQuantity>(LEAK_PARAM, 0.f, 1.f, 0.f, "Leak time");

        configSwitch(RANGE_PARAM, 0.f, 2.f, 1.f, "Range", {"CV (slow)", "LO", "HI (audio)"});

        configSwitch(CLIP_PARAM, 0.f, 2.f, 1.f, "Clip", {"Off", "Hard", "Soft"});
        configButton(RESET_PARAM, "Reset");

        configInput(GAIN_CV_INPUT, "Gain CV");
        configInput(LEAK_CV_INPUT, "Leak CV");
        configInput(IN_INPUT, "In");
        configInput(RESET_INPUT, "Reset Trigger");
        configOutput(OUT_OUTPUT, "Out");
    }

    /* ───────────── DSP ───────────────────────────────── */
    void process(const ProcessArgs& args) override {
        const float v = inputs[IN_INPUT].getVoltageSum();
        const float dt = args.sampleTime;

        /* --- τ from 3-way range + fine knob -------------- */
        static const float baseTau[3] = {4.f, 0.36f, 0.18f};  // CV, LO, HI
        int rangeSel = (int)std::round(params[RANGE_PARAM].getValue());
        rangeSel = clamp(rangeSel, 0, 2);

        const float gain = params[RATE_PARAM].getValue() +
                           (inputs[GAIN_CV_INPUT].isConnected()
                                ? inputs[GAIN_CV_INPUT].getVoltage() / 2.f
                                : 0.f);
        const float tau = baseTau[rangeSel] *
                          std::pow(2.f, -gain);

        /* --- leak multiplier ----------------------------- */
        float leakPos = params[LEAK_PARAM].getValue();
        if (inputs[LEAK_CV_INPUT].isConnected())
            leakPos = clamp(leakPos +
                                inputs[LEAK_CV_INPUT].getVoltage() / 10.f,
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
        y = y * leakMul + v * (dt / tau);

        if (resetButtonTrigger.process(params[RESET_PARAM].getValue()) || resetInputTrigger.process(inputs[RESET_INPUT].getVoltage())) {
            y = 0.f;
        }

        /* ---- clipping ---- */
        const int clipSel = (int)std::round(params[CLIP_PARAM].getValue());
        if (clipSel == 1)  // hard ±10 V
            y = clamp(y, -10.f, 10.f);
        else if (clipSel == 2)  // soft tanh
            y = 10.f * std::tanh(y / 10.f);

        outputs[OUT_OUTPUT].setVoltage(y);
    }
};

/* ───────────── WIDGET ─────────────────────────────── */
struct IntegratorWidget : rack::ModuleWidget {
    IntegratorWidget(Integrator* m) {
        setModule(m);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Integrator.svg")));

        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        /* layout coordinates assume 128 px wide panel */
        addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 53.39), m, Integrator::RATE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 53.39), m, Integrator::LEAK_PARAM));

        addParam(createParamCentered<CKSSThree>(Vec(16.5, 162.66), m, Integrator::RANGE_PARAM));
        addParam(createParamCentered<CKSSThree>(Vec(54.74, 162.66), m, Integrator::CLIP_PARAM));

        addInput(createInputCentered<PJ301MPort>(Vec(22.5, 231.31), m, Integrator::GAIN_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(67.5, 231.31), m, Integrator::LEAK_CV_INPUT));

        addInput(createInputCentered<PJ301MPort>(Vec(22.5, 280.1), m, Integrator::RESET_INPUT));
        addParam(createParamCentered<VCVButton>(Vec(67.5, 280.1), m, Integrator::RESET_PARAM));

        addInput(createInputCentered<PJ301MPort>(Vec(22.5, 329.25), m, Integrator::IN_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(Vec(67.5, 329.25), m, Integrator::OUT_OUTPUT));
    }
};

/* ───────────── PLUGIN ENTRY ───────────────────────── */
Model* modelIntegrator = rack::createModel<Integrator, IntegratorWidget>("Integrator");
