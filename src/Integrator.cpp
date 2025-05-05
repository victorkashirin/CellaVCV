#include "components.hpp"
#include "plugin.hpp"

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
        RATE_CV_PARAM,
        LEAK_CV_PARAM,
        GATE_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        IN_INPUT,
        GATE_INPUT,
        RATE_CV_INPUT,
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
        GATE_LIGHT,
        NUM_LIGHTS
    };

    /* ───────────── STATE ─────────────────────────────── */
    float y = 0.f;          // integrator state
    bool gateMode = false;  // Used to determine if the knob is in gate mode

    dsp::SchmittTrigger resetButtonTrigger;
    dsp::SchmittTrigger resetInputTrigger;
    dsp::ClockDivider lightDivider;
    dsp::BooleanTrigger gateBoolean;

    struct DecayTimeQuantity : rack::ParamQuantity {
        static constexpr float minTime = 0.001f;  // 1 ms
        static constexpr float maxTime = 100.f;   // 100 s
        static constexpr float epsilon = 0.001f;  // Threshold below which value is considered "off"

        DecayTimeQuantity() {
            description = "Time required for the voltage to\ndecay towards zero by approximately 63%";
        }

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
        static float getTimeConstantForValue(float value) {
            return getLeakTimeConstant(value, minTime, maxTime, epsilon);
        }
    };

    struct IntegrationRateQuantity : rack::ParamQuantity {
        // Copy the baseTau values here for easy access
        // These are the base time constants in seconds for each range
        static constexpr float baseTau[3] = {2.f, 0.25f, 0.03125f};      // LO, MID, HI
        const float minRate = 1.f / (baseTau[0] * std::pow(2.f, -4.f));  // Max Tau -> Min Rate
        const float maxRate = 1.f / (baseTau[2] * std::pow(2.f, 4.f));   // Min Tau -> Max Rate

        // Override getDisplayValueString to show the calculated rate
        std::string getDisplayValueString() override {
            // The `module` pointer is automatically set by Rack engine
            // We need it to access the RANGE_PARAM value
            if (!module) {
                // Should not happen in normal operation, but good practice
                return rack::ParamQuantity::getDisplayValueString();  // Fallback
            }

            // Get the raw value of THIS parameter (RATE_PARAM: -4.f to 4.f)
            float rateKnobValue = getValue();

            // Get the current value of the RANGE_PARAM switch from the module
            // We need to cast module to the specific type (Integrator*) to access its enums
            // This creates a dependency but is necessary here.
            Integrator* integratorModule = dynamic_cast<Integrator*>(module);
            if (!integratorModule) {
                return rack::ParamQuantity::getDisplayValueString();  // Fallback if cast fails
            }
            float rangeSwitchValue = module->params[Integrator::RANGE_PARAM].getValue();
            int rangeSel = rack::clamp((int)std::round(rangeSwitchValue), 0, 2);

            // Calculate effectiveTau based *only* on knob and range switch
            // (Ignoring CV modulation as requested)
            float effectiveTau = baseTau[rangeSel] * std::pow(2.f, -rateKnobValue);  // Units: seconds

            // --- Calculate the display rate (1 / effectiveTau) ---
            float displayRate;  // Units: 1/s = Hz

            // Handle edge cases: very small or infinite tau
            if (!std::isfinite(effectiveTau) || effectiveTau <= 0.f) {
                // effectiveTau is inf, nan, zero or negative -> rate is problematic
                if (rateKnobValue <= getMinValue()) {  // Knob at max negative -> large tau
                    return "<0.01 mHz";                // Or "~0 Hz"
                } else {                               // Should ideally not happen with pow(2,...) unless baseTau is weird
                    return "? Hz";
                }
            } else if (effectiveTau < 1e-9f) {  // Effective tau is extremely small (~nanoseconds)
                                                // Rate is extremely high
                displayRate = 1.0e9f;           // Cap display at 1 GHz for practicality
            } else if (effectiveTau > 1e8f) {   // Effective tau is extremely large (~years)
                // Rate is extremely low
                displayRate = 0.f;  // Treat as effectively zero for display
            } else {
                displayRate = 1.f / effectiveTau;
            }

            // --- Format the rate for display ---
            if (displayRate >= 1e6f) {
                // Use fixed precision for large numbers if desired
                if (displayRate >= 100e6) return rack::string::f("%.0f MHz", displayRate / 1e6f);
                if (displayRate >= 10e6) return rack::string::f("%.1f MHz", displayRate / 1e6f);
                return rack::string::f("%.2f MHz", displayRate / 1e6f);
            } else if (displayRate >= 1e3f) {
                if (displayRate >= 100e3) return rack::string::f("%.0f kHz", displayRate / 1e3f);
                if (displayRate >= 10e3) return rack::string::f("%.1f kHz", displayRate / 1e3f);
                return rack::string::f("%.2f kHz", displayRate / 1e3f);
            } else if (displayRate >= 1.f) {
                if (displayRate >= 100.f) return rack::string::f("%.0f Hz", displayRate);
                if (displayRate >= 10.f) return rack::string::f("%.1f Hz", displayRate);
                return rack::string::f("%.2f Hz", displayRate);
            } else if (displayRate >= 1e-3f) {
                if (displayRate >= 0.1f) return rack::string::f("%.1f mHz", displayRate * 1e3f);   // e.g. 150.2 mHz
                if (displayRate >= 0.01f) return rack::string::f("%.1f mHz", displayRate * 1e3f);  // e.g. 15.3 mHz
                return rack::string::f("%.2f mHz", displayRate * 1e3f);                            // e.g. 1.54 mHz
            } else if (displayRate > 1e-5f) {                                                      // Between 0.01 mHz and 1 mHz
                return rack::string::f("%.2f mHz", displayRate * 1e3f);
            } else {                 // Very close or equal to zero
                return "<0.01 mHz";  // Or "0 Hz" if preferred
            }
        }

        // Optional: You might want to override getLabel() if you don't want the unit
        // specified in configParam to appear redundantly, but getDisplayValueString()
        // includes the unit, so it's usually fine.
        // std::string getLabel() override { return ""; } // Removes unit label next to knob
    };

    Integrator() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

        configParam<IntegrationRateQuantity>(RATE_PARAM, -4.f, 4.f, 0.f, "Integration rate", "");
        configParam<DecayTimeQuantity>(LEAK_PARAM, 0.f, 1.f, 0.f, "Decay time");

        configParam(INIT_PARAM, 0.f, 10.f, 5.f, "Init value", "V");
        configButton(INIT_BUTTON_PARAM, "Init");
        getParamQuantity(INIT_BUTTON_PARAM)->description =
            "Adds value of the Init knob to the input\n"
            "when the button is pressed";

        configSwitch(RANGE_PARAM, 0.f, 2.f, 1.f, "Rate range", {"Low", "Mid", "High"});

        configParam(RATE_CV_PARAM, -1.f, 1.f, 0.f, "Rate CV", "%", 0.f, 100.f);
        configParam(LEAK_CV_PARAM, -1.f, 1.f, 0.f, "Leak CV", "%", 0.f, 100.f);
        configButton(GATE_PARAM, "Integrate on gate low/high");

        configSwitch(CLIP_PARAM, 0.f, 1.f, 1.f, "Clip", {"Fold", "Clip"});
        configButton(RESET_PARAM, "Reset");

        configInput(RATE_CV_INPUT, "Rate CV");
        configInput(LEAK_CV_INPUT, "Leak CV");
        configInput(IN_INPUT, "Signal");
        configInput(GATE_INPUT, "Integrator gate");
        configInput(RESET_INPUT, "Reset trigger");
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
        // Recursive call might be risky, iterative is safer
        while (result < a || result > b) {
            if (result > b)
                result = 2 * b - result;
            else if (result < a)
                result = 2 * a - result;
            // Add a safety break for potential infinite loops if logic is flawed
            if (std::abs(result) > 1e6) break;  // Arbitrary large value
        }
        return result;
    }

    /* ───────────── DSP ───────────────────────────────── */
    void process(const ProcessArgs& args) override {
        float v = inputs[IN_INPUT].getVoltageSum();
        const float dt = args.sampleTime;

        if (gateBoolean.process(params[GATE_PARAM].getValue()))
            gateMode ^= true;

        if (params[INIT_BUTTON_PARAM].getValue() > 0.f) {
            v += params[INIT_PARAM].getValue();
        }

        if (resetButtonTrigger.process(params[RESET_PARAM].getValue()) ||
            resetInputTrigger.process(inputs[RESET_INPUT].getVoltage())) {
            y = 0.f;
        }

        /* --- τ from 3-way range + fine knob -------------- */
        int rangeSel = (int)std::round(params[RANGE_PARAM].getValue());
        rangeSel = clamp(rangeSel, 0, 2);

        float rate = params[RATE_PARAM].getValue();
        rate += (inputs[RATE_CV_INPUT].isConnected()
                     ? params[RATE_CV_PARAM].getValue() * inputs[RATE_CV_INPUT].getVoltage() / 5.f
                     : 0.f);
        // const float effectiveTau = baseTau[rangeSel] * std::pow(2.f, -rate);
        const float effectiveTau = IntegrationRateQuantity::baseTau[rangeSel] * std::pow(2.f, -rate);

        /* --- leak multiplier ----------------------------- */
        float leakPos = params[LEAK_PARAM].getValue();
        if (inputs[LEAK_CV_INPUT].isConnected())
            leakPos = clamp(leakPos +
                                params[LEAK_CV_PARAM].getValue() * inputs[LEAK_CV_INPUT].getVoltage() / 5.f,
                            0.f, 1.f);

        const float tauLeak = DecayTimeQuantity::getTimeConstantForValue(leakPos);

        float leakMul;
        if (std::isinf(tauLeak)) {  // treat near-zero as ideal
            leakMul = 1.f;
        } else {
            const float safeTauLeak = std::max(tauLeak, 1e-9f);
            leakMul = std::exp(-dt / safeTauLeak);  // exact decay multiplier
        }

        /* --- integrator step ----------------------------- */
        bool shouldIntegrate = true;
        if (inputs[GATE_INPUT].isConnected()) {
            float gateValue = inputs[GATE_INPUT].getVoltage();
            if (gateMode && gateValue > 0.f) {
                shouldIntegrate = true;
            } else if (!gateMode && gateValue == 0.f) {
                shouldIntegrate = true;
            } else {
                shouldIntegrate = false;
            }
        }
        if (shouldIntegrate) {
            y = y * leakMul + v * (dt / effectiveTau);
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
            lights[GATE_LIGHT].setBrightness(gateMode);
        }
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "gateMode", json_boolean(gateMode));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* gateModeJ = json_object_get(rootJ, "gateMode");
        if (gateModeJ) gateMode = json_boolean_value(gateModeJ);
    }
};

