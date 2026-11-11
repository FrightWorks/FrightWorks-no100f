// Dev tools texture pack switcher: the pack list and cycle order, the dev tools gate, the command words, the dev menu's
// texture list (filter, sort, name lookup), and the
// replacement index's folder order and lookup order (aurora's replacement_layers.hpp, which needs no GPU).
#include "no100f_texture_list.h"
#include "palette_slice.hpp"
#include "no100f_texture_packs.h"
#include "replacement_layers.hpp"

#include <cassert>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace no100f_texture_packs;
namespace tr = aurora::gfx::texture_replacement;

namespace {

void Touch(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "x";
}

struct Hooked {
    std::vector<std::string> packs;
    std::vector<std::string> applied;
    int lists = 0;
    Switcher::Hooks Hooks() {
        return {[this] {
                    ++lists;
                    return packs;
                },
                [this](const std::string& name) {
                    applied.push_back(name);
                    return true;
                }};
    }
};

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "no100f_texture_packs_tests";
    fs::remove_all(root);

    // Pack list: folders only, no hidden ones, name order with either case together.
    fs::create_directories(root / "texture_packs" / "Beta");
    fs::create_directories(root / "texture_packs" / "alpha");
    fs::create_directories(root / "texture_packs" / "gamma2");
    fs::create_directories(root / "texture_packs" / "Gamma");
    fs::create_directories(root / "texture_packs" / ".hidden");
    Touch(root / "texture_packs" / "notes.txt");
    const std::vector<std::string> expected{"alpha", "Beta", "Gamma", "gamma2"};
    assert(ListPacks(root) == expected);
    assert(ListPacks(root / "nowhere").empty());

    // Cycle: usual, each pack, usual again; and back.
    const std::vector<std::string> three{"a", "b", "c"};
    assert(NextInCycle(three, "", false) == "a");
    assert(NextInCycle(three, "a", false) == "b");
    assert(NextInCycle(three, "b", false) == "c");
    assert(NextInCycle(three, "c", false) == "");
    assert(NextInCycle(three, "", true) == "c");
    assert(NextInCycle(three, "c", true) == "b");
    assert(NextInCycle(three, "a", true) == "");
    assert(NextInCycle(three, "gone", false) == "a");  // a pack that vanished counts as usual
    assert(NextInCycle({}, "", false).empty() && NextInCycle({}, "", true).empty());
    assert(Label("").find("usual") == 0 && Label("a") == "a");

    // Gate: with the developer tools off nothing is listed or applied.
    {
        Hooked h;
        h.packs = three;
        Switcher off(false, h.Hooks());
        assert(!off.Enabled());
        assert(!off.Cycle(false) && !off.Select("a") && !off.Select(""));
        assert(off.Command("next").find("off") != std::string::npos);
        assert(off.Command("list").find("off") != std::string::npos);
        assert(h.lists == 0 && h.applied.empty() && off.Active().empty());
    }

    // On: cycling, selecting, commands.
    {
        Hooked h;
        h.packs = {"Alpha", "beta", "Gamma"};
        Switcher on(true, h.Hooks());
        assert(on.Enabled() && on.Active().empty());
        assert(on.Command("").find("usual") != std::string::npos);
        assert(on.Cycle(false) && on.Active() == "Alpha");
        assert(on.Cycle(false) && on.Active() == "beta");
        assert(on.Command("next") == "texpack: Gamma" && on.Active() == "Gamma");
        assert(on.Command("next") == "texpack: usual (no pack)" && on.Active().empty());
        assert(on.Command("prev") == "texpack: Gamma");
        assert(on.Command(" beta \r") == "texpack: beta");  // trimmed
        assert(on.Command("GAMMA") == "texpack: Gamma");    // letters of either case
        assert(on.Command("nope").find("no pack named") != std::string::npos && on.Active() == "Gamma");
        assert(!on.Select("nope") && on.Active() == "Gamma");
        assert(on.Command("list") == "texpack: usual (no pack), Alpha, beta, Gamma | active Gamma");
        const size_t before = h.applied.size();
        assert(on.Command("reload") == "texpack: Gamma" && h.applied.size() == before + 1 && h.applied.back() == "Gamma");
        assert(on.Command("usual") == "texpack: usual (no pack)" && h.applied.back().empty());
        assert(on.Command("none").find("usual") != std::string::npos);
    }
    {
        // Two packs that differ only in case: the exact name picks, a wrong-case one is ambiguous.
        Hooked h;
        h.packs = {"Style", "style"};
        Switcher on(true, h.Hooks());
        assert(on.Command("style") == "texpack: style");
        assert(on.Command("STYLE").find("no pack named") != std::string::npos);
    }
    {
        // aurora refusing the name leaves the active pack alone.
        Switcher refusing(true, {[] { return std::vector<std::string>{"a"}; }, [](const std::string&) { return false; }});
        assert(!refusing.Cycle(false) && refusing.Active().empty());
    }

