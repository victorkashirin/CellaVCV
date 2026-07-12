#pragma once

#include <limits>

struct LoudnessCVMessage {
    float momentaryLufs = -std::numeric_limits<float>::infinity();
    float shortTermLufs = -std::numeric_limits<float>::infinity();
    float targetLufs = -23.f;
};
