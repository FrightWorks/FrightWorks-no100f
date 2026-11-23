// NO100F 60 fps ([video] fps60): particles keep the fade, motion and animation they have at the game's 30 Hz.
//
// With fps60 the main loop runs twice per 30 Hz frame with half the frame time. Three things went wrong:
// - Fade. The per-group update (0x80032918) adds rate * dt to each colour channel, cut to a whole number each frame. A
//   rate of -51 a second loses 1 per frame at 30 fps but rounds to 0 at 60, so the particle never fades; -102 loses 3
//   at 30 fps (-90 a second) but 1 at 60 (-60 a second). Here, with fps60, colour changes once per 30 Hz step using the
//   step's summed time, which cuts to the same whole numbers as 30 fps. Size and life still change every frame.
// - Velocity. MOVE (PARS command 3, 0x800C38F0) adds the velocity to the position every frame with no dt, and emit
//   scales a velocity by the frame time, so velocity means distance per frame, half as large at 60 frames a second. A
//   command that changes velocity by an amount per second then moves particles twice as far. Here it gets half the time.
// - Per-call steps. Some commands do a fixed thing per call or cut to a whole number, so no share of dt makes them
//   right. Here they run once per 30 Hz step, as they do at 30 fps.
//
// All wrappers (REGISTER_NATIVE_FUNCTION_AS): the game's group update and command functions stay translated as
// func_<addr> and always do the work; without fps60 each wrapper is a plain call.
// - Group update: the game's function computes a channel's change as rate * dt, so the wrapper hands it rates scaled
//   by step / dt for the particles it will fade (life not below 0): 0 between steps, 2 at a step with a steady frame
//   time, where (2 * rate) * dt is exactly rate * step. The rates are put back right after. (With an uneven frame time
//   the scaled product can be one rounding step off, which changes a channel only when it sits on a whole number.)
// - Commands: the system update (0x800C68E8) and its parent runner (0x800C65C4) stay the game's code. They look a
//   command's function up in the PARS command table (0x800C48BC, its only callers) and call it with the frame time in
//   f1. The eight command types CommandTimeFps60 (header) treats differently are wrapped at their functions, which
//   nothing else calls or references: each gets its time from the table, or is skipped until a 30 Hz step.
#include <cstdint>
#include <vector>
#include "abi_bridge.h"
#include "hle/no100f_particles_update.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_config.h"

extern "C" void func_80032918(CpuContext* ctx);  // the game's particle group update, kept translated
// The game's PARS command functions with fps60 timing (types in brackets), kept translated.
extern "C" void func_800C4338(CpuContext* ctx);  // [2] acceleration
extern "C" void func_800C3274(CpuContext* ctx);  // [4] push from a point
extern "C" void func_800C4734(CpuContext* ctx);  // [6] speed kill
extern "C" void func_800C2ED8(CpuContext* ctx);  // [13] texture animation
extern "C" void func_800C3B94(CpuContext* ctx);  // [15] random turn
extern "C" void func_800C3AEC(CpuContext* ctx);  // [20] wind
extern "C" void func_800C3938(CpuContext* ctx);  // [21] spin
extern "C" void func_800C3650(CpuContext* ctx);  // [27] pull to the player

namespace {
constexpr uint32_t kUpdateFrameCount = 0x80270890u;  // [r13 - 0x79F0]: main loop frame counter
constexpr uint32_t kGroupHead = 0x00;
constexpr uint32_t kParticleNext = 0x00, kParticleLife = 0x08, kParticleColorRate = 0x40;  // 4 floats, one a channel

bool ParticlesFps60() {
    static const bool fps60 = RuntimeConfigFile::Fps60();
    return fps60;
}

// With fps60, the time of this frame's 30 Hz step (see StepClock). The group update and the commands share it.
float StepTimeFps60(float dt) {
    static No100fParticlesUpdate::StepClock clock;
    return clock.Step(Memory::Read32(kUpdateFrameCount), dt);
}

struct SavedRates {
    uint32_t particle;
    uint32_t rates[4];
};

void RunCommandFps60(CpuContext* ctx, uint32_t type, void (*command)(CpuContext*)) {
    if (!ParticlesFps60()) {
        command(ctx);
        return;
    }
    const float frameTime = static_cast<float>(ctx->fpr[1].d);
    const auto when = No100fParticlesUpdate::CommandTimeFps60(type, frameTime, StepTimeFps60(frameTime));
    if (!when.run) return;
    ctx->fpr[1].d = when.time;
    command(ctx);
}
}  // namespace

