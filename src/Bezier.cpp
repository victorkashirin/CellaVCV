#include "components.hpp"
#include "plugin.hpp"

struct Bezier : Module {
    enum ParamId {
        FREQ_PARAM,
        LEVEL_PARAM,
        CURVE_PARAM,
        OFFSET_PARAM,
        FM_PARAM,
        LEVEL_MOD_PARAM,
        LIMIT_SWITCH,
        PARAMS_LEN
    };
    enum InputId {
        SIGNAL_INPUT,
        FM_INPUT,
        LEVEL_MOD_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        CURVE_OUTPUT,
        ICURVE_OUTPUT,
        TRIG_OUTPUT,
        GATE_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        CURVE_POS_LIGHT,
        CURVE_NEG_LIGHT,
        GATE_LIGHT,
        LIGHTS_LEN
    };

    dsp::PulseGenerator pulseGenerator;
    dsp::ClockDivider lightDivider;
    std::mt19937 rand;
    std::normal_distribution<float> normalDistribution{0.0f, 1.6f};  // gives approximate -5..5

    float phase = 0.f;
    float currentValue = 0.f;
    float targetValue = 0.f;
    float fmParam = 0;
    float fm = 0;

    bool contLevelModulation = false;
    bool contFreqModulation = false;
    bool assymetricCurve = false;
    int distributionType = 0;
    int levelClipType = 0;
    float levels[4][2] = {
        {0.f, 1.f},
        {0.f, 2.f},
        {-1.f, 1.f},
        {-2.f, 2.f}};

    Bezier() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(FREQ_PARAM, -8.f, std::log2(50.f), 1.f, "Frequency", " Hz", 2, 1);
        configParam(LEVEL_PARAM, 0.f, 1.f, 1.f, "Level", "%", 0.f, 100.f);
        configParam(OFFSET_PARAM, -5.f, 5.f, 0.f, "Offset", "V");
        configParam(CURVE_PARAM, -1.f, 1.f, 0.0f, "Curve");
        configParam(FM_PARAM, -1.f, 1.f, 0.f, "Frequency modulation", "%", 0.f, 100.f);
        configParam(LEVEL_MOD_PARAM, -1.f, 1.f, 0.f, "Level modulation", "%", 0.f, 100.f);
        configSwitch(LIMIT_SWITCH, -1.f, 1.f, 1.f, "Limit", {"Wrap", "Fold", "Clip"});

        configInput(SIGNAL_INPUT, "Sampling");
        configInput(FM_INPUT, "Frequency modulation");
        configInput(LEVEL_MOD_INPUT, "Volume modulation");

        configOutput(CURVE_OUTPUT, "Curve");
        configOutput(ICURVE_OUTPUT, "Inverted curve");
        configOutput(TRIG_OUTPUT, "Trigger");
        configOutput(GATE_OUTPUT, "Gate");

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

    float wrap(float x, float a, float b) {
        float result = x;
        if (x > b) {
            result = a + (x - b);
        } else if (x < a) {
            result = b + (x - a);
        }
        if (result < a || result > b) return wrap(result, a, b);
        return result;
    }

    double getPoint(double aX, double mX1, double mY1, double mX2, double mY2) const {
        if (mX1 == mY1 && mX2 == mY2)
            return aX;  // linear
        return calculateBezier(getTForX(aX, mX1, mX2), mY1, mY2);
    }

    double A(double aA1, double aA2) const { return 1.0 - 3.0 * aA2 + 3.0 * aA1; }
    double B(double aA1, double aA2) const { return 3.0 * aA2 - 6.0 * aA1; }
    double C(double aA1) const { return 3.0 * aA1; }

    double calculateBezier(double aT, double aA1, double aA2) const {
        return ((A(aA1, aA2) * aT + B(aA1, aA2)) * aT + C(aA1)) * aT;
    }

    double getSlope(double aT, double aA1, double aA2) const {
        return 3.0 * A(aA1, aA2) * aT * aT + 2.0 * B(aA1, aA2) * aT + C(aA1);
    }

    double getTForX(double aX, double mX1, double mX2) const {
        // Newton raphson iteration
        double aGuessT = aX;
        for (int i = 0; i < 5; ++i) {
            double currentSlope = getSlope(aGuessT, mX1, mX2);
            if (currentSlope == 0.0)
                return aGuessT;
            double currentX = calculateBezier(aGuessT, mX1, mX2) - aX;
            aGuessT -= currentX / currentSlope;
        }
        return aGuessT;
    }

