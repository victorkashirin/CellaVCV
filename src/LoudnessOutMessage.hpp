#pragma once

#include <limits>

struct LoudnessOutMessage {
    float momentaryLufs = -std::numeric_limits<float>::infinity();
    float shortTermLufs = -std::numeric_limits<float>::infinity();
    float targetLufs = -23.f;
};
