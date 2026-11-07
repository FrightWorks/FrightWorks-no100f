#include "runtime_config.h"

#include <iostream>
#include <sstream>

namespace {
bool Require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}
} // namespace

int main(int argc, char** argv) {
    {
        std::istringstream input("[video]\nmetalfx_spatial_upscaling = true\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "enabled.toml");
        if (!Require(config.metalFxSpatialUpscaling == std::optional<bool>{true},
                     "valid MetalFX setting was not loaded")) return 1;
    }
    {
        std::istringstream input("[video]\nmetalfx_spatial_upscaling = false\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "disabled.toml");
        if (!Require(config.metalFxSpatialUpscaling == std::optional<bool>{false},
                     "false MetalFX setting was not loaded")) return 1;
    }
    {
        std::istringstream input("[video]\nmetalfx_spatial_upscaling = \"yes\"\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "invalid.toml");
        if (!Require(!config.metalFxSpatialUpscaling, "invalid MetalFX setting was accepted")) return 1;
    }
    {
        std::istringstream input("[video]\nhdr = false\nhdr_paper_white_nits = 187.125\nhdr_peak_nits = 777.25\n");
        const auto config = RuntimeConfigFile::ParseConfig(input, "saved-hdr.toml");
        if (!Require(config.hdr == std::optional<bool>{false} && config.hdrPaperWhiteNits == 187.125f &&
                     config.hdrPeakNits == 777.25f, "explicit off or precise HDR values were lost")) return 1;
    }
    {
        std::istringstream input("[video]\nhdr = true\nhdr_paper_white_nits = 80\nhdr_peak_nits = 10000\n");
        auto config = RuntimeConfigFile::ParseConfig(input, "launcher-hdr.toml");
        if (!Require(RuntimeConfigFile::Hdr(config) && RuntimeConfigFile::HdrPaperWhiteNits(config) == 80.f &&
                     RuntimeConfigFile::HdrPeakNits(config) == 10000.f, "launcher HDR endpoints differ from runtime")) return 1;
        config.hdrPaperWhiteNits = 1000.f;
        config.hdrPeakNits = 80.f;
        if (!Require(RuntimeConfigFile::HdrPaperWhiteNits(config) == 1000.f && RuntimeConfigFile::HdrPeakNits(config) == 80.f,
                     "opposite HDR endpoints differ from launcher")) return 1;
    }
    {
        std::istringstream input("[video]\nhdr_peak_nits = 1234.5\n");
        const auto config = RuntimeConfigFile::ParseConfig(input, "partial-hdr.toml");
        if (!Require(!config.hdr && !config.hdrPaperWhiteNits && config.hdrPeakNits == 1234.5f,
                     "absent HDR keys confused with explicit defaults")) return 1;
    }
    {
        const RuntimeUserConfig empty;
        if (!Require(!RuntimeConfigFile::Hdr(empty) && RuntimeConfigFile::HdrPaperWhiteNits(empty) == 200.f &&
                     RuntimeConfigFile::HdrPeakNits(empty) == 1000.f, "safe HDR defaults changed")) return 1;
        if (!Require(!RuntimeConfigFile::SetHdrPaperWhiteNits(79.f) && !RuntimeConfigFile::SetHdrPaperWhiteNits(1001.f) &&
                     !RuntimeConfigFile::SetHdrPeakNits(79.f) && !RuntimeConfigFile::SetHdrPeakNits(10001.f) &&
                     !RuntimeConfigFile::SetHdrPeakNits(std::numeric_limits<float>::quiet_NaN()),
                     "invalid HDR values accepted")) return 1;
    }
    // Persistence is opt-in and must use a caller-owned portable copy of this CPU test.
    if (argc == 2) {
        const auto executable = RuntimeConfigFile::ExecutableDirectory();
        const auto expected = std::filesystem::absolute(argv[1]).lexically_normal();
        if (!Require(executable && std::filesystem::is_regular_file(*executable / "portable.txt") &&
                     expected == (*executable / "UserData" / "Config.toml").lexically_normal(),
                     "persistence checks require a portable marker and exact owned fixture path")) return 1;
        if (!Require(RuntimeConfigFile::ResolveConfigPath() == expected, "portable config path mismatch")) return 1;
        std::filesystem::create_directories(expected.parent_path());
        // Each persistence block starts from the same fixture: the layout block ends with manual intent set
        // (automatic matching off, layout "gamecube"), which the HDR block must not inherit.
        const auto writeFixture = [&expected]() {
            std::ofstream initial(expected, std::ios::trunc);
            initial << "[launcher]\nauto_controller_layout = true\nauto_controller_layout_value = \"xbox_original\"\n"
                       "[video]\nhdr = false\nhdr_paper_white_nits = 237.5\nhdr_peak_nits = 1234.5\n"
                       "[controller]\nlayout = \"xbox_original\"\na = \"north@30,east\"\n"
                       "xbox_black = \"left_paddle1\"\n[future]\nvalue = \"keep\"\n";
            return static_cast<bool>(initial);
        };
        if (!Require(writeFixture(), "could not initialize fixture config")) return 1;
        for (bool original : {true, false}) {
            if (!Require(RuntimeConfigFile::WriteSetting("launcher", "auto_controller_layout", "true"),
                         "could not reset automatic intent")) return 1;
            if (!Require(RuntimeConfigFile::SetXboxOriginalLayout(original), "manual F10 preset write failed")) return 1;
            const auto document = toml::parse(expected.string());
            if (!Require(!toml::find<bool>(document, "launcher", "auto_controller_layout") &&
                         toml::find<std::string>(document, "controller", "layout") ==
                         (original ? "xbox_original" : "gamecube"),
                         "same-value or changed manual preset did not stop automatic matching")) return 1;
            if (!Require(toml::find<std::string>(document, "launcher", "auto_controller_layout_value") == "xbox_original" &&
                         toml::find<std::string>(document, "controller", "a") == "north@30,east" &&
                         toml::find<std::string>(document, "controller", "xbox_black") == "left_paddle1" &&
                         toml::find<std::string>(document, "future", "value") == "keep" &&
                         !toml::find<bool>(document, "video", "hdr") &&
                         toml::find<double>(document, "video", "hdr_paper_white_nits") == 237.5 &&
                         toml::find<double>(document, "video", "hdr_peak_nits") == 1234.5,
                         "manual preset changed remembered intent, HDR, bindings or unrelated data")) return 1;
        }
        std::cout << "PASS same-value/changed F10 manual intent and cross-setting preservation\n";
        if (!Require(writeFixture(), "could not reinitialize fixture config for HDR persistence")) return 1;
        if (!Require(RuntimeConfigFile::SetHdrPaperWhiteNits(321.5f) && RuntimeConfigFile::SetHdrPeakNits(2000.f),
                     "valid HDR fixture persistence failed")) return 1;
        const auto document = toml::parse(expected.string());
        if (!Require(!toml::find<bool>(document, "video", "hdr") &&
                     toml::find<double>(document, "video", "hdr_paper_white_nits") == 321.5 &&
                     toml::find<double>(document, "video", "hdr_peak_nits") == 2000.0 &&
                     toml::find<bool>(document, "launcher", "auto_controller_layout") &&
                     toml::find<std::string>(document, "launcher", "auto_controller_layout_value") == "xbox_original" &&
                     toml::find<std::string>(document, "controller", "layout") == "xbox_original" &&
                     toml::find<std::string>(document, "controller", "a") == "north@30,east" &&
                     toml::find<std::string>(document, "controller", "xbox_black") == "left_paddle1" &&
                     toml::find<std::string>(document, "future", "value") == "keep",
                     "HDR persistence changed manual intent or unrelated settings")) return 1;
        std::cout << "PASS HDR persistence and preservation of controller intent/custom bindings\n";
    } else if (argc != 1) {
        std::cerr << "usage: runtime_config_tests [absolute portable fixture Config.toml]\n";
        return 1;
    }
    return 0;
}