    void process(const ProcessArgs &args) override {
        float level = params[LEVEL_PARAM].getValue();
        float cv = clamp(inputs[LEVEL_MOD_INPUT].getVoltage() / 5.f, -2.f, 2.f);
        float levelModParam = params[LEVEL_MOD_PARAM].getValue();
        cv *= levelModParam;
        level += cv;
        level = clamp(level, levels[levelClipType][0], levels[levelClipType][1]);

        if (contFreqModulation == true) {
            fm = inputs[FM_INPUT].getVoltage();
            fmParam = 1.5 * params[FM_PARAM].getValue();
        }
        float pitch = params[FREQ_PARAM].getValue() + fm * fmParam;
        // Calculate the phase
        phase += args.sampleTime * std::pow(2.f, pitch);
        // more expensive
        // phase += args.sampleTime * dsp::exp2_taylor5(pitch);
        if (phase >= 1.f) {
            phase -= 1.f;
            currentValue = targetValue;
            if (inputs[SIGNAL_INPUT].isConnected()) {
                targetValue = inputs[SIGNAL_INPUT].getVoltage();
            } else {
                if (distributionType == 0)
                    targetValue = 5.f * (2.f * random::uniform() - 1.f);  // Bipolar random value
                else
                    targetValue = clamp(normalDistribution(rand), -5.f, 5.f);  // Bipolar random value
            }

            if (contLevelModulation == false) targetValue *= level;
            if (contFreqModulation == false) {
                fm = inputs[FM_INPUT].getVoltage();
                fmParam = 1.5 * params[FM_PARAM].getValue();
            }

            // SEND GATE
            pulseGenerator.trigger(1e-3f);
        }

        // CALCULATIONS
        float curvePoint = clamp(params[CURVE_PARAM].getValue(), -0.99f, 0.99f);
        float bezierValue = phase;
        if (curvePoint >= 0.f) {
            if (assymetricCurve == false) {
                bezierValue = getPoint(phase, curvePoint, 0.0f, (1.f - curvePoint), 1.0f);
            } else {
                bezierValue = getPoint(phase, curvePoint, 0.0f, 1.0f, (1.f - curvePoint));
            }
        } else {
            curvePoint *= -1;
            if (assymetricCurve == false) {
                bezierValue = getPoint(phase, 0.0f, curvePoint, 1.0f, (1.f - curvePoint));
            } else {
                bezierValue = getPoint(phase, 0.0f, curvePoint, (1.f - curvePoint), 1.0f);
            }
        }

        // Interpolate the current value using Bézier curve
        float outputValue = currentValue + bezierValue * (targetValue - currentValue);
        if (contLevelModulation == true) outputValue *= level;

        // OUTPUT
        float offset = params[OFFSET_PARAM].getValue();
        int limitSwitch = (int)params[LIMIT_SWITCH].getValue();
        switch (limitSwitch) {
            case 1:
                outputs[CURVE_OUTPUT].setVoltage(clamp(offset + outputValue, -5.f, 5.f));
                outputs[ICURVE_OUTPUT].setVoltage(clamp(offset - outputValue, -5.f, 5.f));
                break;
            case 0:
                outputs[CURVE_OUTPUT].setVoltage(fold(offset + outputValue, -5.f, 5.f));
                outputs[ICURVE_OUTPUT].setVoltage(fold(offset - outputValue, -5.f, 5.f));
                break;
            default:
                outputs[CURVE_OUTPUT].setVoltage(wrap(offset + outputValue, -5.f, 5.f));
                outputs[ICURVE_OUTPUT].setVoltage(wrap(offset - outputValue, -5.f, 5.f));
        }

        // TRIGGER
        outputs[TRIG_OUTPUT].setVoltage(pulseGenerator.process(args.sampleTime) ? 10.f : 0.f);

        // GATE
        outputs[GATE_OUTPUT].setVoltage(outputValue > 0.0 ? 10.f : 0.f);

        // LIGHT
        if (lightDivider.process()) {
            float lightTime = args.sampleTime * lightDivider.getDivision();
            lights[GATE_LIGHT].setBrightnessSmooth(outputValue > 0.0, lightTime);
            lights[CURVE_POS_LIGHT].setBrightnessSmooth(fmaxf(0.0f, outputValue / 5.f), lightTime);
            lights[CURVE_NEG_LIGHT].setBrightnessSmooth(fmaxf(0.0f, -outputValue / 5.f), lightTime);
        }
    }

