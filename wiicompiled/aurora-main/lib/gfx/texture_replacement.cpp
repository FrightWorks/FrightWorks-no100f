#include "texture_replacement.hpp"

#include "../internal.hpp"
#include "../gx/gx.hpp"
#include "../webgpu/gpu.hpp"
#include "dds_io.hpp"
#include "texture_convert.hpp"

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <fmt/format.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <functional>
#include <array>
#include <atomic>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>

#include "png_io.hpp"
#include "../fs_helper.hpp"

using namespace aurora::gx;
using aurora::webgpu::g_device;

namespace aurora::gfx::texture_replacement {
Module Log("aurora::gfx::texture_replacement");

struct RuntimeTextureKey {
  uint64_t textureHash = 0;
  uint64_t tlutHash = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool hasTlut = false;
  uint32_t format = 0;

  bool operator==(const RuntimeTextureKey& rhs) const = default;

  template <typename H>
  friend H AbslHashValue(H h, const RuntimeTextureKey& key) {
    return H::combine(std::move(h), key.textureHash, key.tlutHash, key.width, key.height, key.hasTlut, key.format);
  }
};

struct TlutMetadata {
  uint32_t size = 0;
  uint32_t format = 0;
  uint16_t entries = 0;
  bool valid = false;
  // Object the palette was registered from. GXLoadTlut consumes the registration, so this is what
  // lets the same object be loaded into more than one hardware slot.
  const GXTlutObj* source = nullptr;
  ByteBuffer data;
};

struct CachedReplacement {
  gfx::TextureHandle handle;
  uint64_t bytes = 0;
  std::list<RuntimeTextureKey>::iterator lruIt;
};

struct ReplacementIndexEntry {
  std::filesystem::path path;
  bool buttonIcons = false;  // the button prompt atlas, put together from button_icons (compose_button_icons)
};

absl::flat_hash_map<RuntimeTextureKey, ReplacementIndexEntry> s_replacementIndex;
// button_icons beside the game (<resourcesPath>, tools/make-button-icons.py; shipped, so never in the user's
// folder): the game's button picture, an atlas of the GameCube A, B, X
// and Y buttons, put together from one icon per pad button of the controller in use, so the prompts follow the
// player's button mapping. Looked up before everything else.
constexpr const char* kButtonIconFamilies[] = {"xbox", "playstation", "nintendo"};
constexpr const char* kButtonIconSlots[] = {"A", "B", "X", "Y"};
constexpr const char* kButtonIconDefaults[] = {"south", "east", "west", "north"};
struct ButtonIconLayout {
  bool valid = false;
  RuntimeTextureKey atlasKey{};
  uint32_t width = 0, height = 0;
  std::array<std::array<uint32_t, 2>, 4> slots{};  // top-left corner of each icon, kButtonIconSlots order
};
ButtonIconLayout s_buttonIconLayout;
std::filesystem::path s_buttonIconRoot;
const ReplacementIndexEntry s_buttonIconEntry{.buttonIcons = true};
std::mutex s_buttonIconMutex;
ButtonIcons s_buttonIcons;          // requested, under s_buttonIconMutex
bool s_buttonIconsChanged = false;  // under s_buttonIconMutex
ButtonIcons s_appliedButtonIcons;   // renderer thread
int s_buttonIconForced = -1;
absl::flat_hash_map<RuntimeTextureKey, CachedReplacement> s_replacementCache;
absl::flat_hash_set<RuntimeTextureKey> s_failedKeys;
absl::flat_hash_set<RuntimeTextureKey> s_reportedMisses;
absl::flat_hash_map<const GXTlutObj*, TlutMetadata> s_pendingTluts;
std::array<TlutMetadata, MaxTluts> s_loadedTluts{};
std::list<RuntimeTextureKey> s_replacementLru;
std::filesystem::path s_replacementRoot;
std::filesystem::path s_dumpRoot;
uint64_t s_replacementCacheBytes = 0;
constexpr uint64_t kReplacementCacheBudgetBytes = 4294967296; // 4GB, reasonable for modern hardware?
constexpr uint64_t kReplacementWildcardTextureHash = 0xFFFFFFFFFFFFFFFFull;
constexpr uint64_t kReplacementWildcardTlutHash = 0xFFFFFFFFFFFFFFFEull;

bool iequals_ascii(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }
  return true;
}

bool is_relative_to(const std::filesystem::path& path, const std::filesystem::path& root) noexcept {
  if (root.empty()) {
    return false;
  }
  auto pathIt = path.begin();
  auto rootIt = root.begin();
  for (; rootIt != root.end(); ++rootIt, ++pathIt) {
    if (pathIt == path.end() || !iequals_ascii(fs_path_to_string(*pathIt), fs_path_to_string(*rootIt))) {
      return false;
    }
  }
  return true;
}

bool is_sidecar_mip(std::string_view stem) noexcept {
  constexpr std::string_view tag = "_mip";
  size_t i = stem.size();
  while (i > 0 && stem[i - 1] >= '0' && stem[i - 1] <= '9') {
    --i;
  }

  if (i == stem.size() || i < tag.size()) {
    return false;
  }

  return stem.substr(i - tag.size(), tag.size()) == tag;
}

