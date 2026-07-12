#include <cmath>

#include "LoudnessOutMessage.hpp"
#include "components.hpp"
#include "plugin.hpp"

using namespace rack;

struct LoudnessOut : Module {
    enum ParamIds {
        RESPONSE_PARAM,
        SCALE_PARAM,
        OUTPUT_MODE_PARAM,
        NUM_PARAMS
    };
    enum InputIds { NUM_INPUTS };
    enum OutputIds {
        OVERSHOOT_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds { NUM_LIGHTS };

    enum Response {
        SHORT_TERM_RESPONSE,
        MOMENTARY_RESPONSE
    };

    enum OutputMode {
        OVERSHOOT_ONLY,
        BIPOLAR_ERROR
    };

    LoudnessOutMessage leftMessages[2];

    LoudnessOut() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configSwitch(RESPONSE_PARAM, SHORT_TERM_RESPONSE, MOMENTARY_RESPONSE, MOMENTARY_RESPONSE,
                     "Response", {"Short-term", "Momentary"});
        ParamQuantity* scaleQuantity = configParam(SCALE_PARAM, -3.f, 1.f, -1.f,
                                                   "Output scale", " V/LU", 2.f);
        scaleQuantity->description = "Voltage generated for each LU above or below the target";
        configSwitch(OUTPUT_MODE_PARAM, OVERSHOOT_ONLY, BIPOLAR_ERROR, OVERSHOOT_ONLY,
                     "Output mode", {"Overshoot only", "Bipolar signed error"});
        PortInfo* outputInfo = configOutput(OVERSHOOT_OUTPUT, "Loudness overshoot / error");
        outputInfo->description = "Loudness relative to the target, scaled and limited to +/-10 V";

        leftExpander.producerMessage = &leftMessages[0];
        leftExpander.consumerMessage = &leftMessages[1];
    }

    bool hasCompatibleParent() const {
        return leftExpander.module &&
               (leftExpander.module->model == modelLoud || leftExpander.module->model == modelLoudnessMeter);
    }

    void process(const ProcessArgs& args) override {
        float voltage = 0.f;

        if (hasCompatibleParent()) {
            const LoudnessOutMessage* message = static_cast<const LoudnessOutMessage*>(leftExpander.consumerMessage);
            float measuredLufs = message->momentaryLufs;
            switch (static_cast<int>(std::round(params[RESPONSE_PARAM].getValue()))) {
                case SHORT_TERM_RESPONSE:
                    measuredLufs = message->shortTermLufs;
                    break;
                default:
                    break;
            }

            if (std::isfinite(measuredLufs) && std::isfinite(message->targetLufs)) {
                const float voltsPerLu = std::exp2(params[SCALE_PARAM].getValue());
                float loudnessDifference = measuredLufs - message->targetLufs;
                const int outputMode = static_cast<int>(std::round(params[OUTPUT_MODE_PARAM].getValue()));
                if (outputMode == OVERSHOOT_ONLY) {
                    loudnessDifference = std::fmax(loudnessDifference, 0.f);
                }
                voltage = clamp(loudnessDifference * voltsPerLu, -10.f, 10.f);
            }
        }

        outputs[OVERSHOOT_OUTPUT].setVoltage(voltage);
    }
};

struct LoudnessOutWidget : ModuleWidget {
    LoudnessOutWidget(LoudnessOut* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/LO.svg"),
                             asset::plugin(pluginInstance, "res/LO-dark.svg")));

        addParam(createParamCentered<CKSS>(Vec(9.5f, 153.5f), module, LoudnessOut::RESPONSE_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(15.f, 203.79f), module, LoudnessOut::SCALE_PARAM));
        addParam(createParamCentered<CKSS>(Vec(9.5f, 252.5f), module, LoudnessOut::OUTPUT_MODE_PARAM));
        addOutput(createOutputCentered<ThemedPJ301MPort>(Vec(15.f, 329.25f), module, LoudnessOut::OVERSHOOT_OUTPUT));
    }
};

Model* modelLoudnessOut = createModel<LoudnessOut, LoudnessOutWidget>("LoudnessOut");
