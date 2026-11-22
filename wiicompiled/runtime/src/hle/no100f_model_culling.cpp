// xEntRender's early iModelCull tests only the root atomic. On multi-atomic
// characters that can be a hat, not the body. Keep that optimization only when
// every visible model is outside; leave the later per-atomic tests unchanged.
//
// A wrapper on RwCameraFrustumTestSphere (0x80168D74): REGISTER_NATIVE_FUNCTION_AS
// takes the address over and keeps the game's function translated as
// func_80168D74, which always runs first and gives the result. Only an
// "outside" from the entity-level gate in a widened picture (main camera, called
// from iModelCull for xEntRender) gets the extra test of the entity's other
// models; every other call returns the game's result as it is.
#include "no100f_model_culling.h"
#include "abi_bridge.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"

#include <atomic>
#include <cstdint>
#include <cstring>

extern std::atomic<float> g_gameCameraAspect;
extern "C" void func_8000E220(CpuContext* ctx);  // iModelCull
extern "C" void func_80168D74(CpuContext* ctx);  // the game's RwCameraFrustumTestSphere, kept translated

namespace {
float ReadFloat(uint32_t address) {
    const uint32_t word = Memory::Read32(address);
    float value;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

void WriteFloat(uint32_t address, float value) {
    uint32_t word;
    std::memcpy(&word, &value, sizeof(word));
    Memory::Write32(address, word);
}

bool ModelVisible(uint32_t model, CpuContext& probe, uint32_t scratchMatrix) {
    if (!(Memory::Read16(model + 0x18) & 1)) return false;
    uint32_t matrix = Memory::Read32(model + 0x20);
    if (ReadFloat(model + 0x24) != 0.0f) {
        // Match xModelRenderSingle's optional instance scale without changing a
        // shared matrix, including when a child is attached to a different bone.
        for (uint32_t offset = 0; offset < 0x40; offset += 4)
            Memory::Write32(scratchMatrix + offset, Memory::Read32(matrix + offset));
        for (uint32_t row = 0; row < 3; ++row)
            for (uint32_t axis = 0; axis < 3; ++axis)
                WriteFloat(scratchMatrix + row * 0x10 + axis * 4,
                    static_cast<float>(PpcFmulsInline(ReadFloat(matrix + row * 0x10 + axis * 4),
                                                     ReadFloat(model + 0x24 + row * 4))));
        matrix = scratchMatrix;
    }
    probe.gpr[3] = Memory::Read32(model + 0x10);
    probe.gpr[4] = matrix;
    probe.lr = 0; // A nested atomic test must not re-enter the entity-level gate.
    func_8000E220(&probe);
    return probe.gpr[3] == 0;
}
} // namespace

extern "C" void No100f_CameraFrustumTestSphere(CpuContext* ctx) {
    const uint32_t camera = ctx->gpr[3];
    const uint32_t returnAddress = ctx->lr;
    func_80168D74(ctx);  // a leaf: r1 and the saved registers read below are as the caller left them
    uint32_t result = ctx->gpr[3];
    const float aspect = g_gameCameraAspect.load(std::memory_order_relaxed);
    // iModelCull's fixed 0x30-byte frame saves its caller at SP+0x34.
    if (result == 0 && returnAddress == 0x8000E380u &&
        no100f_model_culling::IsWideEntityGate(aspect, camera, Memory::Read32(0x80245190u),
                                             returnAddress, Memory::Read32(ctx->gpr[1] + 0x34))) {
        const uint32_t root = Memory::Read32(ctx->gpr[27] + 0x2C);
        if (Memory::Read32(root + 0x10) == ctx->gpr[29] &&
            Memory::Read32(root + 0x20) == ctx->gpr[30]) {
            const uint32_t first = ReadFloat(root + 0x24) == 0.0f ? Memory::Read32(root) : root;
            if (!first) {
                ctx->gpr[3] = result;
                return;
            }
            CpuContext probe = *ctx;
            probe.gpr[1] -= 0x200;
            const uint32_t scratchMatrix = probe.gpr[1] + 0x40;
            CpuContextScope scope(&probe);
            if (no100f_model_culling::AnyModelVisible(first,
                    [](uint32_t model) { return Memory::Read32(model); },
                    [&](uint32_t model) { return ModelVisible(model, probe, scratchMatrix); }))
                result = 1;
        }
    }
    ctx->gpr[3] = result;
}
REGISTER_NATIVE_FUNCTION_AS(0x80168D74, No100f_CameraFrustumTestSphere, "No100f_CameraFrustumTestSphere");