std::optional<uint64_t> parse_hex(std::string_view text) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }
  uint64_t value = 0;
  for (const char ch : text) {
    value <<= 4;
    if (ch >= '0' && ch <= '9') {
      value |= static_cast<uint64_t>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      value |= static_cast<uint64_t>(ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      value |= static_cast<uint64_t>(ch - 'A' + 10);
    } else {
      return std::nullopt;
    }
  }
  return value;
}

std::optional<uint32_t> parse_u32(std::string_view text, int base = 10) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }

  uint32_t value = 0;
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value, base);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::pair<uint32_t, uint32_t>> parse_dimensions(std::string_view text) noexcept {
  const size_t sep = text.find('x');
  if (sep == std::string_view::npos) {
    return std::nullopt;
  }

  const auto width = parse_u32(text.substr(0, sep));
  const auto height = parse_u32(text.substr(sep + 1));
  if (!width.has_value() || !height.has_value()) {
    return std::nullopt;
  }
  return std::pair{*width, *height};
}

uint32_t texture_base_level_size(const GXTexObj_& obj) noexcept {
  switch (obj.format()) {
  case GX_TF_R8_PC:
    return obj.width() * obj.height();
  case GX_TF_RGBA8_PC:
    return obj.width() * obj.height() * 4;
  default:
    return GXGetTexBufferSize(obj.width(), obj.height(), obj.format(), false, 0);
  }
}

std::optional<uint64_t> compute_referenced_tlut_hash(const GXTexObj_& obj) noexcept {
  if (!is_palette_format(obj.format()) || obj.tlut >= s_loadedTluts.size()) {
    return std::nullopt;
  }

  const auto& tlut = s_loadedTluts[obj.tlut];
  const uint32_t textureSize = texture_base_level_size(obj);
  const auto* textureData = static_cast<const uint8_t*>(obj.data);
  if (!tlut.valid || textureData == nullptr || textureSize == 0) {
    return std::nullopt;
  }

  uint32_t minIndex = 0xffff;
  uint32_t maxIndex = 0;
  switch (obj.format()) {
  case GX_TF_C4:
    for (uint32_t i = 0; i < textureSize; ++i) {
      const uint32_t lowNibble = textureData[i] & 0xf;
      const uint32_t highNibble = textureData[i] >> 4;
      minIndex = std::min({minIndex, lowNibble, highNibble});
      maxIndex = std::max({maxIndex, lowNibble, highNibble});
    }
    break;
  case GX_TF_C8:
    for (uint32_t i = 0; i < textureSize; ++i) {
      const uint32_t index = textureData[i];
      minIndex = std::min(minIndex, index);
      maxIndex = std::max(maxIndex, index);
    }
    break;
  case GX_TF_C14X2:
    for (uint32_t i = 0; i + sizeof(uint16_t) <= textureSize; i += sizeof(uint16_t)) {
      uint16_t value = 0;
      std::memcpy(&value, textureData + i, sizeof(value));
      const uint32_t index = bswap(value) & 0x3fff;
      minIndex = std::min(minIndex, index);
      maxIndex = std::max(maxIndex, index);
    }
    break;
  default:
    return std::nullopt;
  }

  size_t tlutSize = 2 * (static_cast<size_t>(maxIndex) + 1 - minIndex);
  const size_t tlutOffset = 2 * static_cast<size_t>(minIndex);
  if (tlutOffset + tlutSize > tlut.data.size()) {
    return std::nullopt;
  }
  return XXH64(tlut.data.data() + tlutOffset, tlutSize, 0);
}

const TlutMetadata* get_loaded_tlut(const GXTexObj_& obj) noexcept {
  if (!is_palette_format(obj.format()) || obj.tlut >= s_loadedTluts.size()) {
    return nullptr;
  }

  const auto& tlut = s_loadedTluts[obj.tlut];
  return tlut.valid ? &tlut : nullptr;
}

bool ensure_directory(const std::filesystem::path& dir) noexcept {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return !ec;
}

RuntimeTextureKey build_runtime_key(const GXTexObj_& obj) noexcept {
  RuntimeTextureKey key{
      .width = obj.width(),
      .height = obj.height(),
      .hasTlut = is_palette_format(obj.format()),
      .format = obj.format(),
  };

  const uint32_t textureSize = texture_base_level_size(obj);
  if (obj.data != nullptr && textureSize != 0) {
    key.textureHash = XXH64(obj.data, textureSize, 0);
  }
  if (key.hasTlut) {
    key.tlutHash = compute_referenced_tlut_hash(obj).value_or(0);
  }
  return key;
}

std::string format_replacement_filename(const RuntimeTextureKey& key) {
  if (key.hasTlut) {
    return fmt::format("tex1_{}x{}_{:016x}_{:016x}_{}.dds", key.width, key.height,
                       key.textureHash, key.tlutHash, key.format);
  }
  return fmt::format("tex1_{}x{}_{:016x}_{}.dds", key.width, key.height, key.textureHash, key.format);
}

