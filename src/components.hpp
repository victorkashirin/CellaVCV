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

struct ScrewGrey : app::ThemedSvgScrew {
    ScrewGrey() {
        setSvg(Svg::load(asset::plugin(pluginInstance, "res/components/ScrewGrey.svg")), Svg::load(asset::plugin(pluginInstance, "res/components/ScrewDark.svg")));
    }
};

struct VCVButtonHuge : app::SvgSwitch {
    VCVButtonHuge() {
        momentary = true;
        this->addFrame(Svg::load(asset::plugin(pluginInstance, "res/components/VCVButtonHuge_0.svg")));
        this->addFrame(Svg::load(asset::plugin(pluginInstance, "res/components/VCVButtonHuge_1.svg")));
    }
};

struct VCVSwitchTiny : app::SvgSwitch {
    VCVSwitchTiny() {
        momentary = false;
        this->addFrame(Svg::load(asset::plugin(pluginInstance, "res/components/VCVButtonTiny_0.svg")));
        this->addFrame(Svg::load(asset::plugin(pluginInstance, "res/components/VCVButtonTiny_1.svg")));
    }
};

struct VCVButtonTiny : app::SvgSwitch {
    VCVButtonTiny() {
        momentary = true;
        this->addFrame(Svg::load(asset::plugin(pluginInstance, "res/components/VCVButtonTiny_0.svg")));
        this->addFrame(Svg::load(asset::plugin(pluginInstance, "res/components/VCVButtonTiny_1.svg")));
    }
};

template <typename TBase = GrayModuleLightWidget>
struct TGoldLight : TBase {
    TGoldLight() {
        // this->addBaseColor(nvgRGB(0xf7, 0xc5, 0xad));
        this->addBaseColor(nvgRGB(0xd9, 0xa1, 0x86));
    }
};
using GoldLight = TGoldLight<>;