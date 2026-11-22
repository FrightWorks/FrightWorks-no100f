// Stream voices' DVD read callback, with the stale end-of-track check fixed: see hle/no100f_stream_read.h.
//
// A wrapper (REGISTER_NATIVE_FUNCTION_AS) on the callback fn_800DA71C(result in r3, DVDFileInfo* in r4), reached through
// the DVD read's callback pointer; the game's callback stays translated as func_800DA71C and always runs.
#include "abi_bridge.h"
#include "hle/no100f_stream_read.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"

extern "C" void func_800DA71C(CpuContext* ctx);  // the game's stream DVD read callback, kept translated

namespace {
constexpr uint32_t kFileInfoEntry = 0x2C;  // DVDFileInfo user data: the stream table entry
}  // namespace

extern "C" void No100f_StreamChunkRead(CpuContext* ctx) {
    const uint32_t entry = Memory::Read32(ctx->gpr[4] + kFileInfoEntry);
    No100fStreamRead::ClearPrevReadOnFirstChunk(entry,
        [](uint32_t address) { return Memory::Read32(address); },
        [](uint32_t address, uint32_t value) { Memory::Write32(address, value); });
    func_800DA71C(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x800DA71C, No100f_StreamChunkRead, "No100f_StreamChunkRead");
