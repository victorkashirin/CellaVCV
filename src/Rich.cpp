#include "components.hpp"
#include "plugin.hpp"

struct Rich : Module {
    enum ParamIds {
        ATTACK_PARAM,
        DECAY_PARAM,
        SHAPE_PARAM,
        LVL_PARAM,
        STEPS_PARAM,
        ALVL_PARAM,
        ATTACK_CV_PARAM,
        DECAY_CV_PARAM,
        INVERT_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        TRIGGER_INPUT,
        ACCENT_INPUT,
        ATTACK_INPUT,
        DECAY_INPUT,
        INVERT_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        ENVELOPE_OUTPUT,
        ACCENT_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds {
        ENVELOPE_LIGHT,
        INVERT_LIGHT,
        NUM_LIGHTS
    };

    static constexpr float MIN_TIME = 1e-3f;
    static constexpr float MAX_TIME = 10.f;
    static constexpr float LAMBDA_BASE = MAX_TIME / MIN_TIME;

    bool invert = false;
    float phase = 0.f;

    float accentCounter = 0.f;
    float accent = 1.f;
    float accentScale = 0.f;
    bool isAttacking = false;
    bool isDecaying = false;
    float envelopeValue = 0.f;

    bool exponentialAttack = false;
    bool retriggerStrategy = false;
    int exponentType = 0;

    bool preserveAccent = false;
    float preserveAccentValue = -1.f;
    float preserveAccentScaleValue = -1.f;

    float crossfadeValue = -1.f;
    float crossfadePhase = 0.f;

    dsp::SchmittTrigger trigger;
    dsp::ClockDivider lightDivider;
    dsp::BooleanTrigger invertBoolean;
    dsp::SchmittTrigger invertSchmitt;

    float binarySearch(float targetValue, float shape, float exponent, float a, float b, float epsilon) {
        float candidate = (a + b) / 2.f;
        float value = (1 - shape) * candidate + shape * std::pow(candidate, 1.0 / exponent);
        if (std::abs(targetValue - value) < epsilon) return candidate;
        if (value < targetValue) {
            return binarySearch(targetValue, shape, exponent, candidate, b, epsilon);
        } else {
            return binarySearch(targetValue, shape, exponent, a, candidate, epsilon);
        };
    }

    Rich() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(ATTACK_PARAM, 0.f, 1.0f, 0.0f, "Attack time", " ms", LAMBDA_BASE, MIN_TIME * 1000);
        configParam(DECAY_PARAM, 0.f, 1.0f, std::log2(400.f) / std::log2(LAMBDA_BASE), "Decay time", " ms", LAMBDA_BASE, MIN_TIME * 1000);
        configParam(SHAPE_PARAM, 0.0f, 1.0f, 1.0f, "Envelope shape");

        configParam(STEPS_PARAM, -8.f, 8.f, 3.f, "Accent Steps");
        paramQuantities[STEPS_PARAM]->snapEnabled = true;
        configParam(LVL_PARAM, 0.0f, 1.0f, 0.75f, "Base level", "%", 0, 100);
        configParam(ALVL_PARAM, 0.f, 1.0f, 1.0f, "Accent level", "%", 0, 100);

        configParam(ATTACK_CV_PARAM, -1.f, 1.f, 0.f, "Attack CV", "%", 0, 100);
        configParam(DECAY_CV_PARAM, -1.f, 1.f, 0.f, "Decay CV", "%", 0, 100);
        configButton(INVERT_PARAM, "Asc/Desc accent");

        configInput(ATTACK_INPUT, "Attack");
        configInput(DECAY_INPUT, "Decay");
        configInput(TRIGGER_INPUT, "Trigger");
        configInput(ACCENT_INPUT, "Accent");
        configInput(INVERT_INPUT, "Asc/Desc accent");

        configOutput(ENVELOPE_OUTPUT, "Envelope");
        configOutput(ACCENT_OUTPUT, "Accent level");

        lightDivider.setDivision(4);
    }

