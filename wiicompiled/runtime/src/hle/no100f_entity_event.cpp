// The game's entity event (0x800487FC): one entity passes an event to another (r3 from, r4 the event `from` raised,
// r5 to, r6 the event `to` gets; the rest are parameters). Every link in a level runs through it, and it calls itself
// for the links of the entity that got the event. This wrapper only watches the events that hurt, kill or end a boss,
// for the speedrun timer's boss splits (no100f_speedrun.h BossForEvent) and the log, and then runs the game's own
// function, so the game is unchanged.
//
// A wrapper: REGISTER_NATIVE_FUNCTION_AS takes the address over but keeps the game's function translated as
// func_800487FC, which this always calls.
#include <cstdint>
#include "abi_bridge.h"
#include "hle_stubs.h"
#include "memory.h"
#include "no100f_speedrun.h"
#include "ppc_runtime.h"

extern "C" void func_800487FC(CpuContext* ctx);

namespace {

uint32_t EntityId(uint32_t base) {
    uint32_t id = 0;
    return base != 0 && Memory::TryRead32(base, id) ? id : 0;  // an entity's ID is its first word
}

}  // namespace

extern "C" void No100f_EntityEvent(CpuContext* ctx) {
    const uint32_t fromEvent = ctx->gpr[4], toEvent = ctx->gpr[6];
    if (toEvent == no100f_speedrun::kEventKill || toEvent == no100f_speedrun::kEventDie ||
        toEvent == no100f_speedrun::kEventHurt || fromEvent == no100f_speedrun::kEventDie) {
        no100f_speedrun::EntityEvent(EntityId(ctx->gpr[3]), fromEvent, EntityId(ctx->gpr[5]), toEvent);
    }
    func_800487FC(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x800487FC, No100f_EntityEvent, "No100f_EntityEvent");
