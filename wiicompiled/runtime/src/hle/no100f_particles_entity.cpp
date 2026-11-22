// NO100F 60 fps ([video] fps60): particles that entity code emits keep the game's 30 Hz emission rate and speed.
//
// Entity code (about 90 call sites) emits through "emit with overrides" (0x800C5C0C): it applies the overrides the
// caller passes (count, interval, position, velocity, ...) to the emitter asset, calls the emit function (0x800C4B04)
// with the caller's time and, if asked, restores the asset. With fps60 the main loop runs twice per 30 Hz frame, which
// broke this two ways:
// - Emission while the emitter timer is 0 (interval 0, or the "emit now" override) happens on every call, so
//   continuous effects (candles, van exhaust, firework trails) made twice the particles.
// - 36 call sites pass a fixed 1/30 s instead of the frame time. Emit sets a particle's velocity to the asset
//   velocity times that time and the particle moves by its velocity every frame, so at 60 fps those particles moved
//   twice as fast (and interval timers ran twice as fast).
// Here, with fps60, a fixed 1/30 s becomes the frame time, and an emitter that emits on every call emits at most once
// per 30 Hz step: not in the frame right after it emitted, unless that frame was a full 30 Hz step (slowdown). Calls in
// the same frame all emit, as at 30 fps. A skipped emit returns 0 (no particles), and the game still applies and
// restores its overrides around it.
//
// Confetti fireworks ([game] holiday = "confetti"): firework bursts come through here too (fn_800ADAC0), so this is also
// where each new burst particle gets its own colour. See ColorBurst.
//
// A wrapper on emit: REGISTER_NATIVE_FUNCTION_AS takes 0x800C4B04 over and keeps the game's emit translated as
// func_800C4B04. Emit has two callers, the per-frame emitter update (0x800C4AAC, see no100f_particles.cpp) and "emit
// with overrides"; this acts only on calls from the second, told apart by the return address, and passes every other
// call straight on. The emitter state it reads is the state after the overrides were applied. With fps60 and
// confetti off it is a plain call of the game's emit.
#include <cstdint>
#include <unordered_map>
#include "abi_bridge.h"
#include "memory.h"
#include "hle/no100f_confetti.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "runtime_config.h"

extern "C" void func_800C4B04(CpuContext* ctx);  // the game's emit(emitter, dt), kept translated, returns r3

namespace {
constexpr uint32_t kEmitWithOverridesReturn = 0x800C5EBCu;  // after "emit with overrides" calls emit
constexpr uint32_t kFrameTime = 0x80270D84u;   // [r13 - 0x74FC]: measured frame time, clamped to 1/60..0.1 s
constexpr uint32_t kFrameCount = 0x80270890u;  // [r13 - 0x79F0]: main loop frame counter

// With fps60: false if this every-call emission would be the second in one 30 Hz step.
bool EmitThisFrame(uint32_t emitter) {
    static std::unordered_map<uint32_t, uint32_t> lastFrame;
    const uint32_t frame = Memory::Read32(kFrameCount);
    auto [it, fresh] = lastFrame.try_emplace(emitter, frame);
    if (fresh || it->second == frame) return true;
    if (frame - it->second == 1 && Memory::ReadFloat32(kFrameTime) < 0.75f / 30.0f) return false;
    it->second = frame;
    return true;
}

// The particle system of a firework burst emitter (PAREMIT_FIREWORKS1 and 2, asset IDs from boot.HIP) when confetti
// fireworks are on, else 0. The game uses these two emitters for nothing else.
uint32_t ConfettiBurstSystem(uint32_t emitter) {
    static const bool confetti = RuntimeConfigFile::Holiday() == "confetti";
    if (!confetti) return 0;
    const uint32_t id = Memory::Read32(Memory::Read32(emitter + 0x10));
    if (id != 0xC85CDF1Cu && id != 0xC85CDF1Du) return 0;
    const uint32_t group = Memory::Read32(emitter + 0x48);
    return group != 0 ? Memory::Read32(group + 0x20) : 0;
}

// Colours the particles emit just added to a system, each its own colour. New particles go on the head of its list
// (fn_80032728). Emit aims each one at random, so going through the colours in list order mixes them through the burst
// like confetti, and takes no random numbers from the game. Only red, green and blue change: alpha keeps fading as the
// game set it, and the game gives a burst the same start and end colour, so the fade rates for these three (+0x40) are
// 0 and the colour stays for the particle's whole life.
void ColorBurst(uint32_t system, uint32_t added) {
    uint32_t particle = Memory::Read32(system);
    for (uint32_t i = 0; i < added && particle != 0; ++i, particle = Memory::Read32(particle)) {
        const No100fConfetti::Rgb color = No100fConfetti::Color(i);
        Memory::Write8(particle + 0xC, color.r);
        Memory::Write8(particle + 0xD, color.g);
        Memory::Write8(particle + 0xE, color.b);
    }
}
}  // namespace

extern "C" void No100f_EmitParticles(CpuContext* ctx) {
    if (ctx->lr != kEmitWithOverridesReturn) {
        func_800C4B04(ctx);
        return;
    }
    const uint32_t emitter = ctx->gpr[3];
    if (static const bool fps60 = RuntimeConfigFile::Fps60(); fps60) {
        if (static_cast<float>(ctx->fpr[1].d) == 1.0f / 30.0f) ctx->fpr[1].d = Memory::ReadFloat32(kFrameTime);
        // Emit only counts the timer (+0x1C) down while it is above 0; at 0 it emits on every call.
        if (!(Memory::ReadFloat32(emitter + 0x1C) > 0.0f) && !EmitThisFrame(emitter)) {
            ctx->gpr[3] = 0;
            return;
        }
    }
    const uint32_t burstSystem = ConfettiBurstSystem(emitter);
    const uint32_t countBefore = burstSystem != 0 ? Memory::Read32(burstSystem + 0x8) : 0;
    func_800C4B04(ctx);
    if (burstSystem != 0) ColorBurst(burstSystem, Memory::Read32(burstSystem + 0x8) - countBefore);
}
REGISTER_NATIVE_FUNCTION_AS(0x800C4B04, No100f_EmitParticles, "No100f_EmitParticles");