    void process(const ProcessArgs &args) override {
        if (invertBoolean.process(params[INVERT_PARAM].getValue()))
            invert ^= true;

        // Invert input
        if (invertSchmitt.process(inputs[INVERT_INPUT].getVoltage(), 0.1f, 1.f))
            invert ^= true;

        float deltaTime = args.sampleTime;
        float baseLevel = params[LVL_PARAM].getValue();
        float accentLevel = params[ALVL_PARAM].getValue();
        float steps = params[STEPS_PARAM].getValue();
        float shape = params[SHAPE_PARAM].getValue();
        float exponent = exponentType + 2.f;

        // Check if the trigger input is high
        if (trigger.process(inputs[TRIGGER_INPUT].getVoltage()) && !isAttacking) {
            float accentTrigger = clamp(inputs[ACCENT_INPUT].getVoltage(), 0.f, 10.f);

            float nextAccent;
            float nextAccentCounter;
            float nextAccentScale;

            if (accentTrigger > 0.f && steps != 0.f) {
                nextAccentCounter = clamp(accentCounter + 1, 1.f, std::abs(steps));
                if (!invert) {
                    nextAccent = clamp(nextAccentCounter / std::abs(steps), 0.f, 1.f);
                } else {
                    nextAccent = clamp((std::abs(steps) + 1 - nextAccentCounter) / std::abs(steps), 0.f, 1.f);
                }

                nextAccentScale = accentTrigger / 10.f;
            } else {
                nextAccentCounter = 0.f;
                nextAccent = 0.f;
                nextAccentScale = 0.f;
            }

            if (isDecaying) {
                // retrigger
                float nextPeakValue;
                if (steps >= 0.f) {
                    nextPeakValue = 10.f * (baseLevel + nextAccent * nextAccentScale * accentLevel * (1 - baseLevel));
                } else {
                    nextPeakValue = 10.f * baseLevel * (1.f - nextAccent * nextAccentScale * accentLevel);
                }

                if (nextPeakValue > envelopeValue) {
                    preserveAccent = false;
                    preserveAccentValue = -1.f;
                    preserveAccentScaleValue = -1.f;

                    // normal condition
                    float intermediaryEnvelopeValue = envelopeValue / nextPeakValue;
                    float exp = (exponentialAttack) ? 1.f / exponent : exponent;
                    phase = binarySearch(intermediaryEnvelopeValue, shape, exp, 0.f, 1.f, 0.001f);
                    isAttacking = true;
                    isDecaying = false;
                } else {
                    if (retriggerStrategy == false) {
                        phase = 1.f;
                        isAttacking = false;
                        isDecaying = true;
                        crossfadeValue = envelopeValue;
                    } else {
                        preserveAccent = true;
                        if (preserveAccentValue == -1.f) {
                            preserveAccentValue = accent;
                            preserveAccentScaleValue = accentScale;
                        }
                    }
                }
            } else {
                isAttacking = true;
                isDecaying = false;
            }
            accentCounter = nextAccentCounter;
            accent = nextAccent;
            accentScale = nextAccentScale;
        }

        // Process the decay stage
        float decayForCrossfade = 0.f;
        if (isDecaying) {
            float decayParam = params[DECAY_PARAM].getValue();
            float decayCvParam = params[DECAY_CV_PARAM].getValue();
            float decay = decayParam + inputs[DECAY_INPUT].getVoltage() / 10.f * decayCvParam;
            decay = clamp(decay, 0.f, 1.f);
            decayForCrossfade = decay;
            float decayLambda = std::pow(LAMBDA_BASE, -decay) / MIN_TIME;
            phase -= deltaTime * decayLambda;
            if (phase <= 0.0f) {
                phase = 0.0f;
                isDecaying = false;
            }
        }

        // Process the attack stage
        if (isAttacking) {
            float attackParam = params[ATTACK_PARAM].getValue();
            float attackCvParam = params[ATTACK_CV_PARAM].getValue();
            float attack = attackParam + inputs[ATTACK_INPUT].getVoltage() / 10.f * attackCvParam;
            attack = clamp(attack, 0.f, 1.f);
            float attackLambda = std::pow(LAMBDA_BASE, -attack) / MIN_TIME;
            phase += deltaTime * attackLambda;
            if (phase >= 1.0) {
                phase = 1.0;
                isAttacking = false;
                isDecaying = true;
            }
        }

        //----------- Merging envelopes ---------------

        float expEnvelope;
        if (exponentialAttack) {
            expEnvelope = std::pow(phase, exponent);
        } else {
            expEnvelope = (isAttacking) ? std::pow(phase, 1.f / exponent) : std::pow(phase, exponent);
        }

        float envelopeMix = (1.f - shape) * phase + shape * expEnvelope;

        // -------------

        // Retrigger strategy II

        float usedAccent = (preserveAccent == true) ? preserveAccentValue : accent;
        float usedAccentScale = (preserveAccent == true) ? preserveAccentScaleValue : accentScale;

        // Envelope update

        if (steps >= 0.f) {
            envelopeValue = envelopeMix * 10.f * (baseLevel + usedAccent * usedAccentScale * accentLevel * (1 - baseLevel));
        } else {
            envelopeValue = envelopeMix * 10.f * baseLevel * (1.f - usedAccent * usedAccentScale * accentLevel);
        }

        // Retrigger Strategy I
        if (crossfadeValue != -1.f) {
            float crossfadeLambda = std::pow(LAMBDA_BASE, -decayForCrossfade * 0.55f) / MIN_TIME;
            crossfadePhase += deltaTime * crossfadeLambda;
            if (crossfadePhase > 1.f) {
                crossfadeValue = -1.f;
                crossfadePhase = 0.f;
            } else {
                envelopeValue = crossfade(crossfadeValue, envelopeValue, crossfadePhase);
            }
        }

        // ----------------------------------------

        outputs[ENVELOPE_OUTPUT].setVoltage(envelopeValue);
        outputs[ACCENT_OUTPUT].setVoltage(10.f * usedAccent * usedAccentScale);

        if (lightDivider.process()) {
            float lightTime = args.sampleTime * lightDivider.getDivision();
            lights[ENVELOPE_LIGHT].setBrightnessSmooth(std::pow(envelopeValue / 10.f, 2.f), lightTime);
            lights[INVERT_LIGHT].setBrightness(invert * 0.4f);
        }
    }

