// Night of 100 Frights: the game's code and data sections come from the player's own disc, never from this exe.
//
// The translation was built from the disc's sys/main.dol. This checks the player's copy against the SHA-256 the build
// pinned (scooby_dol_sha256.inc, written by tools/build-no100f.ps1) and copies its sections into guest memory.
// It replaces the translator's embedded data_sections_init.cpp.
#include "memory.h"
#include "no100f_game_state.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <SDL3/SDL_messagebox.h>
#include <cryptopp/sha.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

#include "scooby_dol_sha256.inc"  // kDolSha256: hex SHA-256 of the main.dol the build translated

namespace fs = std::filesystem;

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

std::string Sha256Hex(const std::vector<uint8_t>& data) {
    uint8_t digest[CryptoPP::SHA256::DIGESTSIZE];
    CryptoPP::SHA256().CalculateDigest(digest, data.data(), data.size());
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (uint8_t b : digest) { out += hex[b >> 4]; out += hex[b & 15]; }
    return out;
}

[[noreturn]] void Fail(const std::string& message) {
    RT_LOG(RT_TAG_RUNTIME) << "[disc] " << message << std::endl;
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FrightWorks: Night of 100 Frights", message.c_str(), nullptr);
    std::exit(1);
}

}  // namespace

extern "C" void InitializeDataSections() {
    static bool done = false;
    if (done) return;
    done = true;

    const fs::path disc = RuntimeConfigFile::ResolvedDvdRoot();
    const fs::path path = disc / "sys" / "main.dol";
    std::ifstream f(path, std::ios::binary);
    if (disc.empty() || !f)
        Fail("No game disc is set up. Choose your Scooby-Doo! Night of 100 Frights (USA, GameCube) disc image in the launcher.");
    const std::vector<uint8_t> dol(std::istreambuf_iterator<char>(f), {});
    if (dol.size() < 0x100 || Sha256Hex(dol) != kDolSha256)
        Fail("This disc is not the copy this build expects (Scooby-Doo! Night of 100 Frights, USA, GameCube, GIHE78 "
             "revision 1). Set up a clean dump of that disc.");
    for (int k = 0; k < 18; ++k) {
        const uint32_t off = Be32(&dol[k * 4]), addr = Be32(&dol[0x48 + k * 4]), size = Be32(&dol[0x90 + k * 4]);
        if (size == 0 || size_t(off) + size > dol.size() || !Memory::Contains(addr, size)) continue;
        std::memcpy(Memory::GetPointer(addr, size), &dol[off], size);
    }
    no100f_game_state::g_diagnosticLayoutVerified =
        std::string_view(kDolSha256) == "141d562a97f9d6a8794e164611a5da45f9c50b9d89049687f051b7af12e37434";
    RT_LOG(RT_TAG_RUNTIME) << "[disc] loaded game code and data from " << RuntimeConfigFile::PathToUtf8(path) << std::endl;
}
