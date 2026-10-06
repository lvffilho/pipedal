#pragma once
#include <algorithm>

namespace pipedal
{
    struct ZeroInputMixLevels
    {
        float pluginLevel;
        float inputLevel;
    };
    // triangular mix: 0 => (0,1); 0.5 => (1,1); 1 => (1,0)
    inline ZeroInputMixLevels ComputeZeroInputMixLevels(float mix)
    {
        return {std::min(1.0f, mix * 2), std::min(1.0f, (1 - mix) * 2)};
    }
}