std::optional<RuntimeTextureKey> parse_replacement_filename(std::string_view filename) noexcept {
  const size_t dot = filename.rfind('.');
  if (dot == std::string_view::npos) {
    return std::nullopt;
  }

  if (!iequals_ascii(filename.substr(dot), ".dds") && !iequals_ascii(filename.substr(dot), ".png")) {
    return std::nullopt;
  }

  const std::string_view stem = filename.substr(0, dot);
  constexpr std::string_view prefix = "tex1_";
  if (!stem.starts_with(prefix)) {
    return std::nullopt;
  }

  std::array<std::string_view, 6> parts{};
  size_t partCount = 0;
  size_t offset = 0;
  bool consumedAll = false;
  while (offset <= stem.size() && partCount < parts.size()) {
    const size_t next = stem.find('_', offset);
    parts[partCount++] = stem.substr(offset, next == std::string_view::npos ? stem.size() - offset : next - offset);
    if (next == std::string_view::npos) {
      consumedAll = true;
      break;
    }
    offset = next + 1;
  }
  if (!consumedAll || partCount < 4 || partCount > 6 || parts[0] != "tex1") {
    return std::nullopt;
  }

  const auto dimensions = parse_dimensions(parts[1]);
  if (!dimensions.has_value()) {
    return std::nullopt;
  }

  size_t index = 2;
  if (parts[index] == "m") {
    ++index;
  }

  size_t remaining = partCount - index;
  if (remaining != 2 && remaining != 3) {
    return std::nullopt;
  }

  uint64_t textureHash = 0;
  if (parts[index] == "$") {
    textureHash = kReplacementWildcardTextureHash;
  } else {
    const auto parsedTex = parse_hex(parts[index]);
    if (!parsedTex.has_value()) {
      return std::nullopt;
    }
    textureHash = *parsedTex;
  }

  auto formatPart = parts[partCount - 1];
  if (formatPart == "arb") {
    formatPart = parts[partCount - 2];
    remaining -= 1;
  }
  const auto format = parse_u32(formatPart);
  if (!format.has_value()) {
    return std::nullopt;
  }

  uint64_t tlutHash = 0;
  const bool hasTlut = remaining == 3;
  if (hasTlut) {
    const std::string_view tlutPart = parts[index + 1];
    if (tlutPart == "$") {
      tlutHash = kReplacementWildcardTlutHash;
    } else {
      const auto parsedTlutHash = parse_hex(tlutPart);
      if (!parsedTlutHash.has_value()) {
        return std::nullopt;
      }
      tlutHash = *parsedTlutHash;
    }
  }

  return RuntimeTextureKey{
      .textureHash = textureHash,
      .tlutHash = tlutHash,
      .width = dimensions->first,
      .height = dimensions->second,
      .hasTlut = hasTlut,
      .format = *format,
  };
}

static std::optional<ConvertedTexture> load_texture_file(const std::filesystem::path& path) {
  if (path.extension() == ".png") {
    return png::load_png_file(path);
  } else {
    return dds::load_dds_file(path);
  }
}

constexpr bool isUnsupportedTextureFormat(const ConvertedTexture& texture) {
  switch (texture.format) {
  case wgpu::TextureFormat::BC1RGBAUnorm:
  case wgpu::TextureFormat::BC1RGBAUnormSrgb:
  case wgpu::TextureFormat::BC2RGBAUnorm:
  case wgpu::TextureFormat::BC2RGBAUnormSrgb:
  case wgpu::TextureFormat::BC3RGBAUnorm:
  case wgpu::TextureFormat::BC3RGBAUnormSrgb:
  case wgpu::TextureFormat::BC4RUnorm:
  case wgpu::TextureFormat::BC4RSnorm:
  case wgpu::TextureFormat::BC5RGUnorm:
  case wgpu::TextureFormat::BC5RGSnorm:
  case wgpu::TextureFormat::BC6HRGBUfloat:
  case wgpu::TextureFormat::BC6HRGBFloat:
  case wgpu::TextureFormat::BC7RGBAUnorm:
  case wgpu::TextureFormat::BC7RGBAUnormSrgb:
    return !webgpu::g_bcTexturesSupported;
  default:
    return false;
  }
}

