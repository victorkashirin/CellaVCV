#include "components.hpp"
#include "plugin.hpp"

struct Integral : rack::Module {
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

    float y = 0.f;
    bool gateMode = false;
    int clipValue = 1;

    dsp::SchmittTrigger resetButtonTrigger;
    dsp::SchmittTrigger resetInputTrigger;
    dsp::ClockDivider lightDivider;
    dsp::BooleanTrigger gateBoolean;

    struct DecayTimeQuantity : rack::ParamQuantity {
        static constexpr float minTime = 0.001f;  // 1 ms
        static constexpr float maxTime = 100.f;   // 100 s
        static constexpr float epsilon = 0.001f;

        DecayTimeQuantity() {
            description = "Time required for the voltage to\ndecay towards zero by approximately 63%";
        }

        static float getLeakTimeConstant(float value, float minT, float maxT, float eps) {
            if (value < eps) {
                return std::numeric_limits<float>::infinity();
            }
            float logMin = std::log10(minT);
            float logMax = std::log10(maxT);
            float factor = (value - eps) / (1.f - eps);
            float timeLog = logMax + factor * (logMin - logMax);
            return std::pow(10.f, timeLog);
        }

        std::string getDisplayValueString() override {
            float knobValue = getValue();
            float time = getLeakTimeConstant(knobValue, minTime, maxTime, epsilon);

            if (std::isinf(time)) {
                return "Off";  // Or "Inf" or "∞"
            } else if (time >= 1.f) {
                return rack::string::f("%.1f s", time);
            } else if (time >= 0.001f) {
                return rack::string::f("%.1f ms", time * 1000.f);
            } else {
                return rack::string::f("%.1f µs", time * 1000000.f);
            }
        }

        static float getTimeConstantForValue(float value) {
            return getLeakTimeConstant(value, minTime, maxTime, epsilon);
        }
    };

    struct IntegrationRateQuantity : rack::ParamQuantity {
        static constexpr float baseTau[3] = {5.f, 0.25f, 0.00125f};      // LO, MID, HI
        const float minRate = 1.f / (baseTau[0] * std::pow(2.f, -5.f));  // Max Tau -> Min Rate
        const float maxRate = 1.f / (baseTau[2] * std::pow(2.f, 5.f));   // Min Tau -> Max Rate

        std::string getDisplayValueString() override {
            if (!module) {
                return rack::ParamQuantity::getDisplayValueString();
            }

            float rateKnobValue = getValue();
            Integral* integratorModule = dynamic_cast<Integral*>(module);
            if (!integratorModule) {
                return rack::ParamQuantity::getDisplayValueString();  // Fallback if cast fails
            }
            float rangeSwitchValue = module->params[Integral::RANGE_PARAM].getValue();
            int rangeSel = rack::clamp((int)std::round(rangeSwitchValue), 0, 2);
            float effectiveTau = baseTau[rangeSel] * std::pow(2.f, -rateKnobValue);  // Units: seconds

            float displayRate;  // Units: 1/s = Hz
            if (!std::isfinite(effectiveTau) || effectiveTau <= 0.f) {
                // effectiveTau is inf, nan, zero or negative -> rate is problematic
                if (rateKnobValue <= getMinValue()) {
                    return "<0.01 mHz";
                } else {
                    return "? Hz";
                }
            } else if (effectiveTau < 1e-9f) {
                displayRate = 1.0e9f;
            } else if (effectiveTau > 1e8f) {
                // Rate is extremely low
                displayRate = 0.f;
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
                if (displayRate >= 0.1f) return rack::string::f("%.1f mHz", displayRate * 1e3f);
                if (displayRate >= 0.01f) return rack::string::f("%.1f mHz", displayRate * 1e3f);
                return rack::string::f("%.2f mHz", displayRate * 1e3f);
            } else if (displayRate > 1e-5f) {
                return rack::string::f("%.2f mHz", displayRate * 1e3f);
            } else {
                return "<0.01 mHz";
            }
        }
    };

    Integral() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

        configParam<IntegrationRateQuantity>(RATE_PARAM, -5.f, 5.f, 0.f, "Integration rate", "");
        configParam<DecayTimeQuantity>(LEAK_PARAM, 0.f, 1.f, 0.f, "Decay time");

        configParam(INIT_PARAM, -10.f, 10.f, 10.f, "Init value", "V");
        configButton(INIT_BUTTON_PARAM, "Init");
        getParamQuantity(INIT_BUTTON_PARAM)->description =
            "Adds value of the Init knob to the input\n"
            "when the button is pressed";

        configSwitch(RANGE_PARAM, 0.f, 2.f, 1.f, "Rate range", {"Low", "Mid", "High"});

        configParam(RATE_CV_PARAM, -1.f, 1.f, 0.f, "Rate CV", "%", 0.f, 100.f);
        configParam(LEAK_CV_PARAM, -1.f, 1.f, 0.f, "Leak CV", "%", 0.f, 100.f);
        configButton(GATE_PARAM, "Integrate on gate low/high");

