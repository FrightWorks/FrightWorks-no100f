// gx_extra.cpp - GX entry points that have no other home in hle/gx.
#include "gx_internal.h"
#include "runtime_log.h"

#include <dolphin/gx/GXGet.h>

namespace {
void LogOnce(bool& logged, const char* what)
{
    if (!logged) {
        logged = true;
        RT_LOGF(RT_TAG_GX, "%s\n", what);
    }
}
} // namespace

// GXGetCullMode(GXCullMode* mode): Aurora holds the mode GXSetCullMode (gx_pixel.cpp) forwards to it.
extern "C" void GX__GetCullMode_801c5890(uint32_t modePtr) {
    GXCullMode mode = GX_CULL_NONE;
    GXGetCullMode(&mode);
    if (modePtr != 0) {
        Memory::Write32(modePtr, static_cast<uint32_t>(mode));
    }
}

// GXAbortFrame asks the GPU to drop the frame in flight. Aurora submits whole frames and has nothing
// half-finished to abandon, so there is nothing to do.
extern "C" void GX__AbortFrame_801c4ce0() {
    static bool logged = false;
    LogOnce(logged, "GXAbortFrame: no partial GPU frame to abandon (no-op)");
}
PPC_NATIVE_OVERRIDE_VOID(80001134, GX__AbortFrame_801c4ce0, (), ());

// GXPoke*: modes for CPU writes straight into the embedded frame buffer (GXPokeARGB/GXPokeZ). On
// hardware they program pixel-engine registers; the runtime does not emulate CPU-to-EFB access, so
// the modes have no consumer. Accept them (Aurora declares but does not implement these).
#define NO100F_GX_POKE_NOOP(addr, name, params, uses)                                        \
    extern "C" void GX__##name##_##addr params {                                            \
        static bool logged = false;                                                          \
        (void)(uses);                                                                        \
        LogOnce(logged, "GX" #name ": CPU-to-EFB poke modes are not emulated (no-op)");     \
    }

NO100F_GX_POKE_NOOP(8028ce58, PokeAlphaMode, (uint32_t func, uint32_t threshold), (func, threshold))
PPC_NATIVE_OVERRIDE_VOID(800fdf38, GX__PokeAlphaMode_8028ce58, (uint32_t func, uint32_t threshold), (func, threshold));
NO100F_GX_POKE_NOOP(8028ce68, PokeAlphaRead, (uint32_t mode), (mode))
PPC_NATIVE_OVERRIDE_VOID(800FDF4C, GX__PokeAlphaRead_8028ce68, (uint32_t mode), (mode));
NO100F_GX_POKE_NOOP(8028ce80, PokeAlphaUpdate, (uint32_t enable), (enable))
PPC_NATIVE_OVERRIDE_VOID(800FDF60, GX__PokeAlphaUpdate_8028ce80, (uint32_t enable), (enable));
NO100F_GX_POKE_NOOP(8028ce94, PokeBlendMode, (uint32_t type, uint32_t src, uint32_t dst, uint32_t op), (type, src, dst, op))
PPC_NATIVE_OVERRIDE_VOID(800FDF7C, GX__PokeBlendMode_8028ce94, (uint32_t type, uint32_t src, uint32_t dst, uint32_t op),
                         (type, src, dst, op));
NO100F_GX_POKE_NOOP(8028cef0, PokeColorUpdate, (uint32_t enable), (enable))
PPC_NATIVE_OVERRIDE_VOID(800FE004, GX__PokeColorUpdate_8028cef0, (uint32_t enable), (enable));
NO100F_GX_POKE_NOOP(8028cf04, PokeDstAlpha, (uint32_t enable, uint32_t alpha), (enable, alpha))
PPC_NATIVE_OVERRIDE_VOID(800FE020, GX__PokeDstAlpha_8028cf04, (uint32_t enable, uint32_t alpha), (enable, alpha));
NO100F_GX_POKE_NOOP(8028cf1c, PokeDither, (uint32_t dither), (dither))
PPC_NATIVE_OVERRIDE_VOID(800FE034, GX__PokeDither_8028cf1c, (uint32_t dither), (dither));
NO100F_GX_POKE_NOOP(8028cf30, PokeZMode, (uint32_t compareEnable, uint32_t func, uint32_t updateEnable),
                   (compareEnable, func, updateEnable))
PPC_NATIVE_OVERRIDE_VOID(800FE050, GX__PokeZMode_8028cf30, (uint32_t compareEnable, uint32_t func, uint32_t updateEnable),
                         (compareEnable, func, updateEnable));

#undef NO100F_GX_POKE_NOOP