    // Pack names reach aurora as plain folder names only.
    assert(tr::valid_pack_name("styleA") && tr::valid_pack_name("my pack 2"));
    for (const char* bad : {"", ".", "..", "a/b", "a\\b", "..\\x", "C:", "a:b"}) assert(!tr::valid_pack_name(bad));
    assert(tr::pack_root(root, "") .empty() && tr::pack_root(root, "..").empty());
    assert(tr::pack_root(root, "styleA") == root / "texture_packs" / "styleA");

    // The usual folder order: the user's, mods (later name first, "disabled" and no textures folder skipped), the game's.
    {
        const fs::path user = root / "user", game = root / "game";
        fs::create_directories(user / "texture_replacements");
        fs::create_directories(game / "texture_replacements");
        fs::create_directories(user / "mods" / "aaa" / "textures");
        fs::create_directories(user / "mods" / "zzz" / "textures");
        fs::create_directories(user / "mods" / "mmm" / "textures");
        Touch(user / "mods" / "mmm" / "disabled");
        fs::create_directories(user / "mods" / "notextures");
        const auto roots = tr::usual_roots(user, game);
        const std::vector<fs::path> want{user / "texture_replacements", user / "mods" / "zzz" / "textures",
                                         user / "mods" / "aaa" / "textures", game / "texture_replacements"};
        assert(roots == want);
        assert(tr::usual_roots(user, {}).size() == 3);
    }

    // Lookup: candidates in priority order; with a pack selected only its index counts, never the usual one.
    {
        const std::map<int, std::string> pack{{1, "pack exact"}, {3, "pack wildcard"}};
        const std::map<int, std::string> usual{{1, "usual exact"}, {2, "usual tlut wildcard"}, {3, "usual wildcard"}, {4, "usual only"}};
        const auto look = [&](bool packSelected, std::vector<int> candidates) -> std::string {
            const auto* found = tr::find_active(pack, usual, packSelected, std::span<const int>(candidates));
            return found ? *found : "none";
        };
        assert(look(false, {1, 2, 3}) == "usual exact");
        assert(look(false, {5, 2, 3}) == "usual tlut wildcard");
        assert(look(false, {5, 6, 3}) == "usual wildcard");
        assert(look(true, {1, 2, 3}) == "pack exact");
        assert(look(true, {5, 2, 3}) == "pack wildcard");  // the usual chain's tlut wildcard (2) is not consulted
        assert(look(true, {4}) == "none");                  // only in the usual chain: not in the pack, so the original shows
        assert(look(true, {5, 6}) == "none");
        assert(look(false, {4}) == "usual only");
    }

