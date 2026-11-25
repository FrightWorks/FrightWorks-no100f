// 3D sound panning: the game pans every positioned sound (monsters, doors, pickups) to the wrong side.
//
// Each frame the game hands the sound engine the camera's matrix as the listener (fn_8003CE50(right, up, at, pos,
// range); both callers pass the camera matrix in the globals, +0xC0..+0xF0). The per-sound update fn_800D928C then
// takes the unit vector from the listener to the sound, dots it with the stored right vector and sets the MIX pan to
// 64 + 64 * dot (0 = left, 127 = right). But the camera's "right" is up x at, the RenderWare convention, which
// points to the screen's left. So a sound on the player's right gets a pan below 64 and plays on the left. The bug
// is in the game's code, not the port's: the AX mix and the output (AI frames are right, left) keep the sides.
//
// A wrapper (REGISTER_NATIVE_FUNCTION_AS) on the listener setter: the game's function stays translated as
// func_8003CE50 and runs first, then the stored right vector is negated so it points to the screen's right. Only the
// pan reads that vector (fn_800D928C, one load), so volume, distance and everything else are unchanged.
#include "abi_bridge.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"

extern "C" void func_8003CE50(CpuContext* ctx);  // the game's listener setter, kept translated

namespace {
// 3 floats: the sound engine's copy of the camera's right vector (its state block 0x80237DF8 + 0x1220; the sound
// records start at +0x20, see music_attenuation.cpp).
constexpr uint32_t kListenerRight = 0x80239018u;
constexpr uint32_t kFloatSign = 0x80000000u;
}  // namespace

extern "C" void No100f_SetSoundListener(CpuContext* ctx) {
    func_8003CE50(ctx);
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t address = kListenerRight + i * 4;
        Memory::Write32(address, Memory::Read32(address) ^ kFloatSign);
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x8003CE50, No100f_SetSoundListener, "No100f_SetSoundListener");
