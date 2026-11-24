// NO100F: shafts of light don't hide what's behind them.
//
// Levels draw shafts of light as see-through models (textures gc_lightbeam_*, gc_light_beam_green, light_beam_*,
// fx_light_shaft_ooze; about 30 levels). The scene draws see-through objects farthest first, judged by the horizontal
// distance to each object, and they write depth. When a shaft sorts before a see-through object that is really behind it
// (s005's acid seen through a LIGHTRAY from the PS2 cutscene cin_9's cameras), the shaft's depth hides that object
// where they overlap: a hard-edged hole. A shaft of light never needs to hide anything, so its atomics draw with depth
// writes off (the depth test stays on, so walls in front still cover it), in normal play and cutscenes alike.
//
// Wrapper (REGISTER_NATIVE_FUNCTION_AS; the game's function stays translated as func_8000E39C):
//   0x8000E39C iModelRender (atomic, matrices): for an atomic with a light-beam texture, RwRenderStateSet
//              (0x80160B10) ZWRITEENABLE off around the draw, then back to what RwRenderStateGet (0x80160B70) said.
// Written from the data (texture names) and the render state calls; no game code.

#include "abi_bridge.h"
#include "hle_stubs.h"
#include "memory.h"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <initializer_list>

extern "C" void func_8000E39C(CpuContext* ctx);  // model render (atomic, matrices)
extern "C" void func_80160B10(CpuContext* ctx);  // RenderWare render state set (state, value)
extern "C" void func_80160B70(CpuContext* ctx);  // RenderWare render state get (state, value out)

namespace {

constexpr uint32_t kRenderStateZWrite = 8;  // rwRENDERSTATEZWRITEENABLE

// A guest call with these arguments; every register is put back afterwards.
uint32_t Call(CpuContext* ctx, void (*fn)(CpuContext*), std::initializer_list<uint32_t> gprs) {
    uint32_t gpr[32];
    PPC_FPR fpr[32];
    std::memcpy(gpr, ctx->gpr, sizeof gpr);
    std::memcpy(fpr, ctx->fpr, sizeof fpr);
    const uint32_t cr = ctx->cr, lr = ctx->lr, ctr = ctx->ctr, xer = ctx->xer;
    int r = 3;
    for (uint32_t v : gprs) ctx->gpr[r++] = v;
    fn(ctx);
    const uint32_t result = ctx->gpr[3];
    std::memcpy(ctx->gpr, gpr, sizeof gpr);
    std::memcpy(ctx->fpr, fpr, sizeof fpr);
    ctx->cr = cr;
    ctx->lr = lr;
    ctx->ctr = ctr;
    ctx->xer = xer;
    return result;
}

bool LightBeamName(const char* name) {
    char lower[33] = {};
    for (int c = 0; c < 32 && name[c]; ++c) lower[c] = char(std::tolower(static_cast<unsigned char>(name[c])));
    return std::strstr(lower, "lightbeam") || std::strstr(lower, "light_beam") || std::strstr(lower, "light_shaft");
}

// Any of the atomic's materials (geometry +0x20 materials, +0x24 count; material +0 texture; texture +0x10 name).
bool LightBeam(uint32_t atomic) {
    if (!Memory::Contains(atomic + 0x18, 4)) return false;
    const uint32_t geometry = Memory::Read32(atomic + 0x18);
    if (!Memory::Contains(geometry, 0x28)) return false;
    const uint32_t materials = Memory::Read32(geometry + 0x20), count = Memory::Read32(geometry + 0x24);
    for (uint32_t i = 0; i < count && i < 64 && Memory::Contains(materials + 4 * i, 4); ++i) {
        const uint32_t material = Memory::Read32(materials + 4 * i);
        if (!Memory::Contains(material, 4)) continue;
        const uint32_t texture = Memory::Read32(material);
        if (!texture || !Memory::Contains(texture + 0x10, 32)) continue;
        if (LightBeamName(reinterpret_cast<const char*>(Memory::GetPointer(texture + 0x10, 32)))) return true;
    }
    return false;
}

}  // namespace

extern "C" void No100f_LightBeamModelRender(CpuContext* ctx) {
    const uint32_t atomic = ctx->gpr[3];
    if (!LightBeam(atomic)) {
        func_8000E39C(ctx);
        return;
    }
    // RwRenderStateGet writes the value through a pointer: a word in a frame below the stack pointer.
    const uint32_t sp = ctx->gpr[1];
    ctx->gpr[1] = sp - 0x20;
    const uint32_t out = sp - 0x18;
    Memory::Write32(out, 1);
    Call(ctx, func_80160B70, {kRenderStateZWrite, out});
    const uint32_t was = Memory::Read32(out);
    ctx->gpr[1] = sp;
    Call(ctx, func_80160B10, {kRenderStateZWrite, 0});
    func_8000E39C(ctx);
    const uint32_t result = ctx->gpr[3];
    Call(ctx, func_80160B10, {kRenderStateZWrite, was});
    ctx->gpr[3] = result;
}

REGISTER_NATIVE_FUNCTION_AS(0x8000E39C, No100f_LightBeamModelRender, "No100f_LightBeamModelRender");
