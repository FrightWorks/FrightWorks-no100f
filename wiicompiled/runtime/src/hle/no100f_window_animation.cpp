// NO100F h001 courtyard window: the scene's surface mapper (No100fWindow::kMapperId) maps nothing on the disc, which
// leaves material 71 without its creeper animation. When the courtyard's assets match (No100fWindow::Inspect), the
// scene gets a rebuilt mapper (EncodeMapper) that maps it to the retained 18-frame animation.
//
// A wrapper on zSurfaceRegisterMapper (0x800D2D28): REGISTER_NATIVE_FUNCTION_AS takes the address over and keeps the
// game's function translated as func_800D2D28, which looks the asset up, stores it in the scene's single mapper slot
// (cleared by the game's zSurfaceExit) and returns it. For the courtyard mapper, the replacement is built first (scene
// heap), the game registers the disc's mapper, and the slot and return value then get the replacement. Every other
// call goes to the game's function unchanged.
#include "abi_bridge.h"
#include "hle/no100f_window_contract.h"
#include "hle_stubs.h"
#include "memory.h"
#include "runtime_log.h"

#include <cstdint>
#include <cstring>

extern "C" void func_8003FF84(CpuContext* ctx); // xSTFindAsset
extern "C" void func_8002FDB0(CpuContext* ctx); // xMemAlloc(heap, size)
extern "C" void func_800D2D28(CpuContext* ctx); // the game's zSurfaceRegisterMapper, kept translated

namespace {

No100fWindow::Asset FindAsset(CpuContext* ctx, uint32_t id) {
    if (ctx->gpr[1] < 0x20 || !Memory::Contains(ctx->gpr[1] - 0x20, 0x20)) {
        RT_LOG(RT_TAG_RUNTIME) << "[window-creeper] invalid guest stack for asset lookup\n";
        return {};
    }
    CpuContext call = *ctx;
    call.gpr[1] -= 0x20;
    const uint32_t size = call.gpr[1] + 8;
    Memory::Write32(size, 0);
    call.gpr[3] = id;
    call.gpr[4] = size;
    func_8003FF84(&call);
    return {call.gpr[3], Memory::Read32(size)};
}

uint32_t SceneWorld() {
    const uint32_t scene = Memory::Read32(0x80245CF4);
    if (!scene || !Memory::Contains(scene, 0x44)) return 0;
    const uint32_t env = Memory::Read32(scene + 0x40);
    if (!env || !Memory::Contains(env, 4)) return 0;
    const uint32_t geometry = Memory::Read32(env);
    if (!geometry || !Memory::Contains(geometry, 4)) return 0;
    return Memory::Read32(geometry);
}

// The rebuilt mapper on the scene heap, or 0 when the courtyard doesn't match (or allocation fails).
uint32_t BuildReplacement(CpuContext* ctx) {
    const auto mapper = FindAsset(ctx, No100fWindow::kMapperId);
    const auto match = No100fWindow::Inspect(No100fWindow::kMapperId, mapper, SceneWorld(),
        [](uint32_t address, std::size_t size) { return Memory::Contains(address, size); },
        [](uint32_t address) { return Memory::Read8(address); },
        [](uint32_t address) { return Memory::Read16(address); },
        [](uint32_t address) { return Memory::Read32(address); },
        [&](uint32_t asset) { return FindAsset(ctx, asset); });
    if (match == No100fWindow::Match::Incompatible)
        RT_LOG(RT_TAG_RUNTIME) << "[window-creeper] incompatible courtyard assets; restoration not applied\n";
    if (match != No100fWindow::Match::Restore) return 0;
    CpuContext call = *ctx;
    call.gpr[3] = Memory::Read32(ctx->gpr[13] - 31024);
    call.gpr[4] = 16;
    func_8002FDB0(&call);
    const uint32_t replacement = call.gpr[3];
    if (!replacement || !Memory::Contains(replacement, 16)) {
        RT_LOG(RT_TAG_RUNTIME) << "[window-creeper] cannot allocate scene mapper; restoration not applied\n";
        return 0;
    }
    const auto encoded = No100fWindow::EncodeMapper();
    std::memcpy(Memory::GetPointer(replacement, encoded.size()), encoded.data(), encoded.size());
    return replacement;
}

} // namespace

extern "C" void No100f_RegisterWindowMapper(CpuContext* ctx) {
    const uint32_t countAddress = ctx->gpr[13] - 29436;
    const uint32_t mapperAddress = ctx->gpr[13] - 32296;  // the single slot (count 0)
    if (ctx->gpr[3] != No100fWindow::kMapperId || Memory::Read32(countAddress) != 0) {
        func_800D2D28(ctx);
        return;
    }
    const uint32_t replacement = BuildReplacement(ctx);
    func_800D2D28(ctx);
    if (!replacement) return;
    if (!ctx->gpr[3]) {  // Inspect found the disc's mapper, so the game's lookup should have too
        RT_LOG(RT_TAG_RUNTIME) << "[window-creeper] the game registered no mapper; restoration not applied\n";
        return;
    }
    Memory::Write32(mapperAddress, replacement);
    ctx->gpr[3] = replacement;
    RT_LOG(RT_TAG_RUNTIME) << "[window-creeper] restored h001 material 71 using the retained 18-frame animation\n";
}
REGISTER_NATIVE_FUNCTION_AS(0x800D2D28, No100f_RegisterWindowMapper, "No100f_RegisterWindowMapper");