    json_t *dataToJson() override {
        json_t *rootJ = json_object();
        json_object_set_new(rootJ, "exponentialAttack", json_boolean(exponentialAttack));
        json_object_set_new(rootJ, "retriggerStrategy", json_boolean(retriggerStrategy));
        json_object_set_new(rootJ, "exponentType", json_boolean(exponentType));
        json_object_set_new(rootJ, "invert", json_boolean(invert));
        return rootJ;
    }

    void dataFromJson(json_t *rootJ) override {
        json_t *expAttackJ = json_object_get(rootJ, "exponentialAttack");
        if (expAttackJ) exponentialAttack = json_boolean_value(expAttackJ);
        json_t *retriggerStrategyJ = json_object_get(rootJ, "retriggerStrategy");
        if (retriggerStrategyJ) retriggerStrategy = json_boolean_value(retriggerStrategyJ);
        json_t *exponentJ = json_object_get(rootJ, "exponentType");
        if (exponentJ) exponentType = json_integer_value(exponentJ);
        json_t *invertJ = json_object_get(rootJ, "invert");
        if (invertJ) invert = json_boolean_value(invertJ);
    }
};

// Module widget
struct RichWidget : ModuleWidget {
    RichWidget(Rich *module) {
        setModule(module);

        setPanel(createPanel(asset::plugin(pluginInstance, "res/Rich.svg")));
        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        addChild(createLightCentered<LargeFresnelLight<BlueLight>>(Vec(45.0, 35.0), module, Rich::ENVELOPE_LIGHT));

        addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 53.59), module, Rich::ATTACK_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 53.59), module, Rich::DECAY_PARAM));

        addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 103.5), module, Rich::SHAPE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 103.5), module, Rich::STEPS_PARAM));

        addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 153.38), module, Rich::LVL_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 153.38), module, Rich::ALVL_PARAM));

        addParam(createParamCentered<Trimpot>(Vec(15, 203.79), module, Rich::ATTACK_CV_PARAM));
        addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<WhiteLight>>>(Vec(45, 203.79), module, Rich::INVERT_PARAM, Rich::INVERT_LIGHT));
        addParam(createParamCentered<Trimpot>(Vec(75, 203.79), module, Rich::DECAY_CV_PARAM));

        addInput(createInputCentered<PJ301MPort>(Vec(15, 231.31), module, Rich::ATTACK_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(45, 231.31), module, Rich::INVERT_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(75, 231.31), module, Rich::DECAY_INPUT));

        // Trigger input
        addInput(createInputCentered<PJ301MPort>(Vec(22.5, 280.0), module, Rich::TRIGGER_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(67.5, 280.0), module, Rich::ACCENT_INPUT));

        // Envelope output
        addOutput(createOutputCentered<PJ301MPort>(Vec(22.5, 329.25), module, Rich::ACCENT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(Vec(67.5, 329.25), module, Rich::ENVELOPE_OUTPUT));
    }

    void appendContextMenu(Menu *menu) override {
        Rich *module = dynamic_cast<Rich *>(this->module);
        assert(module);
        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexPtrSubmenuItem("Attack Curve",
                                                 {"Logarithmic",
                                                  "Exponential"},
                                                 &module->exponentialAttack));
        menu->addChild(createIndexPtrSubmenuItem("Retrigger Strategy",
                                                 {"I",
                                                  "II"},
                                                 &module->retriggerStrategy));
        menu->addChild(createIndexPtrSubmenuItem("Exponent Function",
                                                 {"Quadratic", "Cubic", "Quartic"},
                                                 &module->exponentType));
    }
};

// Plugin declaration
Model *modelRich = createModel<Rich, RichWidget>("Rich");