// The prompt atlas for s_appliedButtonIcons: each GameCube button's spot gets the icon of the pad button it is mapped
// to, or its usual one when the family has no icon for that pad button.
static std::optional<ConvertedTexture> compose_button_icons() noexcept {
  const auto& layout = s_buttonIconLayout;
  const auto& icons = s_appliedButtonIcons;
  if (!layout.valid || icons.family < 0 || icons.family >= static_cast<int>(std::size(kButtonIconFamilies))) {
    return std::nullopt;
  }
  const auto dir = s_buttonIconRoot / kButtonIconFamilies[icons.family];
  ByteBuffer data{static_cast<size_t>(layout.width) * layout.height * 4};  // zeroed: transparent
  for (size_t slot = 0; slot < std::size(kButtonIconSlots); ++slot) {
    std::optional<ConvertedTexture> icon;
    for (const std::string_view name : {std::string_view(icons.buttons[slot]), std::string_view(kButtonIconDefaults[slot])}) {
      const bool plain = !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
      });
      std::error_code ec;
      const auto path = dir / fmt::format("{}.png", name);
      if (plain && std::filesystem::is_regular_file(path, ec) && (icon = png::load_png_file(path))) break;
    }
    if (!icon || icon->format != wgpu::TextureFormat::RGBA8Unorm ||
        icon->data.size() < static_cast<size_t>(icon->width) * icon->height * 4) {
      Log.warn("Button icons: no {} icon for the {} button in {}", kButtonIconFamilies[icons.family],
               kButtonIconSlots[slot], fs_path_to_string(dir));
      continue;
    }
    const auto [x0, y0] = layout.slots[slot];
    if (x0 >= layout.width || y0 >= layout.height) continue;
    const uint32_t columns = std::min(icon->width, layout.width - x0);
    for (uint32_t y = 0; y < icon->height && y0 + y < layout.height; ++y) {
      std::memcpy(data.data() + (static_cast<size_t>(y0 + y) * layout.width + x0) * 4,
                  icon->data.data() + static_cast<size_t>(y) * icon->width * 4, static_cast<size_t>(columns) * 4);
    }
  }
  return ConvertedTexture{
      .format = wgpu::TextureFormat::RGBA8Unorm,
      .width = layout.width,
      .height = layout.height,
      .mips = 1,
      .data = std::move(data),
  };
}

std::optional<ConvertedTexture> load_replacement(const ReplacementIndexEntry& entry) noexcept {
  if (entry.buttonIcons) return compose_button_icons();
  auto base = load_texture_file(entry.path);
  if (!base.has_value()) {
    Log.warn("texture_replacement: failed to load texture {}", fs_path_to_string(entry.path));
    return std::nullopt;
  }
  if (isUnsupportedTextureFormat(base.value())) {
    Log.warn(
      "texture_replacement: failed to load texture {} due to unsupported format: {}",
      fs_path_to_string(entry.path),
      static_cast<uint32_t>(base->format));
    return std::nullopt;
  }

  std::vector<ConvertedTexture> more;
  std::error_code ec;
  for (uint32_t mipLevel = 1;; ++mipLevel) {
    const auto mipPath = entry.path.parent_path() / fmt::format("{}_mip{}{}", fs_path_to_string(entry.path.stem()), mipLevel, fs_path_to_string(entry.path.extension()));
    if (!std::filesystem::is_regular_file(mipPath, ec)) {
      break;
    }

    auto lvl = load_texture_file(mipPath);
    const uint32_t ew = std::max(base->width >> mipLevel, 1u);
    const uint32_t eh = std::max(base->height >> mipLevel, 1u);
    const bool ok = lvl.has_value() && lvl->format == base->format && lvl->width == ew && lvl->height == eh;
    if (!ok) {
      if (!lvl.has_value()) {
        Log.warn("texture_replacement: could not load mip {}", fs_path_to_string(mipPath));
      } else {
        Log.warn("texture_replacement: expected {}x{} for mip {}, got {}x{}", ew, eh, fs_path_to_string(mipPath),
                 lvl->width, lvl->height);
      }

      break;
    }
    more.push_back(std::move(*lvl));
  }

  if (more.empty()) {
    return base;
  }

  const uint32_t mips = 1u + static_cast<uint32_t>(more.size());
  const uint64_t n = calc_texture_size(base->format, base->width, base->height, mips);
  if (n == 0) {
    return std::nullopt;
  }

  ByteBuffer blob{static_cast<size_t>(n)};
  uint8_t* const dst = blob.data();
  uint64_t o = 0;
  const auto append = [&](const ByteBuffer& d) noexcept -> bool {
    if (o + d.size() > n) {
      return false;
    }
    std::memcpy(dst + o, d.data(), d.size());
    o += d.size();
    return true;
  };
  if (!append(base->data)) {
    return std::nullopt;
  }
  for (const auto& mip : more) {
    if (!append(mip.data)) {
      return std::nullopt;
    }
  }
  if (o != n) {
    return std::nullopt;
  }

  return ConvertedTexture{
      .format = base->format,
      .width = base->width,
      .height = base->height,
      .mips = mips,
      .data = std::move(blob),
  };
}

void touch_cached_replacement(decltype(s_replacementCache)::iterator it) noexcept {
  if (it->second.lruIt != s_replacementLru.begin()) {
    s_replacementLru.splice(s_replacementLru.begin(), s_replacementLru, it->second.lruIt);
    it->second.lruIt = s_replacementLru.begin();
  }
}