    // The level's texture list: filter, sort, `texfind` name lookup.
    {
        using no100f_texture_list::Row;
        using no100f_texture_list::Sort;
        const std::vector<Row> rows{
            {"tex1_256x256_aaaa_14", 256, 256, "CMPR", 1, true},
            {"tex1_64x64_bbbb_0", 64, 64, "I4", 0, false},
            {"tex1_512x128_cccc_dddd_9", 512, 128, "C8", 0, true},
            {"tex1_64x64_bbbc_14", 64, 64, "CMPR", 1, false},
        };
        const auto names = [&](const std::string& filter, Sort sort, bool onlyMissing) {
            std::string out;
            for (const Row* row : no100f_texture_list::Shown(rows, filter, sort, onlyMissing)) out += row->name.substr(5, 3) + ",";
            return out;
        };
        assert(names("", Sort::FirstDrawn, false) == "256,64x,512,64x,");
        assert(names("", Sort::Name, false) == "256,512,64x,64x,");
        assert(names("", Sort::LargestFirst, false) == "256,512,64x,64x,");  // 65536, 65536 tie keeps first-drawn order
        assert(names("", Sort::Format, false) == "512,256,64x,64x,");  // C8, CMPR, CMPR, I4
        assert(names("cmpr", Sort::FirstDrawn, false) == "256,64x,");
        assert(names("64x64 CMPR", Sort::FirstDrawn, false) == "64x,");  // every word must match
        assert(names("BBBB", Sort::FirstDrawn, false) == "64x,");
        assert(names("nothing", Sort::FirstDrawn, false).empty());
        assert(names("", Sort::FirstDrawn, true) == "64x,512,");  // only the textures the pack lacks
        assert(no100f_texture_list::CleanName("  tex1_64x64_bbbb_0.PNG \r") == "tex1_64x64_bbbb_0");
        assert(no100f_texture_list::CleanName("tex1_64x64_bbbb_0.dds") == "tex1_64x64_bbbb_0");
        assert(no100f_texture_list::CleanName(" Off ").empty());
        assert(no100f_texture_list::CleanName("").empty());
        size_t matches = 0;
        assert(no100f_texture_list::Find(rows, "tex1_64x64_bbbb_0", &matches)->name == "tex1_64x64_bbbb_0" && matches == 1);
        assert(no100f_texture_list::Find(rows, "cccc", &matches)->name == "tex1_512x128_cccc_dddd_9" && matches == 1);
        assert(no100f_texture_list::Find(rows, "bbb", &matches) == nullptr && matches == 2);  // two rows: ask for more letters
        assert(no100f_texture_list::Find(rows, "zzzz", &matches) == nullptr && matches == 0);
        assert(no100f_texture_list::PaletteUnknown({"tex1_128x128_aaaa_0000000000000000_9"}));
        assert(!no100f_texture_list::PaletteUnknown({"tex1_128x128_aaaa_bbbb_9"}));
        assert(!no100f_texture_list::PaletteUnknown({"tex1_128x128_0000000000000000_14"}));  // no palette part
        assert(std::string(no100f_texture_list::PackColumn(-1)) == "-" && std::string(no100f_texture_list::PackColumn(0)) == "NO" &&
               std::string(no100f_texture_list::PackColumn(1)) == "yes");
    }

    // The palette bytes a palette texture's name hashes (texture_replacement's TLUT part): min..max index, in bytes.
    {
        using aurora::gfx::kPaletteFormatC14X2;
        using aurora::gfx::kPaletteFormatC4;
        using aurora::gfx::kPaletteFormatC8;
        using aurora::gfx::referenced_palette_slice;
        const uint8_t c8[] = {3, 7, 5, 200};
        const auto a8 = referenced_palette_slice(kPaletteFormatC8, c8, 4, 256 * 2);
        assert(a8 && a8->offset == 6 && a8->size == 2 * (200 + 1 - 3));
        const uint8_t c4[] = {0x21, 0x5A};  // nibbles 2 1 5 10
        const auto a4 = referenced_palette_slice(kPaletteFormatC4, c4, 2, 16 * 2);
        assert(a4 && a4->offset == 2 && a4->size == 2 * 10);
        const uint8_t c14[] = {0xC0, 0x05, 0x00, 0x09};  // big-endian 0xC005 -> 5 (top 2 bits unused), 9
        const auto a14 = referenced_palette_slice(kPaletteFormatC14X2, c14, 4, 16 * 2);
        assert(a14 && a14->offset == 10 && a14->size == 2 * 5);
        assert(!referenced_palette_slice(kPaletteFormatC8, c8, 4, 100 * 2));  // indices run past the palette
        assert(!referenced_palette_slice(kPaletteFormatC8, nullptr, 4, 512));
        assert(!referenced_palette_slice(kPaletteFormatC8, c8, 0, 512));
        assert(!referenced_palette_slice(/*I8*/ 1, c8, 4, 512));  // not a palette format
        // One texture's slice does not depend on any palette bytes: the hash is of the draw's own palette
        const uint8_t one[] = {0};
        const auto a0 = referenced_palette_slice(kPaletteFormatC8, one, 1, 2);
        assert(a0 && a0->offset == 0 && a0->size == 2);
    }

    fs::remove_all(root);
    return 0;
}
