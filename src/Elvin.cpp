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
        COUNT_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds {
        NUM_LIGHTS
    };

    static constexpr float MIN_TIME = 1e-3f;
    static constexpr float MAX_TIME = 8.f;
    static constexpr float LAMBDA_BASE = MAX_TIME / MIN_TIME;

    float envelope = 0.f;
    float accent = 0.f;
    float accentScale = 0.f;
    float maxEnvelope = 1.0f;
    bool isAttacking = false;
    bool isDecaying = false;
    float output = 0.f;

    float exponent = 2.f;

    dsp::SchmittTrigger trigger;

    float binarySearch(float output, float shape, float exponent, float a, float b, float epsilon) {
        float candidate = (a + b) / 2.f;
        float value = (1 - shape) * candidate + shape * std::pow(candidate, 1.0 / exponent);
        if (std::abs(output - value) < epsilon) return candidate;
        if (value < output) {
            return binarySearch(output, shape, exponent, candidate, b, epsilon);
        } else {
            return binarySearch(output, shape, exponent, a, candidate, epsilon);
        };
    }

    ElvinModule() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(ATTACK_PARAM, 0.f, 1.0f, 0.1f, "Attack Time", " ms", LAMBDA_BASE, MIN_TIME * 1000);
        configParam(DECAY_PARAM, 0.f, 1.0f, 0.1f, "Decay Time", " ms", LAMBDA_BASE, MIN_TIME * 1000);
        configParam(SHAPE_PARAM, 0.0f, 1.0f, 0.0f, "Envelope Shape");

        configParam(STEPS_PARAM, -8.f, 8.f, 3.f, "Steps");
        paramQuantities[STEPS_PARAM]->snapEnabled = true;
        configParam(LVL_PARAM, 0.0f, 1.0f, 0.5f, "Base Level");
        configParam(ALVL_PARAM, 0.f, 1.0f, 1.0f, "Accent Level");
    }

    void process(const ProcessArgs& args) override {
        float deltaTime = args.sampleTime;
        float baseLevel = params[LVL_PARAM].getValue();
        float accentLevel = params[ALVL_PARAM].getValue();
        float steps = params[STEPS_PARAM].getValue();
        float shape = params[SHAPE_PARAM].getValue();

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
                float nextPeakLevel;
                if (steps >= 0.f) {
                    nextPeakLevel = 10.f * (baseLevel + nextAccent * nextAccentScale * accentLevel * (1 - baseLevel));
                } else {
                    nextPeakLevel = 10.f * baseLevel * (1.f - nextAccent * nextAccentScale * accentLevel);
                }

                if (nextPeakLevel > output) {
                    float realEnvelope = output / nextPeakLevel;
                    envelope = binarySearch(realEnvelope, shape, exponent, 0.f, 1.f, 0.001f);
                    isAttacking = true;
                    isDecaying = false;
                } else {
                    // recover previous accent
                    nextAccent = accent;
                    nextAccentScale = accentScale;
                }
            } else {
                isAttacking = true;
                isDecaying = false;
            }
            accent = nextAccent;
            accentScale = nextAccentScale;
        }

        // Process the attack stage
        if (isAttacking) {
            float attackLambda = powf(LAMBDA_BASE, -params[ATTACK_PARAM].getValue()) / MIN_TIME;
            envelope += deltaTime * attackLambda;
            if (envelope >= 1.0) {
                envelope = 1.0;
                isAttacking = false;
                isDecaying = true;
            }
        }

        // Process the decay stage
        if (isDecaying) {
            float decayLambda = powf(LAMBDA_BASE, -params[DECAY_PARAM].getValue()) / MIN_TIME;
            envelope -= deltaTime * decayLambda;
            if (envelope <= 0.0f) {
                envelope = 0.0f;
                isDecaying = false;
            }
        }

        float expEnvelope = (isAttacking) ? std::pow(envelope, 1.f / exponent) : std::pow(envelope, exponent);

        float envelopeMix = (1 - shape) * envelope + shape * expEnvelope;

        if (steps >= 0.f) {
            output = envelopeMix * 10.f * (baseLevel + accent * accentScale * accentLevel * (1 - baseLevel));
        } else {
            output = envelopeMix * 10.f * baseLevel * (1.f - accent * accentScale * accentLevel);
        }

        // Output the envelope
        outputs[ENVELOPE_OUTPUT].setVoltage(output);
    }
};

// Module widget
struct ElvinModuleWidget : ModuleWidget {
    ElvinModuleWidget(ElvinModule* module) {
        setModule(module);

        setPanel(createPanel(asset::plugin(pluginInstance, "res/Elvin.svg")));
        addChild(createWidget<ScrewGrey>(Vec(0, 0)));
        addChild(createWidget<ScrewGrey>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Attack and Decay knobs
        addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 53.59), module, ElvinModule::ATTACK_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 53.59), module, ElvinModule::DECAY_PARAM));

        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 104.36), module, ElvinModule::STEPS_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 202.66), module, ElvinModule::LVL_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 202.66), module, ElvinModule::ALVL_PARAM));

        // Shape switch (Linear/Exponential)
        addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 104.36), module, ElvinModule::SHAPE_PARAM));

        // Trigger input
        addInput(createInputCentered<PJ301MPort>(Vec(22.5, 159.15), module, ElvinModule::TRIGGER_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(67.5, 159.15), module, ElvinModule::ACCENT_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(22.5, 280.0), module, ElvinModule::ATTACK_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(67.5, 280.0), module, ElvinModule::DECAY_INPUT));

        // Envelope output
        addOutput(createOutputCentered<PJ301MPort>(Vec(67.5, 329.25), module, ElvinModule::ENVELOPE_OUTPUT));
    }
};

// Plugin declaration
Model* modelElvin = createModel<ElvinModule, ElvinModuleWidget>("Elvin");