void evict_replacement_cache_if_needed() noexcept {
  while (s_replacementCacheBytes > kReplacementCacheBudgetBytes && !s_replacementLru.empty()) {
    const RuntimeTextureKey key = s_replacementLru.back();
    s_replacementLru.pop_back();

    const auto it = s_replacementCache.find(key);
    if (it == s_replacementCache.end()) {
      continue;
    }

    const uint64_t entryBytes = it->second.bytes;
    s_replacementCache.erase(it);
    s_replacementCacheBytes -= std::min(s_replacementCacheBytes, entryBytes);
  }
}

// Button prompts follow the controller whether or not texture replacements (the HD pack) are on.
// button_icons/layout.txt: "atlas <file name> <width> <height>", "icon <size>", then "<A|B|X|Y> <left> <top>".
static void index_button_icons(const std::filesystem::path& resourcesPath) noexcept {
  s_buttonIconLayout = {};
  s_buttonIconRoot = resourcesPath / "button_icons";
  std::ifstream file(s_buttonIconRoot / "layout.txt");
  ButtonIconLayout layout;
  bool atlas = false;
  std::array<bool, 4> placed{};
  std::string line;
  while (std::getline(file, line)) {
    std::istringstream in(line);
    std::string what;
    in >> what;
    if (what == "atlas") {
      std::string name;
      in >> name >> layout.width >> layout.height;
      const auto parsed = parse_replacement_filename(name);
      atlas = !in.fail() && parsed.has_value() && layout.width > 0 && layout.height > 0 && layout.width <= 4096 &&
              layout.height <= 4096;
      if (parsed.has_value()) layout.atlasKey = *parsed;
    }
    for (size_t slot = 0; slot < std::size(kButtonIconSlots); ++slot) {
      if (what != kButtonIconSlots[slot]) continue;
      in >> layout.slots[slot][0] >> layout.slots[slot][1];
      placed[slot] = !in.fail();
    }
  }
  layout.valid = atlas && std::all_of(placed.begin(), placed.end(), [](bool p) { return p; });
  if (layout.valid) s_buttonIconLayout = layout;
  Log.info("Button icons: {}", layout.valid ? fmt::format("{}x{} atlas from {}", layout.width, layout.height,
                                                          fs_path_to_string(s_buttonIconRoot))
                                            : std::string("none (no valid button_icons/layout.txt)"));
  // BUTTON_ICONS=xbox|playstation|nintendo pins a family (tests: scripted input never reaches the gamepad code).
  if (const char* forced = std::getenv("BUTTON_ICONS")) {
    for (size_t family = 0; family < std::size(kButtonIconFamilies); ++family)
      if (std::string_view(forced) == kButtonIconFamilies[family]) s_buttonIconForced = static_cast<int>(family);
    set_button_icons({});
  }
}

void build_index() noexcept {
  auto userPath = fs_path_from_string(g_config.userPath);
  // Files the game ships (button_icons, the built-in HD pack in texture_replacements) sit beside it, not in the user's
  // folder: an installed game's program folder is replaced on every update.
  const auto resourcesPath = g_config.resourcesPath != nullptr ? fs_path_from_string(g_config.resourcesPath)
                                                               : std::filesystem::path{};
  index_button_icons(resourcesPath);
  if (!g_config.allowTextureReplacements) {
    return;
  }

  auto cachePath = fs_path_from_string(g_config.cachePath);

  s_replacementRoot = userPath / "texture_replacements";
  s_dumpRoot = cachePath / "texture_dumps";

  if (!ensure_directory(s_replacementRoot)) {
    return;
  }
  if (g_config.allowTextureDumps && !ensure_directory(s_dumpRoot)) {
    return;
  }

  std::error_code ec;
  // Texture mods: <UserData>/mods/<name>/textures (skipped when the mod folder holds a file
  // named "disabled"). The user's own folder is indexed first and try_emplace keeps the first entry, so
  // it wins over mods; among mods the alphabetically later one wins, like their disc file overrides.
  // The game's own texture_replacements (the built-in HD pack) come last, so anything of the user's wins.
  std::vector<std::filesystem::path> roots{s_replacementRoot};
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
  ec.clear();
  for (const auto& root : roots)
  for (std::filesystem::recursive_directory_iterator it(
           root,
           std::filesystem::directory_options::skip_permission_denied |
               std::filesystem::directory_options::follow_directory_symlink,
           ec);
       it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      break;
    }

    if (!it->is_regular_file()) {
      continue;
    }

    const auto& path = it->path();

    if (is_relative_to(path, s_dumpRoot)) {
      continue;
    }

    if (!iequals_ascii(fs_path_to_string(path.extension()), ".dds") && !iequals_ascii(fs_path_to_string(path.extension()), ".png")) {
      continue;
    }

    if (is_sidecar_mip(fs_path_to_string(path.stem()))) {
      continue;
    }

    const auto parsed = parse_replacement_filename(fs_path_to_string(path.filename()));
    if (!parsed.has_value()) {
      continue;
    }

    s_replacementIndex.try_emplace(*parsed, path);
  }

  Log.info("Indexed {} texture replacements from {} folder(s) (texture_replacements + mods + the game's own)", s_replacementIndex.size(), roots.size());
}