/* ───────────── WIDGET ─────────────────────────────── */
struct IntegratorWidget : rack::ModuleWidget {
    IntegratorWidget(Integrator* m) {
        setModule(m);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Integrator.svg"), asset::plugin(pluginInstance, "res/Integrator-dark.svg")));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        addChild(createLightCentered<LargeFresnelLight<GreenRedLight>>(Vec(45.0, 35.0), module, Integrator::OUT_POS_LIGHT));

        addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 53.39), m, Integrator::RATE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 53.39), m, Integrator::LEAK_PARAM));

        addParam(createParamCentered<VCVButtonHuge>(Vec(22.5, 104.35), module, Integrator::INIT_BUTTON_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 104.35), module, Integrator::INIT_PARAM));

        addParam(createParamCentered<CKSSThree>(Vec(16.54, 162.66), m, Integrator::RANGE_PARAM));
        addParam(createParamCentered<CKSS>(Vec(54.74, 162.66), m, Integrator::CLIP_PARAM));

        addParam(createParamCentered<Trimpot>(Vec(15.f, 203.79), module, Integrator::RATE_CV_PARAM));
        addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<GoldLight>>>(Vec(45, 203.79), module, Integrator::GATE_PARAM, Integrator::GATE_LIGHT));
        addParam(createParamCentered<Trimpot>(Vec(75.f, 203.79), module, Integrator::LEAK_CV_PARAM));

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(15.f, 231.31), m, Integrator::RATE_CV_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(45.f, 231.31), m, Integrator::GATE_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(75.f, 231.31), m, Integrator::LEAK_CV_INPUT));

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(22.5, 280.1), m, Integrator::RESET_INPUT));
        addParam(createParamCentered<VCVButton>(Vec(67.5, 280.1), m, Integrator::RESET_PARAM));

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(22.5, 329.25), m, Integrator::IN_INPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(67.5, 329.25), m, Integrator::OUT_OUTPUT));
    }
};

Model* modelIntegrator = rack::createModel<Integrator, IntegratorWidget>("Integrator");