extern "C" void No100f_UpdateParticleGroup(CpuContext* ctx) {
    const float dt = static_cast<float>(ctx->fpr[1].d);
    if (!ParticlesFps60() || !(dt > 0.0f)) {
        func_80032918(ctx);
        return;
    }
    const float scale = StepTimeFps60(dt) / dt;
    if (scale == 1.0f) {
        func_80032918(ctx);
        return;
    }
    static std::vector<SavedRates> saved;  // a stack, in case of re-entry
    const size_t first = saved.size();
    for (uint32_t particle = Memory::Read32(ctx->gpr[3] + kGroupHead); particle != 0;
         particle = Memory::Read32(particle + kParticleNext)) {
        if (Memory::ReadFloat32(particle + kParticleLife) < 0.0f) continue;  // removed or looped: not faded this time
        SavedRates entry{particle, {}};
        for (uint32_t i = 0; i < 4; ++i) {
            const uint32_t rate = particle + kParticleColorRate + i * 4;
            entry.rates[i] = Memory::Read32(rate);
            Memory::WriteFloat32(rate, Memory::ReadFloat32(rate) * scale);
        }
        saved.push_back(entry);
    }
    func_80032918(ctx);
    for (size_t n = first; n < saved.size(); ++n)
        for (uint32_t i = 0; i < 4; ++i)
            Memory::Write32(saved[n].particle + kParticleColorRate + i * 4, saved[n].rates[i]);
    saved.resize(first);
}
REGISTER_NATIVE_FUNCTION_AS(0x80032918, No100f_UpdateParticleGroup, "No100f_UpdateParticleGroup");

extern "C" void No100f_ParsAccel(CpuContext* ctx) { RunCommandFps60(ctx, 2, func_800C4338); }
extern "C" void No100f_ParsPush(CpuContext* ctx) { RunCommandFps60(ctx, 4, func_800C3274); }
extern "C" void No100f_ParsSpeedKill(CpuContext* ctx) { RunCommandFps60(ctx, 6, func_800C4734); }
extern "C" void No100f_ParsTextureAnim(CpuContext* ctx) { RunCommandFps60(ctx, 13, func_800C2ED8); }
extern "C" void No100f_ParsRandomTurn(CpuContext* ctx) { RunCommandFps60(ctx, 15, func_800C3B94); }
extern "C" void No100f_ParsWind(CpuContext* ctx) { RunCommandFps60(ctx, 20, func_800C3AEC); }
extern "C" void No100f_ParsSpin(CpuContext* ctx) { RunCommandFps60(ctx, 21, func_800C3938); }
extern "C" void No100f_ParsPull(CpuContext* ctx) { RunCommandFps60(ctx, 27, func_800C3650); }
REGISTER_NATIVE_FUNCTION_AS(0x800C4338, No100f_ParsAccel, "No100f_ParsAccel");
REGISTER_NATIVE_FUNCTION_AS(0x800C3274, No100f_ParsPush, "No100f_ParsPush");
REGISTER_NATIVE_FUNCTION_AS(0x800C4734, No100f_ParsSpeedKill, "No100f_ParsSpeedKill");
REGISTER_NATIVE_FUNCTION_AS(0x800C2ED8, No100f_ParsTextureAnim, "No100f_ParsTextureAnim");
REGISTER_NATIVE_FUNCTION_AS(0x800C3B94, No100f_ParsRandomTurn, "No100f_ParsRandomTurn");
REGISTER_NATIVE_FUNCTION_AS(0x800C3AEC, No100f_ParsWind, "No100f_ParsWind");
REGISTER_NATIVE_FUNCTION_AS(0x800C3938, No100f_ParsSpin, "No100f_ParsSpin");
REGISTER_NATIVE_FUNCTION_AS(0x800C3650, No100f_ParsPull, "No100f_ParsPull");