const ReplacementIndexEntry* find_replacement_path(const RuntimeTextureKey& key) noexcept {
  if (s_appliedButtonIcons.family >= 0 && s_buttonIconLayout.valid && key == s_buttonIconLayout.atlasKey) {
    return &s_buttonIconEntry;
  }
  if (!g_config.allowTextureReplacements) return nullptr;
  if (const auto it = s_replacementIndex.find(key); it != s_replacementIndex.end()) {
    return &it->second;
  }

  if (key.hasTlut) {
    RuntimeTextureKey tlutWildcardKey = key;
    tlutWildcardKey.tlutHash = kReplacementWildcardTlutHash;
    if (const auto it = s_replacementIndex.find(tlutWildcardKey); it != s_replacementIndex.end()) {
      return &it->second;
    }
  }

  RuntimeTextureKey textureWildcardKey = key;
  textureWildcardKey.textureHash = kReplacementWildcardTextureHash;
  if (const auto it = s_replacementIndex.find(textureWildcardKey); it != s_replacementIndex.end()) {
    return &it->second;
  }

  return nullptr;
}

const gfx::TextureHandle* find_cached_replacement(const RuntimeTextureKey& key) noexcept {
  const auto cached = s_replacementCache.find(key);
  if (cached == s_replacementCache.end()) {
    return nullptr;
  }

  touch_cached_replacement(cached);
  return &cached->second.handle;
}

gfx::TextureHandle load_replacement_texture(const RuntimeTextureKey& key, const ReplacementIndexEntry& entry) noexcept {
  const auto replacement = load_replacement(entry);
  if (!replacement.has_value()) {
    s_failedKeys.insert(key);
    return {};
  }

  const auto label = fmt::format("TextureReplacement {}", format_replacement_filename(key));
  const wgpu::Extent3D size{
      .width = replacement->width,
      .height = replacement->height,
      .depthOrArrayLayers = 1,
  };
  const wgpu::TextureDescriptor textureDescriptor{
      .label = label.c_str(),
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = replacement->format,
      .mipLevelCount = replacement->mips,
      .sampleCount = 1,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);
  const auto viewLabel = fmt::format("{} view", label);
  const wgpu::TextureViewDescriptor textureViewDescriptor{
      .label = viewLabel.c_str(),
      .format = replacement->format,
      .dimension = wgpu::TextureViewDimension::e2D,
      .mipLevelCount = replacement->mips,
  };
  auto textureView = texture.CreateView(&textureViewDescriptor);
  auto handle = std::make_shared<gfx::TextureRef>(std::move(texture), std::move(textureView), wgpu::TextureView{}, size,
                                                  replacement->format, replacement->mips, gfx::InvalidTextureFormat);
  handle->isReplacement = true;
  gfx::write_texture(*handle, replacement->data);
  return handle;
}

void cache_replacement(const RuntimeTextureKey& key, const gfx::TextureHandle& handle) noexcept {
  const uint64_t replacementBytes =
      calc_texture_size(handle->format, handle->size.width, handle->size.height, handle->mipCount);
  s_replacementLru.push_front(key);
  s_replacementCache.emplace(
      key, CachedReplacement{.handle = handle, .bytes = replacementBytes, .lruIt = s_replacementLru.begin()});
  s_replacementCacheBytes += replacementBytes;
  evict_replacement_cache_if_needed();
}

bool dump_editable_texture_dds(const RuntimeTextureKey& key, const GXTexObj_& obj) noexcept {
  const ArrayRef<uint8_t> texData{static_cast<const uint8_t*>(obj.data), UINT32_MAX};
  const uint32_t texWidth = obj.width();
  const uint32_t texHeight = obj.height();

  ConvertedTexture pixels;
  if (is_palette_format(obj.format())) {
    const TlutMetadata* tlut = get_loaded_tlut(obj);
    if (tlut == nullptr) {
      return false;
    }
    pixels =
        convert_texture_palette(obj.format(), texWidth, texHeight, 1, texData, static_cast<GXTlutFmt>(tlut->format),
                                tlut->entries, {tlut->data.data(), tlut->data.size()});
  } else {
    pixels = convert_texture(obj.format(), texWidth, texHeight, 1, texData);
  }

  const uint64_t rgbaBytes = calc_texture_size(wgpu::TextureFormat::RGBA8Unorm, texWidth, texHeight, 1);

  if (pixels.data.empty() || pixels.format != wgpu::TextureFormat::RGBA8Unorm || pixels.data.size() != rgbaBytes) {
    return false;
  }

  const auto path = s_dumpRoot / format_replacement_filename(key);
  return dds::write_rgba8_dds(path, texWidth, texHeight, pixels.data);
}

bool report_missing_key(const RuntimeTextureKey& key, const GXTexObj_& obj) noexcept {
  if (!s_reportedMisses.insert(key).second) {
    return false;
  }

  if (g_config.allowTextureDumps) {
    dump_editable_texture_dds(key, obj);
  }
  return true;
}

