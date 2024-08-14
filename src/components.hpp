#pragma once

#include "plugin.hpp"
#include "rack.hpp"


template <typename TBase>
struct LargeFresnelLight : TSvgLight<TBase> {
    LargeFresnelLight() {
        this->setSvg(Svg::load(asset::plugin(pluginInstance, "res/components/LargeFresnelLight.svg")));
    }
};

template <typename TBase>
struct MediumFresnelLight : TSvgLight<TBase> {
    MediumFresnelLight() {
        this->setSvg(Svg::load(asset::plugin(pluginInstance, "res/components/MediumFresnelLight.svg")));
    }
};

struct ScrewGrey : app::SvgScrew {
    ScrewGrey() {
        setSvg(Svg::load(asset::plugin(pluginInstance, "res/components/ScrewGrey.svg")));
    }
};