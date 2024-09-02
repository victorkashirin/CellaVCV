#include "components.hpp"
#include "plugin.hpp"

struct ElvinModule : Module {
    enum ParamIds {
        ATTACK_PARAM,
        DECAY_PARAM,
        SHAPE_PARAM,
        LVL_PARAM,
        STEPS_PARAM,
        ALVL_PARAM,
        ATTACK_CV_PARAM,
        DECAY_CV_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        TRIGGER_INPUT,
        ACCENT_INPUT,
        ATTACK_INPUT,
        DECAY_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        ENVELOPE_OUTPUT,
        ACCENT_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds {
        ENVELOPE_LIGHT,
        NUM_LIGHTS
    };

    static constexpr float MIN_TIME = 1e-3f;
    static constexpr float MAX_TIME = 10.f;
    static constexpr float LAMBDA_BASE = MAX_TIME / MIN_TIME;

    float phase = 0.f;
    float accent = 0.f;
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

    float binarySearch(float targetValue, float shape, float exponent, float a, float b, float epsilon) {
        float candidate = (a + b) / 2.f;
        float value = (1 - shape) * candidate + shape * std::powf(candidate, 1.0 / exponent);
        if (std::abs(targetValue - value) < epsilon) return candidate;
        if (value < targetValue) {
            return binarySearch(targetValue, shape, exponent, candidate, b, epsilon);
        } else {
            return binarySearch(targetValue, shape, exponent, a, candidate, epsilon);
        };
    }

    ElvinModule() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(ATTACK_PARAM, 0.f, 1.0f, 0.1f, "Attack Time", " ms", LAMBDA_BASE, MIN_TIME * 1000);
        configParam(DECAY_PARAM, 0.f, 1.0f, 0.1f, "Decay Time", " ms", LAMBDA_BASE, MIN_TIME * 1000);
        configParam(SHAPE_PARAM, 0.0f, 1.0f, 0.0f, "Envelope Shape");

        configParam(STEPS_PARAM, -8.f, 8.f, 3.f, "Accent Steps");
        paramQuantities[STEPS_PARAM]->snapEnabled = true;
        configParam(LVL_PARAM, 0.0f, 1.0f, 0.5f, "Base Level", "%", 0, 100);
        configParam(ALVL_PARAM, 0.f, 1.0f, 1.0f, "Accent Level", "%", 0, 100);

        configParam(ATTACK_CV_PARAM, -1.f, 1.f, 0.f, "Attack CV", "%", 0, 100);
        configParam(DECAY_CV_PARAM, -1.f, 1.f, 0.f, "Decay CV", "%", 0, 100);

        configInput(ATTACK_INPUT, "Attack");
        configInput(DECAY_INPUT, "Decay");
        configInput(TRIGGER_INPUT, "Trigger");
        configInput(ACCENT_INPUT, "Accent");

        configOutput(ENVELOPE_OUTPUT, "Envelope");
        configOutput(ACCENT_OUTPUT, "Accent Level");

        lightDivider.setDivision(4);
    }

    void process(const ProcessArgs &args) override {
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
            float nextAccentScale;

            if (accentTrigger > 0.f && steps != 0.f) {
                nextAccent = clamp(accent + (1.f / std::abs(steps)), 0.f, 1.f);
                nextAccentScale = accentTrigger / 10.f;
            } else {
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
            float decayLambda = std::powf(LAMBDA_BASE, -decay) / MIN_TIME;
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
            float attackLambda = std::powf(LAMBDA_BASE, -attack) / MIN_TIME;
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
            expEnvelope = std::powf(phase, exponent);
        } else {
            expEnvelope = (isAttacking) ? std::powf(phase, 1.f / exponent) : std::powf(phase, exponent);
        }

        float envelopeMix = (1 - shape) * phase + shape * expEnvelope;

        // -------------

        // strategy II

        float usedAccent = (preserveAccent == true) ? preserveAccentValue : accent;
        float usedAccentScale = (preserveAccent == true) ? preserveAccentScaleValue : accentScale;

        if (steps >= 0.f) {
            envelopeValue = envelopeMix * 10.f * (baseLevel + usedAccent * usedAccentScale * accentLevel * (1 - baseLevel));
        } else {
            envelopeValue = envelopeMix * 10.f * baseLevel * (1.f - usedAccent * usedAccentScale * accentLevel);
        }

        // Strategy I
        if (crossfadeValue != -1.f) {
            float crossfadeLambda = std::powf(LAMBDA_BASE, -decayForCrossfade * 0.55f) / MIN_TIME;
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
            lights[ENVELOPE_LIGHT].setBrightnessSmooth(std::powf(envelopeValue / 10.f, 2.f), lightTime);
        }
    }

    json_t *dataToJson() override {
        json_t *rootJ = json_object();
        json_object_set_new(rootJ, "exponentialAttack", json_boolean(exponentialAttack));
        json_object_set_new(rootJ, "retriggerStrategy", json_boolean(retriggerStrategy));
        json_object_set_new(rootJ, "exponentType", json_boolean(exponentType));
        return rootJ;
    }

    void dataFromJson(json_t *rootJ) override {
        json_t *expAttackJ = json_object_get(rootJ, "exponentialAttack");
        if (expAttackJ) exponentialAttack = json_boolean_value(expAttackJ);
        json_t *retriggerStrategyJ = json_object_get(rootJ, "retriggerStrategy");
        if (retriggerStrategyJ) retriggerStrategy = json_boolean_value(retriggerStrategyJ);
        json_t *exponentJ = json_object_get(rootJ, "exponentType");
        if (exponentJ) exponentType = json_integer_value(exponentJ);
    }
};

// Module widget
struct ElvinModuleWidget : ModuleWidget {
    ElvinModuleWidget(ElvinModule *module) {
        setModule(module);

        setPanel(createPanel(asset::plugin(pluginInstance, "res/Elvin.svg")));
        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        addChild(createLightCentered<LargeFresnelLight<BlueLight>>(Vec(45.0, 35.0), module, ElvinModule::ENVELOPE_LIGHT));

        addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 53.59), module, ElvinModule::ATTACK_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 53.59), module, ElvinModule::DECAY_PARAM));

        addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 103.5), module, ElvinModule::SHAPE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 103.5), module, ElvinModule::STEPS_PARAM));

        addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 153.38), module, ElvinModule::LVL_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 153.38), module, ElvinModule::ALVL_PARAM));

        addParam(createParamCentered<Trimpot>(Vec(22.5, 203.79), module, ElvinModule::ATTACK_CV_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(67.5, 203.79), module, ElvinModule::DECAY_CV_PARAM));

        addInput(createInputCentered<PJ301MPort>(Vec(22.5, 231.31), module, ElvinModule::ATTACK_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(67.5, 231.31), module, ElvinModule::DECAY_INPUT));

        // Trigger input
        addInput(createInputCentered<PJ301MPort>(Vec(22.5, 280.0), module, ElvinModule::TRIGGER_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(67.5, 280.0), module, ElvinModule::ACCENT_INPUT));

        // Envelope output
        addOutput(createOutputCentered<PJ301MPort>(Vec(22.5, 329.25), module, ElvinModule::ACCENT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(Vec(67.5, 329.25), module, ElvinModule::ENVELOPE_OUTPUT));
    }

    void appendContextMenu(Menu *menu) override {
        ElvinModule *module = dynamic_cast<ElvinModule *>(this->module);
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
Model *modelElvin = createModel<ElvinModule, ElvinModuleWidget>("Elvin");
