// NO100F: RenderWare's 2D projections are one pixel short (seam around the top-left 256x256 of the picture in every
// level; the original in Dolphin has it too).
//
// RenderWare's GameCube driver draws in pixel coordinates in three places: the Im2D setup (0x801727C8: HUD, text,
// fades, anything RwIm2D), the camera clear (0x8017A3EC, used by 0x8017A71C) and 0x80179734. Each sets the viewport
// to the whole framebuffer (fbWidth x xfbHeight pixels) but builds its orthographic projection over 0..fbWidth - 1 x
// 0..xfbHeight - 1 (C_MTXOrtho at 0x80172A84; projection vectors with 2/(fbWidth - 1), -2/(xfbHeight - 1), or the
// precomputed 2/639, -2/527, for GXSetProjectionv at 0x8017A6EC and 0x80179A60). So every 2D vertex lands
// x * 640/639, y * 480/479 from the top-left corner: most 2D art is just 0.4 pixel bigger.
// The shadow pass (0x8003B548) is where it shows. In the middle of the frame it copies the top-left 256x256 of the
// scene to a texture, clears that corner (camera clear), renders the shadow camera into it, inverts it (Im2D quad,
// 0x8003B2C8), copies the shadow out, and draws the saved corner back (Im2D quad (0,0)-(256,256), UV 0-1, nearest;
// 0x8003B47C). Clear, invert and restore all come out 256.4 x 256.5 pixels: the restore covers what the other two
// spill over the box, but the restored scene is enlarged against the scene around it. At 640x480 one row repeats at
// y = 256; at higher resolutions everything inside the box sits up to a screen pixel off along its right and bottom
// edges, and textures crossing them look cut. (Correcting only the restore quad uncovers the strip the clear and
// invert spill.)
// Here those three projections span 0..fbWidth x 0..xfbHeight, so 2D coordinates are pixel coordinates.
//
// Two wrappers (REGISTER_NATIVE_FUNCTION_AS): the SDK's C_MTXOrtho (0x80103544) and GXSetProjectionv (0x801025A8) stay
// translated as func_<addr>, and the wrappers change only the arguments of those three calls, told apart by their
// return address; every other call (C_MTXOrtho from 0x800DB268; GXSetProjectionv from the camera begin update
// 0x80176698 and the Im2D restore of the camera's projection 0x80172E48) goes straight on.
// - C_MTXOrtho(m, t, b, l, r, n, f) takes its bounds in f1..f6: the Im2D setup's bottom (f2) and right (f4) are
//   xfbHeight - 1 and fbWidth - 1, so each gets 1 added.
// - GXSetProjectionv(v) reads a 7-float vector at r3 (type, x scale, x offset, y scale, y offset, z scale, z offset).
//   It gets a copy with the x and y scales made full-pixel, in a 40-byte frame of our own on the guest stack with the
//   back chain set; the caller's vector is left as it was. The offsets (-1, +1) keep 0 at the top-left corner.
#include <cmath>
#include <cstdint>
#include <cstring>
#include "abi_bridge.h"
#include "memory.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"

extern "C" void func_80103544(CpuContext* ctx);  // the SDK's C_MTXOrtho, kept translated
extern "C" void func_801025A8(CpuContext* ctx);  // the SDK's GXSetProjectionv, kept translated

namespace {
constexpr uint32_t kIm2DOrthoReturn = 0x80172A88u;        // after C_MTXOrtho at 0x80172A84 (Im2D setup)
constexpr uint32_t kClearProjectionReturn = 0x8017A6F0u;  // after GXSetProjectionv at 0x8017A6EC (camera clear)
constexpr uint32_t kBlitProjectionReturn = 0x80179A64u;   // after GXSetProjectionv at 0x80179A60 (0x80179734)

// A 2D scale 2/(n - 1) (or -2/(n - 1)) from those projection vectors, as 2/n: n - 1 is a whole pixel count.
float FullPixelScale(float scale) {
    const float span = std::round(2.0f / std::fabs(scale));
    return std::copysign(2.0f / (span + 1.0f), scale);
}

uint32_t ProjectionWordBits(float f) { uint32_t w; std::memcpy(&w, &f, 4); return w; }
}  // namespace

extern "C" void No100f_MtxOrtho(CpuContext* ctx) {
    if (ctx->lr == kIm2DOrthoReturn) {
        ctx->fpr[2].d = static_cast<double>(static_cast<float>(ctx->fpr[2].d) + 1.0f);  // bottom
        ctx->fpr[4].d = static_cast<double>(static_cast<float>(ctx->fpr[4].d) + 1.0f);  // right
    }
    func_80103544(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80103544, No100f_MtxOrtho, "No100f_MtxOrtho");

extern "C" void No100f_SetProjectionv(CpuContext* ctx) {
    if (ctx->lr != kClearProjectionReturn && ctx->lr != kBlitProjectionReturn) {
        func_801025A8(ctx);
        return;
    }
    const uint32_t src = ctx->gpr[3];
    const uint32_t sp = ctx->gpr[1];
    const uint32_t frame = sp - 40;
    Memory::Write32(frame, sp);  // back chain
    for (uint32_t i = 0; i < 7; ++i) Memory::Write32(frame + 8 + i * 4, Memory::Read32(src + i * 4));
    Memory::Write32(frame + 8 + 1 * 4, ProjectionWordBits(FullPixelScale(Memory::ReadFloat32(src + 1 * 4))));  // x scale
    Memory::Write32(frame + 8 + 3 * 4, ProjectionWordBits(FullPixelScale(Memory::ReadFloat32(src + 3 * 4))));  // y scale
    ctx->gpr[1] = frame;
    ctx->gpr[3] = frame + 8;
    func_801025A8(ctx);
    ctx->gpr[1] = sp;
}
REGISTER_NATIVE_FUNCTION_AS(0x801025A8, No100f_SetProjectionv, "No100f_SetProjectionv");
