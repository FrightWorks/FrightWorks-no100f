#include "no100f_model_culling.h"

#include <cassert>

int main() {
    using namespace no100f_model_culling;
    assert(IsWideEntityGate(16.0f / 9.0f, 1, 1, 0x8000E380, 0x80022D20));
    assert(!IsWideEntityGate(0, 1, 1, 0x8000E380, 0x80022D20));
    assert(!IsWideEntityGate(4.0f / 3.0f, 1, 1, 0x8000E380, 0x80022D20));
    assert(!IsWideEntityGate(16.0f / 9.0f, 2, 1, 0x8000E380, 0x80022D20));
    assert(!IsWideEntityGate(16.0f / 9.0f, 1, 1, 0x8000E380, 0x8003075C));
    assert(!IsWideEntityGate(16.0f / 9.0f, 1, 1, 0, 0x80022D20));

    const auto next = [](uint32_t model) { return model < 3 ? model + 1 : 0; };
    assert(AnyModelVisible(1, next, [](uint32_t model) { return model == 2; }));
    assert(AnyModelVisible(1, next, [](uint32_t) { return false; }) == false);
    assert(AnyModelVisible(1, [](uint32_t) { return 0; }, [](uint32_t) { return true; }));
    assert(!AnyModelVisible(0, next, [](uint32_t) { return true; }));
}