    json_t *dataToJson() override {
        json_t *rootJ = json_object();
        json_object_set_new(rootJ, "continuousFrequency", json_boolean(contFreqModulation));
        json_object_set_new(rootJ, "continuousLevel", json_boolean(contLevelModulation));
        json_object_set_new(rootJ, "assymetricCurve", json_boolean(assymetricCurve));
        json_object_set_new(rootJ, "distributionType", json_integer(distributionType));
        json_object_set_new(rootJ, "levelClipType", json_integer(levelClipType));
        return rootJ;
    }

    void dataFromJson(json_t *rootJ) override {
        json_t *contLevelJ = json_object_get(rootJ, "continuousLevel");
        if (contLevelJ)
            contLevelModulation = json_boolean_value(contLevelJ);
        json_t *contFrequencyJ = json_object_get(rootJ, "continuousFrequency");
        if (contFrequencyJ)
            contFreqModulation = json_boolean_value(contFrequencyJ);
        json_t *assymJ = json_object_get(rootJ, "assymetricCurve");
        if (assymJ)
            assymetricCurve = json_boolean_value(assymJ);
        json_t *distributionJ = json_object_get(rootJ, "distributionType");
        if (distributionJ)
            distributionType = json_integer_value(distributionJ);
        json_t *levelClipTypeJ = json_object_get(rootJ, "levelClipType");
        if (levelClipTypeJ)
            levelClipType = json_integer_value(levelClipTypeJ);
    }
};

struct BezierWidget : ModuleWidget {
    BezierWidget(Bezier *module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Bezier.svg")));

        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        addChild(createLightCentered<LargeFresnelLight<GreenRedLight>>(Vec(45.0, 35.0), module, Bezier::CURVE_POS_LIGHT));

        addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 53.39), module, Bezier::FREQ_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 53.39), module, Bezier::LEVEL_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 104.35), module, Bezier::CURVE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 104.35), module, Bezier::OFFSET_PARAM));

        addInput(createInputCentered<PJ301MPort>(Vec(22.5, 159.15), module, Bezier::SIGNAL_INPUT));
        addParam(createParamCentered<CKSSThree>(Vec(54.74, 162.66), module, Bezier::LIMIT_SWITCH));

        addParam(createParamCentered<Trimpot>(Vec(22.5, 203.79), module, Bezier::FM_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(67.5, 203.79), module, Bezier::LEVEL_MOD_PARAM));

        addInput(createInputCentered<PJ301MPort>(Vec(22.5, 231.31), module, Bezier::FM_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(67.5, 231.31), module, Bezier::LEVEL_MOD_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(Vec(22.5, 280.1), module, Bezier::CURVE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(Vec(67.5, 280.1), module, Bezier::ICURVE_OUTPUT));

        addOutput(createOutputCentered<PJ301MPort>(Vec(22.5, 329.25), module, Bezier::TRIG_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(Vec(67.5, 329.25), module, Bezier::GATE_OUTPUT));

        addChild(createLightCentered<TinyLight<YellowLight>>(Vec(79.96, 318.0), module, Bezier::GATE_LIGHT));
    }

    void appendContextMenu(Menu *menu) override {
        Bezier *module = dynamic_cast<Bezier *>(this->module);
        assert(module);
        menu->addChild(new MenuSeparator);
        menu->addChild(createBoolPtrMenuItem("Continuous Frequency Modulation", "", &module->contFreqModulation));
        menu->addChild(createBoolPtrMenuItem("Continuous Level Modulation", "", &module->contLevelModulation));
        menu->addChild(createBoolPtrMenuItem("Assymetric Curve", "", &module->assymetricCurve));
        menu->addChild(createIndexPtrSubmenuItem("Distribution",
                                                 {"Uniform", "Normal"},
                                                 &module->distributionType));
        menu->addChild(createIndexPtrSubmenuItem("Post-Modulation Level Clip",
                                                 {"0..100%", "0..200%", "-100..100%", "-200..200%"},
                                                 &module->levelClipType));
    }
};

Model *modelBezier = createModel<Bezier, BezierWidget>("Bezier");