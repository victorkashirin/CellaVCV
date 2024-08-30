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

    float envelope = 0.0f;
    float accent = 0.0f;
    float maxEnvelope = 1.0f;
    bool isAttacking = false;
    bool isDecaying = false;

    dsp::SchmittTrigger trigger;

    ElvinModule() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(ATTACK_PARAM, 0.001f, 1.0f, 0.1f, "Attack Time", "s");
        configParam(DECAY_PARAM, 0.001f, 1.0f, 0.1f, "Decay Time", "s");
        configParam(SHAPE_PARAM, 0.0f, 1.0f, 0.0f, "Envelope Shape");

        configParam(STEPS_PARAM, -8.f, 8.f, 3.f, "Steps");
        paramQuantities[STEPS_PARAM]->snapEnabled = true;
        configParam(LVL_PARAM, 0.0f, 1.0f, 0.5f, "Attack Mix");
        configParam(ALVL_PARAM, 0.f, 1.0f, 1.0f, "Accent Level");
    }

    void process(const ProcessArgs& args) override {
        float deltaTime = args.sampleTime;
        float baseLevel = params[LVL_PARAM].getValue();
        float accentLevel = params[ALVL_PARAM].getValue();
        float steps = params[STEPS_PARAM].getValue();
        float shape = params[SHAPE_PARAM].getValue();

        // Check if the trigger input is high
        if (trigger.process(inputs[TRIGGER_INPUT].getVoltage())) {
            isAttacking = true;
            isDecaying = false;

            if (inputs[ACCENT_INPUT].getVoltage() > 2.f && steps != 0.f) {
                accent = clamp(accent + (10.f / std::abs(steps)), 0.f, 10.f);
            } else {
                accent = 0.f;
            }
        }

        float output = 0.f;

        // Process the attack stage
        if (isAttacking) {
            envelope += deltaTime / params[ATTACK_PARAM].getValue();
            if (envelope >= 1.0) {
                envelope = 1.0;
                isAttacking = false;
                isDecaying = true;
            }
        }

        // Process the decay stage
        if (isDecaying) {
            envelope -= deltaTime / params[DECAY_PARAM].getValue();
            if (envelope <= 0.0f) {
                envelope = 0.0f;
                isDecaying = false;
            }
        }

        output = (1 - shape) * envelope + shape * std::pow(envelope, 4);
        if (steps >= 0.f) {
            output *= (10 * baseLevel + accent * accentLevel * (1 - baseLevel));
        } else {
            output *= baseLevel * (10 - accent * accentLevel);
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
        // addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 202.66), module, ElvinModule::STEPS_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 202.66), module, ElvinModule::LVL_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(Vec(67.5, 202.66), module, ElvinModule::ALVL_PARAM));

        // Shape switch (Linear/Exponential)
        // addParam(createParamCentered<CKSS>(Vec(22.5, 104.36), module, ElvinModule::SHAPE_PARAM));
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