        configSwitch(CLIP_PARAM, 0.f, 2.f, 2.f, "Clip", {"Reset", "Fold", "Clip"});
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
        while (result < a || result > b) {
            if (result > b)
                result = 2 * b - result;
            else if (result < a)
                result = 2 * a - result;
            if (std::abs(result) > 1e6) break;
        }
        return result;
    }

    void onReset() override {
        y = 0.f;
    }

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

        int rangeSel = (int)std::round(params[RANGE_PARAM].getValue());
        rangeSel = clamp(rangeSel, 0, 2);

        float rate = params[RATE_PARAM].getValue();
        rate += (inputs[RATE_CV_INPUT].isConnected()
                     ? params[RATE_CV_PARAM].getValue() * inputs[RATE_CV_INPUT].getVoltage() / 2.f
                     : 0.f);
        const float effectiveTau = IntegrationRateQuantity::baseTau[rangeSel] * std::pow(2.f, -rate);

        float leakPos = params[LEAK_PARAM].getValue();
        if (inputs[LEAK_CV_INPUT].isConnected()) {
            float leakCV = params[LEAK_CV_PARAM].getValue() * inputs[LEAK_CV_INPUT].getVoltage() / 10.f;
            leakPos = clamp(leakPos + leakCV, 0.f, 1.f);
        }

        const float tauLeak = DecayTimeQuantity::getTimeConstantForValue(leakPos);

        float leakMul;
        if (std::isinf(tauLeak)) {
            leakMul = 1.f;
        } else {
            const float safeTauLeak = std::max(tauLeak, 1e-9f);
            leakMul = std::exp(-dt / safeTauLeak);
        }

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

        float clipValueVolt = 5.f * (clipValue + 1);
        const int clipSel = (int)std::round(params[CLIP_PARAM].getValue());
        if (clipSel == 2) {
            y = clamp(y, -clipValueVolt, clipValueVolt);
        } else if (clipSel == 0 && abs(y) > clipValueVolt) {
            y = 0.f;
        }

        float outputValue = y;
        if (clipSel == 1) {
            outputValue = fold(y, -clipValueVolt, clipValueVolt);
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
        json_object_set_new(rootJ, "clipValue", json_integer(clipValue));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* gateModeJ = json_object_get(rootJ, "gateMode");
        if (gateModeJ) gateMode = json_boolean_value(gateModeJ);

        json_t* clipValueJ = json_object_get(rootJ, "clipValue");
        if (clipValueJ) clipValue = json_integer_value(clipValueJ);
    }
};

constexpr float Integral::IntegrationRateQuantity::baseTau[3];

struct IntegralWidget : rack::ModuleWidget {
    IntegralWidget(Integral* m) {
        setModule(m);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Integral.svg"), asset::plugin(pluginInstance, "res/Integral-dark.svg")));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        addChild(createLightCentered<LargeFresnelLight<GreenRedLight>>(Vec(45.0, 35.0), module, Integral::OUT_POS_LIGHT));

        addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 53.39), m, Integral::RATE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 53.39), m, Integral::LEAK_PARAM));

        addParam(createParamCentered<VCVButtonHuge>(Vec(22.5, 104.35), module, Integral::INIT_BUTTON_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 104.35), module, Integral::INIT_PARAM));

        addParam(createParamCentered<CKSSThree>(Vec(16.54, 162.66), m, Integral::RANGE_PARAM));
        addParam(createParamCentered<CKSSThree>(Vec(54.74, 162.66), m, Integral::CLIP_PARAM));

        addParam(createParamCentered<Trimpot>(Vec(15.f, 203.79), module, Integral::RATE_CV_PARAM));
        addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<GoldLight>>>(Vec(45, 203.79), module, Integral::GATE_PARAM, Integral::GATE_LIGHT));
        addParam(createParamCentered<Trimpot>(Vec(75.f, 203.79), module, Integral::LEAK_CV_PARAM));

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(15.f, 231.31), m, Integral::RATE_CV_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(45.f, 231.31), m, Integral::GATE_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(75.f, 231.31), m, Integral::LEAK_CV_INPUT));

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(22.5, 280.1), m, Integral::RESET_INPUT));
        addParam(createParamCentered<VCVButton>(Vec(67.5, 280.1), m, Integral::RESET_PARAM));

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(22.5, 329.25), m, Integral::IN_INPUT));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(67.5, 329.25), m, Integral::OUT_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        Integral* module = dynamic_cast<Integral*>(this->module);
        assert(module);
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Settings"));
        menu->addChild(createIndexPtrSubmenuItem("Range",
                                                 {"-5V..5V",
                                                  "-10V..10V"},
                                                 &module->clipValue));
    }
};

Model* modelIntegral = rack::createModel<Integral, IntegralWidget>("Integral");