// Dev menu (F8): names of the replacements the game has drawn since start, for the dev menu's Textures tab.
// find_replacement runs once per texture upload (the GX texture caches keep the handle), so this stays cheap.
absl::flat_hash_set<RuntimeTextureKey> s_usedKeys;
absl::flat_hash_set<std::string> s_usedNames;
std::mutex s_usedMutex;

void note_used_replacement(const RuntimeTextureKey& key) noexcept {
  std::lock_guard lock(s_usedMutex);
  if (!s_usedKeys.insert(key).second) {
    return;
  }
  auto name = format_replacement_filename(key);
  name.resize(name.size() - 4);  // ".dds"
  s_usedNames.insert(std::move(name));
}

bool replacement_used(std::string_view name) noexcept {
  std::lock_guard lock(s_usedMutex);
  return s_usedNames.contains(name);
}

// Dev menu (F8): forget what was drawn (new level). Raises the highlight-change flag so the GX caches are
// dropped and every texture still on screen goes through find_replacement (and gets noted) again.
std::atomic<bool> s_highlightChanged{false};

void reset_used() noexcept {
  {
    std::lock_guard lock(s_usedMutex);
    s_usedKeys.clear();
    s_usedNames.clear();
  }
  s_highlightChanged.store(true);
}

// Dev menu (F8): the dev menu's "Find" draws one replacement solid magenta so it stands out in the scene.
std::optional<RuntimeTextureKey> s_highlightKey;  // guarded by s_usedMutex
gfx::TextureHandle s_highlightTexture;

void set_highlight(std::string_view name) noexcept {
  std::optional<RuntimeTextureKey> key;
  if (!name.empty()) key = parse_replacement_filename(std::string(name) + ".dds");
  std::lock_guard lock(s_usedMutex);
  s_highlightKey = key;
  s_highlightChanged.store(true);
}

bool take_highlight_change() noexcept { return s_highlightChanged.exchange(false); }

bool is_highlighted(const RuntimeTextureKey& key) noexcept {
  std::lock_guard lock(s_usedMutex);
  return s_highlightKey.has_value() && *s_highlightKey == key;
}

bool is_highlight_handle(const gfx::TextureHandle& handle) noexcept { return handle && handle == s_highlightTexture; }

// Dev menu (F8): the texture log's scene tag; the dev menu sets it when the scene or its log switch changes.
std::mutex s_texLogMutex;
std::string s_texLogScene;  // guarded by s_texLogMutex
std::atomic<uint32_t> s_texLogGeneration{0};

void set_texture_log_scene(std::string_view scene) noexcept {
  std::lock_guard lock(s_texLogMutex);
  s_texLogScene = scene;
  s_texLogGeneration.fetch_add(1);
}

std::string texture_log_scene() noexcept {
  std::lock_guard lock(s_texLogMutex);
  return s_texLogScene;
}

uint32_t texture_log_generation() noexcept { return s_texLogGeneration.load(std::memory_order_relaxed); }

gfx::TextureHandle highlight_texture() noexcept {
  if (s_highlightTexture) return s_highlightTexture;
  constexpr wgpu::Extent3D size{.width = 4, .height = 4, .depthOrArrayLayers = 1};
  const wgpu::TextureDescriptor textureDescriptor{
      .label = "TextureReplacement highlight",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = wgpu::TextureFormat::RGBA8Unorm,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);
  const wgpu::TextureViewDescriptor textureViewDescriptor{
      .label = "TextureReplacement highlight view",
      .format = wgpu::TextureFormat::RGBA8Unorm,
      .dimension = wgpu::TextureViewDimension::e2D,
      .mipLevelCount = 1,
  };
  auto textureView = texture.CreateView(&textureViewDescriptor);
  s_highlightTexture = std::make_shared<gfx::TextureRef>(std::move(texture), std::move(textureView), wgpu::TextureView{},
                                                         size, wgpu::TextureFormat::RGBA8Unorm, 1, gfx::InvalidTextureFormat);
  s_highlightTexture->isReplacement = true;
  std::array<uint8_t, 4 * 4 * 4> magenta{};
  for (size_t i = 0; i < magenta.size(); i += 4) {
    magenta[i] = 255;
    magenta[i + 2] = 255;
    magenta[i + 3] = 255;
  }
  gfx::write_texture(*s_highlightTexture, {magenta.data(), magenta.size()});
  return s_highlightTexture;
}

void initialize() noexcept { build_index(); }

void shutdown() noexcept {
  s_replacementIndex.clear();
  s_replacementCache.clear();
  s_failedKeys.clear();
  s_reportedMisses.clear();
  s_pendingTluts.clear();
  for (auto& tlut : s_loadedTluts) {
    tlut = {};
  }
  s_replacementLru.clear();
  s_buttonIconLayout = {};
  s_highlightTexture = {};
  s_replacementCacheBytes = 0;
  s_replacementRoot.clear();
  s_dumpRoot.clear();
}

