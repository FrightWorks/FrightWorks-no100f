#pragma once

#include <cstdint>

namespace no100f_model_culling {

constexpr bool IsWideEntityGate(float aspect, uint32_t camera, uint32_t mainCamera,
                                uint32_t returnAddress, uint32_t modelCullReturn) {
    return aspect > 4.0f / 3.0f && camera == mainCamera &&
           returnAddress == 0x8000E380u && modelCullReturn == 0x80022D20u;
}

template <typename NextModel, typename ModelVisible>
bool AnyModelVisible(uint32_t model, NextModel next, ModelVisible visible) {
    for (; model; model = next(model))
        if (visible(model)) return true;
    return false;
}

} // namespace no100f_model_culling
