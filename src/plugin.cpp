#include "plugin.hpp"

Plugin *pluginInstance;

void init(Plugin *p) {
    pluginInstance = p;

    p->addModel(modelEuler);
    p->addModel(modelBezier);
    p->addModel(modelRich);
    p->addModel(modelTwinPeaks);
    p->addModel(modelResonators);
    p->addModel(modelBytebeat);
    p->addModel(modelCognitiveShift);
    p->addModel(modelLoudnessMeter);
}
