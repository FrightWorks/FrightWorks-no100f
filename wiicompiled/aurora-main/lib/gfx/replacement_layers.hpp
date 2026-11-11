#pragma once

// The texture replacement index's folder order and lookup order, with no GPU or GX types so a test can build it alone
// (runtime/tests/no100f_texture_packs_tests.cpp).

#include <algorithm>
#include <filesystem>
#include <functional>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

namespace aurora::gfx::texture_replacement {

// A texture pack is one plain folder under <UserData>/texture_packs: never a path, so a name from a command file or
// the dev menu can't reach outside it.
inline bool valid_pack_name(std::string_view name) noexcept {
  if (name.empty() || name == "." || name == "..") {
    return false;
  }
  return std::none_of(name.begin(), name.end(), [](char c) {
    return c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20;
  });
}

// The usual chain, first wins: the user's texture_replacements, then the mods' textures folders (a mod holding a file
// named "disabled" is skipped; the alphabetically later mod wins, like their disc file overrides), then the game's own
// texture_replacements (the built-in HD pack) beside it.
inline std::vector<std::filesystem::path> usual_roots(const std::filesystem::path& userPath,
                                                      const std::filesystem::path& resourcesPath) {
  std::error_code ec;
  std::vector<std::filesystem::path> roots{userPath / "texture_replacements"};
  std::vector<std::filesystem::path> mods;
  for (const auto& entry : std::filesystem::directory_iterator(userPath / "mods", ec)) {
    if (entry.is_directory(ec) && !std::filesystem::exists(entry.path() / "disabled", ec) &&
        std::filesystem::is_directory(entry.path() / "textures", ec)) {
      mods.push_back(entry.path() / "textures");
    }
  }
  std::sort(mods.begin(), mods.end(), std::greater<>());
  roots.insert(roots.end(), mods.begin(), mods.end());
  if (!resourcesPath.empty() && std::filesystem::is_directory(resourcesPath / "texture_replacements", ec)) {
    roots.push_back(resourcesPath / "texture_replacements");
  }
  return roots;
}

// The selected texture pack's folder, or empty for "usual" (no pack) and for a name that isn't a plain folder name.
inline std::filesystem::path pack_root(const std::filesystem::path& userPath, std::string_view name) {
  if (!valid_pack_name(name)) {
    return {};
  }
  return userPath / "texture_packs" / std::filesystem::path(std::u8string_view(
                                          reinterpret_cast<const char8_t*>(name.data()), name.size()));
}

// One replacement lookup. `candidates` are the keys to try, most specific first (exact, TLUT wildcard, texture
// wildcard). With a texture pack selected only the pack's index is searched: a texture it doesn't have is missing, and
// never falls back to the usual chain (user texture_replacements, mods, the built-in HD pack). With no pack the usual
// index is searched, as it always was.
template <class Map>
const typename Map::mapped_type* find_active(const Map& pack, const Map& usual, bool packSelected,
                                             std::span<const typename Map::key_type> candidates) {
  const Map& index = packSelected ? pack : usual;
  for (const auto& key : candidates) {
    if (const auto it = index.find(key); it != index.end()) {
      return &it->second;
    }
  }
  return nullptr;
}

}  // namespace aurora::gfx::texture_replacement
