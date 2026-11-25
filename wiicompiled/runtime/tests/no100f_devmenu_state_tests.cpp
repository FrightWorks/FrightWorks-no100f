#include "no100f_devmenu_state.h"
#include "no100f_devmenu_progress.h"

#include <cassert>
#include <iterator>
#include <map>
#include <set>
#include <string_view>

int main() {
    using namespace no100f_devmenu;
    static_assert(kWarpScenes.size() == 26);
    static_assert(kWarpScenes[10] == 0x48303031);
    static_assert(kWarpScenes[20] == 0x53303032);
    static_assert(kWarpScenes[23] == 0x4C303135);
    static_assert(kWarpScenes[24] == 0x47303031);
    static_assert(kWarpScenes[25] == kGalleryScene);
    std::map<uint32_t, uint32_t> memory;
    const auto read = [&](uint32_t address, uint32_t& value) {
        const auto it = memory.find(address);
        if (it == memory.end()) return false;
        value = it->second;
        return true;
    };
    unsigned writes = 0;
    const auto write = [&](uint32_t address, uint32_t value) {
        ++writes;
        memory[address] = value;
        return true;
    };
    std::set<uint32_t> scenes;
    for (size_t i = 0; i < kWarpScenes.size(); ++i) {
        assert(scenes.insert(kWarpScenes[i]).second);
        memory[kWarpTable + i * 12] = static_cast<uint32_t>(i);
        memory[kWarpTable + i * 12 + 4] = i == 10 ? 1 : 0;
        memory[kWarpTable + i * 12 + 8] = kWarpScenes[i];
    }
    std::array<uint32_t, 26> flags{};
    assert(ReadWarps(read, flags));
    const auto originalWarps = memory;
    for (size_t i = 0; i < kWarpScenes.size(); ++i) {
        for (bool on : {true, false}) {
            memory = originalWarps;
            uint32_t before;
            writes = 0;
            assert(WriteWarp(read, write, i, on, before));
            assert(writes == 1 && before == (i == 10 ? 1u : 0u));
            auto expected = originalWarps;
            expected[kWarpTable + i * 12 + 4] = on ? 1 : 0;
            assert(memory == expected);
        }
    }
    uint32_t before;
    writes = 0;
    assert(!WriteWarp(read, write, 26, true, before) && writes == 0);
    memory = originalWarps;
    memory[kWarpTable + 4] = 2;
    assert(!WriteWarp(read, write, 1, true, before) && writes == 0);
    memory = originalWarps;
    memory[kWarpTable + 8] = 0;
    assert(!ReadWarps(read, flags));
    memory = originalWarps;
    memory.erase(kWarpTable);
    assert(!ReadWarps(read, flags));
    memory = originalWarps;
    assert(!WriteWarp(read, [](uint32_t, uint32_t) { return false; }, 0, true, before));
    assert(!WriteWarp(read, [](uint32_t, uint32_t) { return true; }, 0, true, before));
    assert(GuestRange(0x80000000, 4));
    assert(GuestRange(0x817FFFFC, 4));
    assert(!GuestRange(0x817FFFFC, 8));
    assert(!GuestRange(0, 4));
    assert(!GuestRange(0xFFFFFFFF, 4));
    {
        constexpr uint32_t list = 0x81300000, player = 0x80245388, other = 0x81310000;
        std::map<uint32_t, uint32_t> indexed;
        memory[list] = memory[list + 4] = player;
        memory[player] = 0x2EA5F237;
        memory[list + 8] = other;
        memory[other] = 0x12345678;
        assert(ReadSceneObjects(read, list, 3, indexed) && indexed.size() == 2);
        assert(indexed.at(0x2EA5F237) == player);
        memory[other] = memory[player];
        assert(!ReadSceneObjects(read, list, 3, indexed));
        memory[list + 8] = 0;
        assert(!ReadSceneObjects(read, list, 3, indexed));
        memory.erase(list + 8);
        assert(!ReadSceneObjects(read, list, 3, indexed));
        assert(!ReadSceneObjects(read, list, 0, indexed));
        assert(!ReadSceneObjects(read, list, 4001, indexed));
        assert(!ReadSceneObjects(read, 0x817FFFFC, 2, indexed));
    }
    using Kind = ProgressCommand::Kind;
    assert(ParseProgressCommand("warps", "").kind == Kind::Warps);
    assert(ParseProgressCommand("warp", "h001 off").warp == 10);
    assert(ParseProgressCommand("warp", "h003 on").enabled);
    assert(ParseProgressCommand("warp", "S002 on").warp == 20);
    assert(ParseProgressCommand("gallery", "").kind == Kind::Gallery);
    assert(ParseProgressCommand("gallery", "pictures locked").rule == GalleryRule::Locked);
    assert(ParseProgressCommand("gallery", "movies natural").section == GallerySection::Movies);
    for (const char* invalid : {"", "H001", "H001 yes", "H001 on extra", "H001 off extra more", "S003 on",
                                "H0010 on", "0x48303031 on", "all on", "H001 ON"})
        assert(ParseProgressCommand("warp", invalid).kind == Kind::Invalid);
    for (const char* invalid : {"all unlocked", "pictures", "movies off", "pictures natural extra", "natural"})
        assert(ParseProgressCommand("gallery", invalid).kind == Kind::Invalid);
    assert(ParseProgressCommand("warps", "all").kind == Kind::Invalid);
    ProgressAvailability available{true, true, false, false, true, false};
    assert(available.Reason() == nullptr);
    available.layout = false;
    assert(available.Reason() != nullptr);
    available.layout = true;
    available.inPlay = false;
    assert(available.Reason() != nullptr);
    available.inPlay = true;
    available.cinematic = true;
    assert(available.Reason() != nullptr);
    available.cinematic = false;
    available.movie = true;
    assert(available.Reason() != nullptr);
    available.movie = false;
    available.control = false;
    assert(available.Reason() != nullptr);
    available.control = true;
    available.transfer = true;
    assert(available.Reason() != nullptr);
    available.transfer = false;
    available.context = false;
    assert(available.Reason() != nullptr);
    {
        memory = originalWarps;
        memory[kPlayerHealth] = 3;
        memory[kPlayerSnacks] = 100;
        memory[kSnackPayment] = memory[kPlayerDying] = 0;
        memory[0x80245190 + 0xCAC] = 17;  // Scene-local, not a world-total counter.
        memory[kPlayerHealth - 4] = memory[kPlayerHealth + 4] = 0xBAD;
        memory[kPlayerSnacks + 4] = 0xBEEF;  // Gadget mask must not change.
        for (uint32_t i = 0; i < kSnackAreaScenes.size(); ++i) {
            const uint32_t address = kSnackAreaTable + i * 36;
            const uint32_t title = 0x81000000 + i * 64;
            memory[address] = title;
            memory[title] = 0x41726561;  // "Area"
            memory[title + 4] = 0;
            memory[address + 4] = i;
            memory[address + 8] = memory[address + 12] = i == 0 ? 999 : 100;
            memory[address + 16] = 1;
            for (size_t j = 0; j < 4; ++j) memory[address + 20 + j * 4] = kSnackAreaScenes[i][j];
            memory[kSnackAreaCounts + i * 0xD0] = 10;
            memory[kSnackAreaCounts + i * 0xD0 - 4] = 0xCAFE;
            memory[kSnackAreaCounts + i * 0xD0 + 4] = 0xFACE;
        }
        memory[0x81000000] = 0;  // Original fallback record has an empty title; it is never selectable.
        const auto playerMemory = memory;
        PlayerStats player;
        assert(ReadPlayerStats(read, player) && !player.Reason() && player.health == 3 && player.snacks == 100);
        for (PlayerStat stat : {PlayerStat::Health, PlayerStat::Snacks}) {
            for (uint32_t value : stat == PlayerStat::Health ? std::array<uint32_t, 3>{1, 3, 5} :
                                                            std::array<uint32_t, 3>{0, 100, kDiagnosticSnackLimit}) {
                memory = playerMemory;
                writes = 0;
                assert(WritePlayerStat(read, write, stat, value, before) && writes == 1);
                assert(before == (stat == PlayerStat::Health ? 3u : 100u));
                auto expected = playerMemory;
                expected[stat == PlayerStat::Health ? kPlayerHealth : kPlayerSnacks] = value;
                assert(memory == expected);
            }
        }
        memory = playerMemory;
        writes = 0;
        for (uint32_t value : {0u, 6u, 0xFFFFFFFFu})
            assert(!WritePlayerStat(read, write, PlayerStat::Health, value, before) && writes == 0);
        assert(!WritePlayerStat(read, write, PlayerStat::Snacks, 100000, before) && writes == 0);
        for (uint32_t field : {kPlayerHealth, kSnackPayment, kPlayerDying}) {
            for (uint32_t value : field == kPlayerHealth ? std::array<uint32_t, 2>{0, 6} :
                                                        std::array<uint32_t, 2>{1, 0xFFFFFFFF}) {
                memory = playerMemory;
                memory[field] = value;
                assert(ReadPlayerStats(read, player) && player.Reason());
                assert(!WritePlayerStat(read, write, PlayerStat::Snacks, 10, before) && writes == 0);
            }
            memory = playerMemory;
            memory.erase(field);
            assert(!WritePlayerStat(read, write, PlayerStat::Health, 5, before) && writes == 0);
        }
        memory = playerMemory;
        assert(!WritePlayerStat(read, [](uint32_t, uint32_t) { return false; }, PlayerStat::Health, 5, before));
        assert(!WritePlayerStat(read, [](uint32_t, uint32_t) { return true; }, PlayerStat::Health, 5, before));
        SnackAreas areas{};
        assert(ReadSnackAreas(read, areas));
        constexpr uint32_t scene = 0x81200000, id = kGalleryScene;
        SnackConfirmation confirmation;
        for (uint32_t index = 1; index < areas.size(); ++index) {
            for (uint32_t value : {0u, 50u, 100u}) {
                memory = playerMemory;
                writes = 0;
                assert(confirmation.Stage(scene, id, index, value, areas));
                assert(confirmation.Matches(scene, id, areas));
                assert(WriteSnackArea(read, write, confirmation, scene, id) && writes == 1);
                auto expected = playerMemory;
                expected[kSnackAreaCounts + index * 0xD0] = value;
                assert(memory == expected);
            }
        }
        memory = playerMemory;
        writes = 0;
        assert(confirmation.Stage(scene, id, 1, 100, areas));
        assert(!WriteSnackArea(read, write, confirmation, scene + 4, id) && writes == 0);
        assert(!WriteSnackArea(read, write, confirmation, scene, 0x48303031) && writes == 0);
        for (uint32_t changed : {kSnackAreaCounts + 0xD0, kSnackAreaTable + 36, kSnackAreaTable + 44}) {
            memory = playerMemory;
            ++memory[changed];
            assert(!WriteSnackArea(read, write, confirmation, scene, id) && writes == 0);
        }
        memory = playerMemory;
        memory[kSnackPayment] = 1;
        assert(!WriteSnackArea(read, write, confirmation, scene, id) && writes == 0);
        memory = playerMemory;
        memory[kPlayerDying] = 1;
        assert(!WriteSnackArea(read, write, confirmation, scene, id) && writes == 0);
        memory = playerMemory;
        confirmation.value = 101;
        assert(!WriteSnackArea(read, write, confirmation, scene, id) && writes == 0);
        assert(!confirmation.Stage(scene, id, 0, 0, areas) && !confirmation.active);
        assert(!confirmation.Stage(scene, id, 30, 0, areas));
        assert(!confirmation.Stage(0, id, 1, 0, areas));
        assert(!confirmation.Stage(scene, 0, 1, 0, areas));
        assert(!confirmation.Stage(scene, id, 1, 101, areas));
        assert(confirmation.Stage(scene, id, 1, 0, areas));
        confirmation.Clear();
        assert(!WriteSnackArea(read, write, confirmation, scene, id) && writes == 0);
        for (const auto& change : std::array<std::pair<uint32_t, uint32_t>, 7>{{
                {kSnackAreaTable + 36, 0x817FFFFF}, {kSnackAreaTable + 40, 2},
                {kSnackAreaTable + 44, 0}, {kSnackAreaTable + 44, 100000},
                {kSnackAreaTable + 56, 0xBAD}, {kSnackAreaCounts + 0xD0, 100000},
                {0x81000040, 0x0A000000}}}) {
            memory = playerMemory;
            memory[change.first] = change.second;
            assert(!ReadSnackAreas(read, areas));
        }
        memory = playerMemory;
        memory.erase(kSnackAreaTable + 36);
        assert(!ReadSnackAreas(read, areas));
        using PlayerKind = PlayerCommand::Kind;
        assert(ParsePlayerCommand("player", "").kind == PlayerKind::Read);
        assert(ParsePlayerCommand("health", "1").kind == PlayerKind::Health);
        assert(ParsePlayerCommand("health", "5").value == 5);
        assert(ParsePlayerCommand("snacks", "0").kind == PlayerKind::Snacks);
        assert(ParsePlayerCommand("snacks", "99999").value == 99999);
        assert(ParsePlayerCommand("snackareas", "").kind == PlayerKind::Areas);
        assert(ParsePlayerCommand("snackarea", "29 100").area == 29);
        assert(ParsePlayerCommand("snackarea", "1 0").kind == PlayerKind::Stage);
        assert(ParsePlayerCommand("snackconfirm", "").kind == PlayerKind::Confirm);
        assert(ParsePlayerCommand("snackcancel", "").kind == PlayerKind::Cancel);
        for (const char* command : {"player", "health", "snacks", "snackareas", "snackarea", "snackconfirm", "snackcancel"}) {
            assert(IsPlayerCommand(command));
            assert(ParsePlayerCommand(command, "1 2 extra").kind == PlayerKind::Invalid);
        }
        for (const char* argument : {"", "0", "6", "-1", "+1", "1.5", "0x3", "3 extra", "4294967296",
                                     "9999999999999999999999999999999999"})
            assert(ParsePlayerCommand("health", argument).kind == PlayerKind::Invalid);
        for (const char* argument : {"-1", "100000", "4294967295", "1e2", "1 more"})
            assert(ParsePlayerCommand("snacks", argument).kind == PlayerKind::Invalid);
        for (const char* argument : {"0 1", "30 1", "1", "1 -1", "1 100000", "1 1 confirm"})
            assert(ParsePlayerCommand("snackarea", argument).kind == PlayerKind::Invalid);
        assert(ParsePlayerCommand("snackconfirm", "yes").kind == PlayerKind::Invalid);
        assert(!IsPlayerCommand("peek"));
    }
    GalleryRules rules;
    rules.Observe(0x81000000, kGalleryScene, 11, true);
    rules.Set(GallerySection::Pictures, GalleryRule::Locked);
    rules.Set(GallerySection::Movies, GalleryRule::Unlocked);
    rules.Observe(0x81000000, kGalleryScene, 7, true);
    assert(rules.Get(GallerySection::Pictures) == GalleryRule::Locked);
    assert(rules.Get(GallerySection::Movies) == GalleryRule::Unlocked);
    rules.Observe(0x81000000, kGalleryScene, 4, true);
    assert(rules.Get(GallerySection::Movies) == GalleryRule::Natural);
    rules.Set(GallerySection::Movies, GalleryRule::Locked);
    rules.Observe(0x81000001, kGalleryScene, 11, true);
    assert(rules.Get(GallerySection::Movies) == GalleryRule::Natural);
    rules.Set(GallerySection::Pictures, GalleryRule::Unlocked);
    rules.Observe(0x81000001, kGalleryScene, 11, false);
    assert(rules.Get(GallerySection::Pictures) == GalleryRule::Natural);
    rules.Set(GallerySection::Pictures, GalleryRule::Locked);
    rules.Observe(0x81000001, 0x48303031, 11, true);
    assert(rules.Get(GallerySection::Pictures) == GalleryRule::Natural);

    memory.clear();
    std::map<uint32_t, uint32_t> objects;
    const auto find = [&](uint32_t id) {
        const auto it = objects.find(id);
        return it == objects.end() ? 0u : it->second;
    };
    const auto addTarget = [&](uint32_t id) {
        if (objects.count(id)) return;
        const uint32_t base = 0x81200000 + static_cast<uint32_t>(objects.size()) * 32;
        objects[id] = base;
        memory[base] = id;
    };
    std::set<uint32_t> conditionIds;
    for (size_t i = 0; i < kGalleryConditionCount; ++i) {
        const auto spec = ConditionAt(i);
        assert(conditionIds.insert(spec.id).second);
        const uint32_t base = 0x81000000 + static_cast<uint32_t>(i) * 32;
        const uint32_t asset = 0x81100000 + static_cast<uint32_t>(i) * 128;
        objects[spec.id] = base;
        memory[base] = spec.id;
        memory[base + 4] = 0x1F02000D;
        memory[base + 8] = asset + 20;
        memory[base + 12] = kConditionCallback;
        memory[base + 16] = asset;
        const uint32_t words[] = {spec.id, 0x1F02000D, spec.threshold, spec.variable, spec.operation};
        for (uint32_t w = 0; w < 5; ++w) memory[asset + w * 4] = words[w];
        for (uint32_t link = 0; link < 2; ++link) {
            const uint32_t address = asset + 20 + link * 32;
            memory[address] = (link ? 0x3E0000 : 0x3D0000) | (link ? spec.noEvent : spec.yesEvent);
            memory[address + 4] = link ? spec.noTarget : spec.yesTarget;
            for (uint32_t offset = 8; offset < 32; offset += 4) memory[address + offset] = 0;
            if (link == 0) {
                memory[address + 8] = spec.firstParameter;
                memory[address + 12] = spec.secondParameter;
            }
            addTarget(link ? spec.noTarget : spec.yesTarget);
        }
    }
    assert(!conditionIds.count(0xB8ED42A4)); // The stock group references this absent condition.
    const auto validMemory = memory;
    for (size_t i = 0; i < kGalleryConditionCount; ++i) {
        const auto spec = ConditionAt(i);
        uint32_t base = 0;
        assert(ValidateCondition(read, find, spec, base));
        const uint32_t asset = memory[base + 16];
        for (uint32_t address : {base, base + 4, base + 8, base + 12, base + 16, asset, asset + 4,
                                 asset + 8, asset + 12, asset + 16, asset + 20, asset + 24,
                                 asset + 28, asset + 52, asset + 56, asset + 80}) {
            memory[address] ^= 0x80000000u;
            assert(!ValidateCondition(read, find, spec, base));
            memory = validMemory;
        }
        assert(!ValidateCondition(read, [](uint32_t) { return 0u; }, spec, base));
    }
    assert(kMovieEntries[10].display == 0xCE2336FC && kMovieEntries[10].playback == 0x31F85F30);
    assert(kMovieEntries[12].display == 0xD71AA016 && kMovieEntries[12].playback == 0xFFA18A82);
    assert(kMovieEntries[10].playbackArgument == 0x41500000);
    assert(kMovieEntries[12].playbackArgument == 0x41300000);
    unsigned visibilityCount = 0;
    for (size_t i = 0; i < kGalleryConditionCount; ++i) {
        const auto spec = ConditionAt(i);
        if (spec.visibility) {
            ++visibilityCount;
            assert(spec.yesEvent != 0x126 && spec.noEvent != 0x126);
        }
    }
    assert(visibilityCount == 14);
    for (auto rule : {GalleryRule::Natural, GalleryRule::Locked, GalleryRule::Unlocked}) {
        for (bool natural : {true, false}) {
            for (uint32_t event : {0x3Cu, 0xAu, 0u, 0x126u}) {
                unsigned evaluations = 0, resets = 0, dispatches = 0;
                const uint32_t returned = ConditionalEvent(0x81000000, event, rule,
                    [&](uint32_t target) { assert(target == 0x81000000); ++evaluations; return natural; },
                    [&](uint32_t target) { assert(target == 0x81000000); ++resets; },
                    [&](uint32_t target, uint32_t result) {
                        assert(target == 0x81000000);
                        const bool passed = rule == GalleryRule::Natural ? natural : rule == GalleryRule::Unlocked;
                        assert(result == (passed ? 0x3Du : 0x3Eu));
                        ++dispatches;
                    });
                assert(returned == 1);
                assert(evaluations == (event == 0x3C && rule == GalleryRule::Natural ? 1u : 0u));
                assert(resets == (event == 0xA ? 1u : 0u));
                assert(dispatches == (event == 0x3C ? 1u : 0u));
            }
        }
    }
    constexpr const char* labels[] = {
        "Map", "Shovel", "Springs", "Helmet", "Lamp", "Slippers", "Knight Armor", "Galoshes (Boots)", "Super Smash",
        "Super Sonic Smash (Stun)", "Umbrella", "Plunger Boots", "Bubblegum", "Soap Bubbles",
        "Flower Pot", "Diving Helmet",
    };
    constexpr const char* keysByBit[] = {
        "BubbleGum", "SoapBar", "StickyBoots", "Plungers", "Slippers", "LampShade", "BlackKnight",
        "FlowerPot", "DivingHelmet", "Spring", "LightningBolt", "FootballHelmet", "Umbrella", "Shovel",
        "LightningBoltStun",
    };
    static_assert(std::size(kPowerupList) == std::size(labels));
    static_assert(kAllPowerups == 0x7FFFu);
    static_assert(kMainGamePowerups == 0x7E7Fu);
    assert(std::string_view(kUnusedPowerupHeading) == "--UNUSED--");
    uint32_t seen = 0;
    for (size_t i = 0; i < std::size(kPowerupList); ++i) {
        const auto& power = kPowerupList[i];
        assert(std::string_view(power.label) == labels[i]);
        assert(power.unused == (i >= 14));
        if (power.bit == 0) {
            assert(i == 0 && std::string_view(power.key) == "Map");
            assert(!PowerupEnabled(power, 0));
            assert(PowerupEnabled(power, 1));
            assert(WithPowerup(power, 0, true) == 1);
            assert(WithPowerup(power, 1, false) == 0);
            continue;
        }
        assert((seen & power.bit) == 0);
        seen |= power.bit;
        unsigned bit = 0;
        while ((1u << bit) != power.bit) {
            ++bit;
            assert(bit < std::size(keysByBit));
        }
        assert(std::string_view(power.key) == keysByBit[bit]);
        for (uint32_t current : {0u, 0x80000000u, 0xFFFFFFFFu, 0xA5A51234u}) {
            const uint32_t enabled = WithPowerup(power, current, true);
            const uint32_t disabled = WithPowerup(power, current, false);
            assert(PowerupEnabled(power, enabled));
            assert(!PowerupEnabled(power, disabled));
            assert((enabled & ~power.bit) == (current & ~power.bit));
            assert((disabled & ~power.bit) == (current & ~power.bit));
        }
    }
    assert(seen == kAllPowerups);
    constexpr uint32_t unused = kAllPowerups & ~kMainGamePowerups;
    assert(WithPowerupBits(0x80000000u, kMainGamePowerups, kMainGamePowerups) == 0x80007E7Fu);
    assert((WithPowerupBits(unused, kMainGamePowerups, kMainGamePowerups) & unused) == unused);
    assert(WithPowerupBits(0xFFFFFFFFu, kAllPowerups, 0) == 0xFFFF8000u);
    assert(WithPowerupBits(0x80000000u, kAllPowerups, 0xFFFFFFFFu) == 0x80007FFFu);

    using no100f_devmenu::SkipControl;
    assert(no100f_devmenu::SkipControlFor(2, 0, false) == SkipControl::Start);
    assert(no100f_devmenu::SkipControlFor(2, 1, false) == SkipControl::Start);
    assert(no100f_devmenu::SkipControlFor(2, 2, false) == SkipControl::None);
    assert(no100f_devmenu::SkipControlFor(11, 1, true) == SkipControl::Confirm);
    assert(no100f_devmenu::SkipControlFor(11, 1, false) == SkipControl::None);
    using no100f_devmenu::SceneRequest;
    using Action = SceneRequest::Action;
    SceneRequest travel;
    assert(!travel.Begin(0, 100));
    assert(travel.Begin(2, 100));
    assert(!travel.Begin(3, 200));
    assert(travel.Update(1, false, false, false, 300) == Action::Wait);
    assert(travel.Update(1, true, true, false, 400) == Action::Wait);
    assert(travel.Update(1, true, false, true, 500) == Action::Wait);
    assert(travel.Update(1, true, false, false, 600) == Action::Submit);
    assert(travel.Submitted());
    assert(travel.Update(1, true, false, false, 700) == Action::Wait);
    assert(travel.Update(2, false, false, false, 800) == Action::Wait);
    assert(travel.Update(2, true, true, false, 900) == Action::Arrived);
    assert(!travel.Active());
    assert(!travel.Submitted());
    assert(travel.Begin(3, 1000));
    assert(travel.Update(2, false, false, false, 61000) == Action::Timeout);
    assert(!travel.Active());
    assert(travel.Begin(3, 62000));
    travel.Cancel();
    assert(!travel.Active());

    no100f_devmenu::SkipRequest skip;
    assert(!skip.Press(0, true));
    skip.Begin(100);
    assert(skip.Press(100, true));
    assert(!skip.Press(200, true));
    assert(skip.Press(600, true));
    assert(!skip.Press(601, false));
    assert(!skip.Active());
    assert(!skip.Press(700, true));
    skip.Begin(1000);
    assert(skip.Press(10499, true) == false);
    assert(!skip.Press(11000, true));
    assert(!skip.Active());
}
