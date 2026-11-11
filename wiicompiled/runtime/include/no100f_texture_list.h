#pragma once

// Dev menu texture list (F8 Textures tab, `texlist` and `texfind` in devmenu.txt): the rows, the filter, the sort and
// the name lookup. Needs neither the game nor aurora, so a test can run it (tests/no100f_texture_packs_tests.cpp).

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace no100f_texture_list {

struct Row {
    std::string name;  // replacement file name without extension, as a texture pack or a dump needs it
    uint32_t width = 0, height = 0;
    std::string format;
    int pack = -1;  // -1 no pack selected, 0 the selected pack has no file for it, 1 it has
    bool usual = false;  // the usual chain (texture_replacements, mods, HD pack) has one
};

inline std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Every word of the filter must appear (letters of either case) in the name, the size ("256x256") or the format.
inline bool Matches(const Row& row, const std::string& filter) {
    if (filter.empty()) return true;
    const std::string hay =
        Lower(row.name + " " + std::to_string(row.width) + "x" + std::to_string(row.height) + " " + row.format);
    std::istringstream words(Lower(filter));
    std::string word;
    while (words >> word)
        if (hay.find(word) == std::string::npos) return false;
    return true;
}

enum class Sort { FirstDrawn = 0, Name, LargestFirst, Format };

// `rows` come in the order first drawn; ties keep that order.
inline std::vector<const Row*> Shown(const std::vector<Row>& rows, const std::string& filter, Sort sort,
                                     bool onlyMissingFromPack) {
    std::vector<const Row*> shown;
    for (const Row& row : rows)
        if (Matches(row, filter) && !(onlyMissingFromPack && row.pack != 0)) shown.push_back(&row);
    switch (sort) {
    case Sort::Name:
        std::stable_sort(shown.begin(), shown.end(), [](auto* a, auto* b) { return a->name < b->name; });
        break;
    case Sort::LargestFirst:
        std::stable_sort(shown.begin(), shown.end(), [](auto* a, auto* b) {
            return uint64_t(a->width) * a->height > uint64_t(b->width) * b->height;
        });
        break;
    case Sort::Format:
        std::stable_sort(shown.begin(), shown.end(), [](auto* a, auto* b) { return a->format < b->format; });
        break;
    case Sort::FirstDrawn: break;
    }
    return shown;
}

// What `texfind` was given: surrounding spaces and a .png/.dds ending go; empty or "off" means switch off.
inline std::string CleanName(std::string name) {
    const size_t first = name.find_first_not_of(" \t\r");
    name = first == std::string::npos ? std::string() : name.substr(first, name.find_last_not_of(" \t\r") + 1 - first);
    for (const char* ext : {".png", ".dds"})
        if (name.size() > 4 && Lower(name.substr(name.size() - 4)) == ext) name.resize(name.size() - 4);
    return Lower(name) == "off" ? std::string() : name;
}

// The row `texfind` means: the exact name, else the only row whose name contains it. nullptr when none or several
// match (`matches` says how many).
inline const Row* Find(const std::vector<Row>& rows, const std::string& name, size_t* matches = nullptr) {
    size_t count = 0;
    const Row* only = nullptr;
    for (const Row& row : rows) {
        if (row.name == name) {
            if (matches) *matches = 1;
            return &row;
        }
        if (Lower(row.name).find(Lower(name)) != std::string::npos) {
            ++count;
            only = &row;
        }
    }
    if (matches) *matches = count;
    return count == 1 ? only : nullptr;
}

// A palette texture whose palette wasn't loaded when it was resolved is keyed with palette hash 0 (the file name's
// fourth part, 16 zeros); such a name still matches a file called that.
inline bool PaletteUnknown(const Row& row) {
    // tex1_<w>x<h>_<texture hash>_<palette hash>_<format>: five parts for a palette texture.
    std::vector<std::string> parts;
    std::istringstream in(row.name);
    for (std::string part; std::getline(in, part, '_');) parts.push_back(part);
    return parts.size() == 5 && parts[3] == "0000000000000000";
}

inline const char* PackColumn(int pack) { return pack < 0 ? "-" : pack ? "yes" : "NO"; }

}  // namespace no100f_texture_list
