// NO100F true widescreen: RwCameraSetViewWindow (0x80168FC0) sizes every camera's view (gameplay cameras are 4:3,
// y = 0.75 * x. In a widened picture mode the view window is widened to the picture's aspect, so the game culls and
// projects for the wide view itself (Aurora's projection widening only stretches what the game already decided to
// draw, which left cutscene and demo sides empty).
//
// A wrapper: REGISTER_NATIVE_FUNCTION_AS takes the address over and keeps the game's function translated as
// func_80168FC0, which stores the view window (r4 points at x, y) in the camera with its reciprocals and updates the
// camera's frame. When widening changes the width, the game's function gets a widened copy of the view window instead
// of the caller's (left as it was: it may even be the camera's own +0x68). The copy sits in a 16-byte frame of our own
// on the guest stack, with the back chain set as a real frame has; the game's function reads it once, before anything
// else could reuse that memory. In 4:3 (camera aspect 0), and for square or taller views (the shadow pass), the width
// doesn't change and this is a plain call of the game's function.
#include <atomic>
#include <cstdint>
#include <cstring>
#include "abi_bridge.h"
#include "memory.h"
#include "hle_stubs.h"
#include "hle/no100f_camera.h"
#include "ppc_runtime.h"

extern std::atomic<float> g_gameCameraAspect;  // display_settings.cpp
extern "C" void func_80168FC0(CpuContext* ctx);  // the game's RwCameraSetViewWindow, kept translated

namespace {
float ReadF32(uint32_t a) { const uint32_t w = Memory::Read32(a); float f; std::memcpy(&f, &w, 4); return f; }
uint32_t Bits(float f) { uint32_t w; std::memcpy(&w, &f, 4); return w; }
}

extern "C" void No100f_CameraSetViewWindow(CpuContext* ctx) {
    const uint32_t viewWindow = ctx->gpr[4];
    const float x = ReadF32(viewWindow);
    const float wide = No100fCamera::WidenedViewWindowWidth(x, ReadF32(viewWindow + 4),
                                                            g_gameCameraAspect.load(std::memory_order_relaxed));
    if (Bits(wide) == Memory::Read32(viewWindow)) {
        func_80168FC0(ctx);
        return;
    }
    const uint32_t sp = ctx->gpr[1];
    const uint32_t frame = sp - 16;
    Memory::Write32(frame, sp);                                   // back chain
    Memory::Write32(frame + 8, Bits(wide));                       // x, widened
    Memory::Write32(frame + 12, Memory::Read32(viewWindow + 4));  // y as it was
    ctx->gpr[1] = frame;
    ctx->gpr[4] = frame + 8;
    func_80168FC0(ctx);
    ctx->gpr[1] = sp;
}
REGISTER_NATIVE_FUNCTION_AS(0x80168FC0, No100f_CameraSetViewWindow, "No100f_CameraSetViewWindow");
