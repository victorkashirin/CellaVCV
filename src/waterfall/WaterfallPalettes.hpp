#pragma once

#include "WaterfallTypes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace cella {
namespace waterfall {

constexpr int PALETTE_CONTROL_POINTS = 12;
constexpr int PALETTE_LUT_SIZE = 256;

struct PaletteDefinition {
    const char* name;
    std::array<uint32_t, PALETTE_CONTROL_POINTS> colors;
};

/**
 * Canonical sRGB samples from CMasher, Matplotlib, and Seaborn colormaps.
 * Heat is retained from the original Waterfall shader for patch compatibility.
 * The LUT builder adds a short black silence ramp, matching spectrogram use.
 */
static const std::array<PaletteDefinition, static_cast<int>(Palette::COUNT)> PALETTE_DEFINITIONS = {{
    {"Heat", {{0x03020B, 0x20032C, 0x3E054D, 0x5E0861, 0x860E4F, 0xAF143D,
               0xD71A2B, 0xE54026, 0xF26721, 0xFF8E1C, 0xFFC269, 0xFFF6B8}}},
    {"Gray", {{0x000000, 0x171717, 0x2E2E2E, 0x454545, 0x5D5D5D, 0x747474,
               0x8B8B8B, 0xA2A2A2, 0xBABABA, 0xD1D1D1, 0xE8E8E8, 0xFFFFFF}}},
    {"Viridis", {{0x440154, 0x482173, 0x433E85, 0x38588C, 0x2D708E, 0x25858E,
                  0x1E9B8A, 0x2AB07F, 0x52C569, 0x86D549, 0xC2DF23, 0xFDE725}}},
    {"Amber", {{0x000000, 0x170B0E, 0x371B1F, 0x57272A, 0x79342E, 0x97422A,
                0xB25723, 0xC7701E, 0xD98E20, 0xE7AE2B, 0xF2CF3E, 0xFBF357}}},
    {"Amethyst", {{0x000000, 0x140D1A, 0x311D41, 0x4D2870, 0x6B2AAF, 0x7642E2,
                   0x7470DC, 0x7E91D6, 0x93AFD4, 0xAFCBD9, 0xD2E6E3, 0xFFFFFF}}},
    {"Apple", {{0x000000, 0x1E080E, 0x470D1C, 0x72031B, 0x8F2507, 0x9C4C01,
                0xA17112, 0x9D9531, 0x8EBB5E, 0x93D89E, 0xCAEAD5, 0xFFFFFF}}},
    {"Arctic", {{0x000000, 0x0F1018, 0x22263A, 0x323A60, 0x3C508D, 0x3967B9,
                 0x3584C8, 0x569DC9, 0x80B6CE, 0xAACDD8, 0xD4E5E8, 0xFFFFFF}}},
    {"Bubblegum", {{0x072238, 0x1E2D5C, 0x393184, 0x5A2EA9, 0x7E26C2, 0x9F27C9,
                    0xBB34C2, 0xD34AB5, 0xE566A7, 0xEF83A0, 0xF6A0A1, 0xFCBDA6}}},
    {"Chroma", {{0x000000, 0x020F31, 0x330273, 0x630272, 0x93025A, 0xB32A31,
                 0xBC5C0F, 0xB38C04, 0x8EBD2F, 0x22E97B, 0xAFF3CE, 0xFFFFFF}}},
    {"Cividis", {{0x00224E, 0x013271, 0x2F426D, 0x48526C, 0x5E636F, 0x727374,
                  0x878478, 0x9D9576, 0xB6A96F, 0xCEBC63, 0xE7D150, 0xFEE838}}},
    {"Cosmic", {{0x000000, 0x120A19, 0x2D1743, 0x48197A, 0x5F04C5, 0x573CE7,
                 0x406AE1, 0x2B8BDA, 0x1BA9D7, 0x0AC6D7, 0x0CE3D5, 0x5FFDD0}}},
    {"Cubehelix", {{0x000000, 0x19122B, 0x17344C, 0x185B48, 0x3C7632, 0x7E7A36,
                    0xBC7967, 0xD486AF, 0xCAA9E7, 0xC2D2F3, 0xD6F0EF, 0xFFFFFF}}},
    {"Dusk", {{0x000000, 0x070D18, 0x022531, 0x173934, 0x354B33, 0x57592C,
               0x7F6224, 0xAA6826, 0xD66B38, 0xF07968, 0xF29C9C, 0xF1BFCA}}},
    {"Eclipse", {{0x000000, 0x0C0F18, 0x16263C, 0x093E5A, 0x1F5660, 0x456965,
                  0x667B6C, 0x888E6E, 0xB39F6C, 0xDDB161, 0xF2CE57, 0xFEF255}}},
    {"Ember", {{0x000000, 0x110B18, 0x331631, 0x581A40, 0x811646, 0xA80A3E,
                0xC81D2A, 0xDD4615, 0xEA6E03, 0xF19307, 0xF4B921, 0xF1E13E}}},
    {"Emerald", {{0x000000, 0x0B1009, 0x1A2616, 0x253C1F, 0x2E5326, 0x316A2C,
                  0x268336, 0x0B9B54, 0x21B27B, 0x3FC8A0, 0x5CDFC6, 0x77F7EE}}},
    {"Fall", {{0x24051D, 0x47043D, 0x6A0641, 0x852038, 0x9D3A2D, 0xB0541F,
               0xBE710A, 0xC49208, 0xC1B54C, 0xCAD090, 0xE1E7CC, 0xFFFFFF}}},
    {"Flamingo", {{0x000000, 0x1B0C05, 0x40190B, 0x681E11, 0x94191F, 0xBD013F,
                   0xD72774, 0xE05CA1, 0xE28BC1, 0xE5B5D7, 0xEDDBEA, 0xFFFFFF}}},
    {"Freeze", {{0x000000, 0x120E1B, 0x292143, 0x3B3274, 0x4743B1, 0x375FDC,
                 0x1A82DF, 0x31A0D9, 0x61BBD6, 0x97D2D9, 0xCEE7E7, 0xFFFFFF}}},
    {"Gem", {{0x410206, 0x5A0125, 0x6D094D, 0x7C1779, 0x8727AA, 0x8A3DD5,
              0x835CF4, 0x717EFF, 0x5B9DFD, 0x47B8F9, 0x35D2F5, 0x44E9EE}}},
    {"Ghostlight", {{0x000000, 0x0F0E1A, 0x291E41, 0x4A285F, 0x633F60, 0x735863,
                    0x827169, 0x8D8C6E, 0x96AA71, 0x9CC96B, 0xC1E157, 0xFEF255}}},
    {"Gothic", {{0x000000, 0x0F0F1F, 0x252051, 0x442391, 0x7305C7, 0x9A21C9,
                 0xBD3FBF, 0xC96DBA, 0xC899D5, 0xCABFEF, 0xDAE1F8, 0xFFFFFF}}},
    {"Horizon", {{0x0D340B, 0x04482D, 0x0A594E, 0x1B6A70, 0x2F7A95, 0x4989BC,
                  0x6C95E2, 0x97A0F9, 0xBCB3F5, 0xD5CAF1, 0xEAE4F4, 0xFFFFFF}}},
    {"Inferno", {{0x000004, 0x140B34, 0x390963, 0x5F136E, 0x85216B, 0xA92E5E,
                  0xCB4149, 0xE65D2F, 0xF78410, 0xFCAE12, 0xF5DB4C, 0xFCFFA4}}},
    {"Jungle", {{0x000000, 0x07130E, 0x0E2E1D, 0x0C4725, 0x056225, 0x127B19,
                 0x4C8F0E, 0x77A143, 0x9BB674, 0xBCCCA3, 0xDDE4D2, 0xFFFFFF}}},
    {"Lavender", {{0x000000, 0x150512, 0x340737, 0x4B036B, 0x4B2A8E, 0x404D92,
                   0x376891, 0x318090, 0x26998C, 0x14B281, 0x30C96A, 0x77DB46}}},
    {"Lilac", {{0x000000, 0x011118, 0x022545, 0x282F75, 0x543984, 0x744980,
                0x8E5D79, 0xA37472, 0xB0916D, 0xB3B16F, 0xADD474, 0x92FB7A}}},
    {"Magma", {{0x000004, 0x120D31, 0x331067, 0x59157E, 0x7E2482, 0xA3307E,
                0xC83E73, 0xE95462, 0xFA7D5E, 0xFEA973, 0xFED395, 0xFCFDBF}}},
    {"Mako", {{0x0B0405, 0x231526, 0x35264C, 0x403974, 0x3D5296, 0x366DA0,
               0x3487A6, 0x35A1AB, 0x44BCAD, 0x6DD3AD, 0xAEE3C0, 0xDEF5E5}}},
    {"Neon", {{0xA70C03, 0xB51048, 0xC20888, 0xC619CB, 0xB451F5, 0x957DFE,
               0x6F9EFA, 0x3EB9EC, 0x0CD0D2, 0x27E2B1, 0x56F280, 0x8EFE15}}},
    {"Nuclear", {{0x000000, 0x0D0F08, 0x1D2516, 0x293B22, 0x31522D, 0x366937,
                  0x36803F, 0x329944, 0x2DB444, 0x3FCD3D, 0x68E52E, 0x97FC1A}}},
    {"Ocean", {{0x1C0041, 0x241E5A, 0x273770, 0x255081, 0x26688E, 0x307F98,
                0x4196A0, 0x5CADA8, 0x84C2B2, 0xB0D4C6, 0xD9E8E1, 0xFFFFFF}}},
    {"Pepper", {{0x3E0517, 0x5B0119, 0x730C10, 0x822707, 0x8D4102, 0x945903,
                 0x97710D, 0x97891B, 0x92A327, 0x87BD2F, 0x70D82E, 0x2DF617}}},
    {"Plasma", {{0x0D0887, 0x3E049C, 0x6300A7, 0x8606A6, 0xA62098, 0xC03A83,
                 0xD5546E, 0xE76F5A, 0xF68D45, 0xFDAE32, 0xFCD225, 0xF0F921}}},
    {"Rainforest", {{0x000000, 0x1E0520, 0x3B0164, 0x342C93, 0x095888, 0x05757B,
                    0x1A916B, 0x4FA946, 0xA4B702, 0xE0C25E, 0xFCD9B8, 0xFFFFFF}}},
    {"Rocket", {{0x03051A, 0x221331, 0x451C47, 0x691F55, 0x921C5B, 0xB91657,
                 0xD92847, 0xED503E, 0xF47D57, 0xF6A47C, 0xF7C9AA, 0xFAEBDD}}},
    {"Sapphire", {{0x000000, 0x0D0E14, 0x1F2131, 0x303451, 0x3F4676, 0x4B579E,
                   0x506EB9, 0x5D87BD, 0x719FC5, 0x85B8CE, 0x99D1DB, 0xADEDE8}}},
    {"Savanna", {{0x000000, 0x08121B, 0x022D33, 0x10453A, 0x305C37, 0x576F2A,
                  0x857E16, 0xB7890C, 0xDE9A4C, 0xEBB98F, 0xF3DCCC, 0xFFFFFF}}},
    {"Sepia", {{0x000000, 0x130D13, 0x2E1F2D, 0x4B2D43, 0x693D51, 0x834F58,
                0x9A645F, 0xAD7D68, 0xBC997A, 0xC8B794, 0xD4D5B5, 0xE4F4DA}}},
    {"Sunburst", {{0x000000, 0x1A0A16, 0x40142C, 0x691634, 0x951130, 0xBC121D,
                   0xD34006, 0xDC6E25, 0xDF9953, 0xE0BF89, 0xE6E1C6, 0xFFFFFF}}},
    {"Swamp", {{0x000000, 0x110F1A, 0x1C273C, 0x1B4152, 0x165D5F, 0x127766,
                0x219166, 0x49A960, 0x88BC71, 0xB3CF9F, 0xD9E5D0, 0xFFFFFF}}},
    {"Torch", {{0x000000, 0x041123, 0x002463, 0x392799, 0x663A91, 0x8E498B,
                0xB85580, 0xE36268, 0xFE824C, 0xFAB372, 0xF4DDBA, 0xFFFFFF}}},
    {"Toxic", {{0x000000, 0x100B1B, 0x221C48, 0x1D3664, 0x1A5067, 0x1C6869,
                0x138168, 0x059A5D, 0x28B442, 0x6FC810, 0xAED554, 0xD9E691}}},
    {"Tree", {{0x000000, 0x150905, 0x321808, 0x4C2703, 0x583F09, 0x5D5722,
               0x5D6F3B, 0x588851, 0x46A45F, 0x2CC05A, 0x24DB47, 0x37F611}}},
    {"Tropical", {{0x900EA5, 0xAC078D, 0xC50E6C, 0xD52E47, 0xD95128, 0xD4710E,
                   0xC79005, 0xB0AD26, 0x89CA53, 0x51E189, 0x24F1C5, 0x44FCFC}}},
    {"Turbo", {{0x30123B, 0x4454C3, 0x448FFE, 0x20C7DF, 0x2AEFA1, 0x7DFF56,
                0xC1F334, 0xF1CB3A, 0xFE9029, 0xEA4E0D, 0xBE2102, 0x7A0403}}},
    {"Voltage", {{0x000000, 0x1A0A16, 0x3E1439, 0x611267, 0x7C14A6, 0x833BD5,
                  0x7E66F1, 0x728EFE, 0x73B3FA, 0x9CCEF1, 0xCFE6F2, 0xFFFFFF}}},
}};

struct PaletteMenuGroup {
    const char* name;
    std::vector<Palette> palettes;
};

inline std::vector<PaletteMenuGroup> paletteMenuGroups() {
    return {
        {"Matplotlib",
         {Palette::GRAY, Palette::CIVIDIS, Palette::CUBEHELIX, Palette::INFERNO,
          Palette::MAGMA, Palette::PLASMA, Palette::TURBO, Palette::VIRIDIS}},
        {"Seaborn", {Palette::MAKO, Palette::ROCKET}},
        {"CMasher · A–F",
         {Palette::AMBER, Palette::AMETHYST, Palette::APPLE, Palette::ARCTIC,
          Palette::BUBBLEGUM, Palette::CHROMA, Palette::COSMIC, Palette::DUSK,
          Palette::ECLIPSE, Palette::EMBER, Palette::EMERALD, Palette::FALL,
          Palette::FLAMINGO, Palette::FREEZE}},
        {"CMasher · G–P",
         {Palette::GEM, Palette::GHOSTLIGHT, Palette::GOTHIC, Palette::HORIZON,
          Palette::JUNGLE, Palette::LAVENDER, Palette::LILAC, Palette::NEON,
          Palette::NUCLEAR, Palette::OCEAN, Palette::PEPPER}},
        {"CMasher · R–V",
         {Palette::RAINFOREST, Palette::SAPPHIRE, Palette::SAVANNA, Palette::SEPIA,
          Palette::SUNBURST, Palette::SWAMP, Palette::TORCH, Palette::TOXIC,
          Palette::TREE, Palette::TROPICAL, Palette::VOLTAGE}},
    };
}

inline const PaletteDefinition& paletteDefinition(Palette palette) {
    const int index = clampValue(static_cast<int>(palette), 0, static_cast<int>(Palette::COUNT) - 1);
    return PALETTE_DEFINITIONS[static_cast<size_t>(index)];
}

inline std::vector<std::string> paletteNames() {
    std::vector<std::string> names;
    names.reserve(PALETTE_DEFINITIONS.size());
    for (const PaletteDefinition& definition : PALETTE_DEFINITIONS) names.push_back(definition.name);
    return names;
}

inline std::array<unsigned char, PALETTE_LUT_SIZE * 4> buildPaletteLut(Palette palette) {
    std::array<unsigned char, PALETTE_LUT_SIZE * 4> result = {};
    const PaletteDefinition& definition = paletteDefinition(palette);
    constexpr float silenceRamp = 0.055f;
    for (int index = 0; index < PALETTE_LUT_SIZE; ++index) {
        const float value = static_cast<float>(index) / static_cast<float>(PALETTE_LUT_SIZE - 1);
        const float paletteValue = clampValue((value - silenceRamp) / (1.f - silenceRamp), 0.f, 1.f);
        const float position = paletteValue * static_cast<float>(PALETTE_CONTROL_POINTS - 1);
        const int left = clampValue(static_cast<int>(std::floor(position)), 0, PALETTE_CONTROL_POINTS - 1);
        const int right = std::min(left + 1, PALETTE_CONTROL_POINTS - 1);
        const float fraction = position - static_cast<float>(left);
        const float fade = clampValue(value / silenceRamp, 0.f, 1.f);
        const uint32_t leftColor = definition.colors[static_cast<size_t>(left)];
        const uint32_t rightColor = definition.colors[static_cast<size_t>(right)];
        for (int channel = 0; channel < 3; ++channel) {
            const int shift = (2 - channel) * 8;
            const float a = static_cast<float>((leftColor >> shift) & 0xff);
            const float b = static_cast<float>((rightColor >> shift) & 0xff);
            result[static_cast<size_t>(index * 4 + channel)] =
                static_cast<unsigned char>(std::lround((a + (b - a) * fraction) * fade));
        }
        result[static_cast<size_t>(index * 4 + 3)] = 255;
    }
    return result;
}

}  // namespace waterfall
}  // namespace cella