// Mirrors of the guest palettes, kept only so find_replacement can hash and the DDS dump decode a
// CI palette. The renderer uploads from the guest pointer, so skip this work when replacement is off.
void register_tlut(const GXTlutObj* obj, const void* data, GXTlutFmt format, uint16_t entries) noexcept {
  if (!g_config.allowTextureReplacements) {
    return;
  }
  if (obj == nullptr || data == nullptr) {
    return;
  }

  // A registration is only consumed by a matching load_tlut, and the map is keyed by a guest address
  // the game may free and reuse, so a table this large means stale entries: drop them wholesale.
  constexpr size_t kMaxPendingTluts = 4096;
  if (s_pendingTluts.size() >= kMaxPendingTluts && !s_pendingTluts.contains(obj)) {
    s_pendingTluts.clear();
  }

  const size_t sz = static_cast<size_t>(entries) * 2;
  ByteBuffer buffer{sz};
  std::memcpy(buffer.data(), static_cast<const uint8_t*>(data), sz);
  s_pendingTluts[obj] = {
      .size = static_cast<uint32_t>(entries) * 2,
      .format = static_cast<uint32_t>(format),
      .entries = entries,
      .valid = true,
      .source = obj,
      .data = std::move(buffer),
  };
}

void load_tlut(const GXTlutObj* obj, uint32_t idx) noexcept {
  if (!g_config.allowTextureReplacements) {
    return;
  }
  if (idx >= s_loadedTluts.size()) {
    return;
  }

  // Consume the pending registration. The map is keyed by a guest object address that can be freed
  // or reused at any time, and entries used to live until shutdown.
  if (const auto it = s_pendingTluts.find(obj); it != s_pendingTluts.end()) {
    s_loadedTluts[idx] = std::move(it->second);
    s_pendingTluts.erase(it);
    return;
  }

  // The same object may legally be loaded into several hardware slots; only the first load finds the
  // pending entry, so recover the palette from whichever slot already holds it.
  for (const auto& loaded : s_loadedTluts) {
    if (!loaded.valid || loaded.source != obj) {
      continue;
    }
    s_loadedTluts[idx] = {
        .size = loaded.size,
        .format = loaded.format,
        .entries = loaded.entries,
        .valid = loaded.valid,
        .source = loaded.source,
        .data = loaded.data.clone(),
    };
    return;
  }

  s_loadedTluts[idx] = {};
}

std::optional<TextureHandle> find_replacement(const GXTexObj_& obj) noexcept {
  ZoneScoped;

  if (!g_config.allowTextureReplacements && s_appliedButtonIcons.family < 0) {
    return std::nullopt;
  }

  const RuntimeTextureKey key = build_runtime_key(obj);
  const auto* path = find_replacement_path(key);
  if (path == nullptr) {
    if (g_config.allowTextureReplacements) report_missing_key(key, obj);
    return std::nullopt;
  }
  note_used_replacement(key);
  if (is_highlighted(key)) {
    return highlight_texture();
  }

  if (const auto* cached = find_cached_replacement(key); cached != nullptr) {
    return *cached;
  }

  if (s_failedKeys.contains(key)) {
    return std::nullopt;
  }

  auto handle = load_replacement_texture(key, *path);
  if (!handle) {
    return std::nullopt;
  }

  cache_replacement(key, handle);
  return handle;
}

std::string button_icon_folder() noexcept {
  std::lock_guard lock(s_buttonIconMutex);
  if (s_buttonIcons.family < 0 || s_buttonIconRoot.empty()) return {};
  return fs_path_to_string(s_buttonIconRoot / kButtonIconFamilies[s_buttonIcons.family]);
}

void set_button_icons(const ButtonIcons& requested) noexcept {
  ButtonIcons icons = requested;
  if (s_buttonIconForced >= 0) icons.family = s_buttonIconForced;
  if (icons.family < -1 || icons.family >= static_cast<int>(std::size(kButtonIconFamilies))) icons.family = -1;
  if (icons.family < 0) icons.buttons = {};
  std::lock_guard lock(s_buttonIconMutex);
  if (!(s_buttonIcons == icons)) {
    s_buttonIcons = std::move(icons);
    s_buttonIconsChanged = true;
  }
}

// Called on the renderer's thread before textures are resolved: drops the cached atlas of the old icons.
bool take_button_icon_style_change() noexcept {
  {
    std::lock_guard lock(s_buttonIconMutex);
    if (!s_buttonIconsChanged) return false;
    s_buttonIconsChanged = false;
    s_appliedButtonIcons = s_buttonIcons;
  }
  if (s_buttonIconLayout.valid) {
    const auto& key = s_buttonIconLayout.atlasKey;
    if (const auto it = s_replacementCache.find(key); it != s_replacementCache.end()) {
      s_replacementLru.erase(it->second.lruIt);
      s_replacementCacheBytes -= std::min(s_replacementCacheBytes, it->second.bytes);
      s_replacementCache.erase(it);
    }
    s_failedKeys.erase(key);
  }
  return true;
}

std::string build_texture_replacement_name(const GXTexObj_& obj) noexcept {
  const RuntimeTextureKey key = build_runtime_key(obj);
  return format_replacement_filename(key);
}

} // namespace aurora::gfx::texture_replacement
