// Bloom's light textures (see no100f_light_textures.h).
//
// Finding them (dev tools on): NO100F_TEXLOG=1 logs every texture a level draws to devmenu_texlog.csv; "lighttex add KEY"
// makes a texture glow for this run and "bloomview 2" shows what glows, so a candidate can be checked in place.
// The list covers every scene's textures as drawn around its start spot; a light seen only deeper in a level may still
// be missing. Light that falls on floors and walls (pools, patches) and the game's own glow sprites are left out.

#include "no100f_light_textures.h"

#include <aurora/aurora.h>

namespace no100f_light_textures {

std::vector<std::string>& List() {
    // clang-format off
    static std::vector<std::string> list{
        // Lit windows. Mystic Manor (h001): the amber windows, each with its own shadow
        "tex1_64x64_039dbf109f1f5351_14",
        "tex1_64x64_03ac8f758bfe64ff_14",
        "tex1_64x64_06d7213b1d677f8c_14",
        "tex1_64x64_16fa0c013e08fbf7_14",
        "tex1_64x64_263183eb62dcb47b_14",
        "tex1_64x64_265e5ef0be53f3d0_14",
        "tex1_64x64_26fdbb462e6ab5ac_14",
        "tex1_64x64_2deb9c97151a9aea_14",
        "tex1_64x64_4fd81838eb4d6d3d_14",
        "tex1_64x64_5ae22b5394419bcc_14",
        "tex1_64x64_6d4aecd06af4f398_14",
        "tex1_64x64_919f789599a9d20f_14",
        "tex1_64x64_b2503da9b07563ca_14",
        "tex1_64x64_b43b3f56e8e3d83a_14",
        "tex1_64x64_c80b94207c1cf326_14",
        "tex1_64x64_ccc19960e985de0c_14",
        "tex1_64x64_e140fc72dfd43dc3_14",
        "tex1_64x64_fa77100f9852893a_14",
        "tex1_64x64_ff69b5dc1353daff_14",
        // Shock on the Dock! (f001): amber, green, the yellow lantern panes
        "tex1_128x128_1fddcbd5a7c325d2_14",
        "tex1_128x128_2571af3644f71609_14",
        "tex1_128x128_62810c7ff2420d90_14",
        "tex1_128x128_bf047e1f8627a5ca_14",
        "tex1_64x64_09c19f777c29855f_14",
        // Mind Your Manors! (i004): green, the one beside the curtain
        "tex1_128x128_42db7d1064ba0537_14",
        "tex1_128x128_7df19363e0d095aa_14",
        // Lamps: the lantern (h001), the cyan lamps (h001, e001), the f001 street lamp's panel, cone and flare
        "tex1_64x64_fb59c4ebbdd03e0a_14",
        "tex1_64x64_f3267f25b29cb776_14",
        "tex1_64x64_c7087284c34e10c4_14",
        "tex1_64x64_3cb3df5fae4f0b3f_14",
        "tex1_64x64_c8e8b71a2d8d8a88_14",
        // Fire: the wall torches (i004)
        "tex1_64x64_cb2bf47bede85807_5",
        // The moon (h001), the sparkle stars (save points, pickups)
        "tex1_64x64_1b4b0b5ce7d49c44_5",
        "tex1_128x128_9ef4f6af9813cb73_4",
        // The hubs (h001, h003): the manor door's amber panes, the gallery door's green panes
        "tex1_64x64_22ff9865563143c1_14",
        "tex1_64x64_3088946fe4242460_14",
        // Hedge Maze (e004): the distant manor's windows
        "tex1_128x128_21c8bcb68d8ff3d1_5",
        // Cliffs & Caves: the lamp panes and hanging lanterns
        "tex1_64x64_05bb130cf46ba502_14",
        "tex1_64x64_fc5d11a0fe3e03cb_14",
        // Cemetery: the moon, eyes in the dark, candle flames, lit windows, the carriage lamps
        "tex1_128x128_76e74cfb2916fef5_5",
        "tex1_128x128_fe2585fe70c436cf_14",
        "tex1_32x32_a650863940a77135_6",
        "tex1_64x32_4a217423ddfc5ed2_6",
        "tex1_64x32_a0b3d1bdd4127459_6",
        "tex1_64x32_a3261bbbc8bc628b_6",
        "tex1_64x64_cf72ff1c834a50eb_14",
        // Fishing Village: the green transom panes, the moon, a distant lit window, the lamp cone
        "tex1_128x128_172d382a6dd8d4fc_14",
        "tex1_128x128_d6ba57b490a6b3ff_5",
        "tex1_64x64_58d391f171e57404_14",
        "tex1_64x64_dffe45d00e9c2a2e_0000000000000000_9",
        "tex1_64x64_dffe45d00e9c2a2e_ffc0b3ac2f1d3f94_9",
        // Lighthouse: lit windows, a lantern
        "tex1_64x64_5c52a6660f3df552_14",
        "tex1_64x64_61f54f3deb9962a9_14",
        "tex1_64x64_7bc0a2d68a7fd0d5_14",
        "tex1_64x64_8dd9ba2585bff664_14",
        // Shipwreck: portholes, cabin windows, lanterns, ceiling lamps, a gauge panel's lights
        "tex1_64x64_292122af1a8f98a4_14",
        "tex1_64x64_321fb6883e57fe50_14",
        "tex1_64x64_3e6c648bb75d9c65_14",
        "tex1_64x64_4bc5fcce6a690e55_14",
        "tex1_64x64_74689ee60760c5f5_14",
        "tex1_64x64_7f831f926ff91063_14",
        "tex1_64x64_827e2b6b7a1312ea_14",
        "tex1_64x64_8d0ab8cedbcb1dd5_14",
        "tex1_64x64_e31a593a71301a3e_14",
        // The manor's 1st floor: green and teal windows, the ceiling lamps and their light cones, indicator lights
        "tex1_128x128_1ee5698aeb876dc3_14",
        "tex1_128x128_7306ee5e95ffaee7_14",
        "tex1_128x128_981acece1456561b_14",
        "tex1_64x64_4c5537c4192b4dde_4",
        "tex1_64x64_4e067d5a9aa28a5a_14",
        "tex1_64x64_d9f463c963845da9_5",
        // Attic & Balconies: green windows, lamps and lanterns, amber windows
        "tex1_128x128_01b5bf42de30a9b9_14",
        "tex1_128x128_916b50f1df64b41c_14",
        "tex1_64x64_13a11e6d66c979e2_14",
        "tex1_64x64_47636866f006e2d3_14",
        "tex1_64x64_58bb6960dd27e107_14",
        "tex1_64x64_6a7535e4528c4bfe_14",
        "tex1_64x64_709526245d8f6756_14",
        "tex1_64x64_f5624d95806e2e1d_14",
        // Tower: amber windows, green and stained-glass windows
        "tex1_128x128_1ba15208011d021f_14",
        "tex1_128x128_2c9c696b04ca05ce_14",
        "tex1_128x128_91cc7d64411c56dc_14",
        "tex1_128x128_9214849788c7240d_14",
        "tex1_128x128_a3ad5e4e15059a5c_14",
        "tex1_128x128_c38dc987aa8df133_14",
        "tex1_128x128_edf0bd35548132b9_14",
        "tex1_128x128_f177345ff03e0a2c_14",
        "tex1_64x64_7eb8826642fd3ce5_14",
        "tex1_64x64_acbbfdc77e512a60_14",
        "tex1_64x64_d2204ad814b3921d_14",
        "tex1_64x64_e4a085569a608e46_14",
        // Cellars: fire, a lantern
        "tex1_64x64_3877fb08fb60da37_5",
        "tex1_64x64_89e80b7b78832e3e_14",
        // Super Secret Lab: ceiling lamps, screens, indicator panels, a skylight, the moon
        "tex1_128x128_02c924ddd3009a95_5",
        "tex1_128x128_147aaf6a2b96e533_14",
        "tex1_128x128_8998f4bc1edc20d2_14",
        "tex1_128x128_ada5cb63c7572de5_14",
        "tex1_64x64_1c824ff28e4b421c_14",
        "tex1_64x64_1f1ef8225c0ecd46_14",
        "tex1_64x64_212b60098bf5de2a_14",
        "tex1_64x64_2197ad2894c0a1fc_14",
        "tex1_64x64_2890e859aeff2762_14",
        "tex1_64x64_353bbefe3b67e38f_14",
        "tex1_64x64_4770d93840b82312_14",
        "tex1_64x64_5e982ab313cc5e50_14",
        "tex1_64x64_6a03a6d5fde9a63b_14",
        "tex1_64x64_ae1afe8b45774ac2_14",
        "tex1_64x64_b4d9f98330a413cc_14",
        "tex1_64x64_d82a6f119a655ea6_14",
    };
    // clang-format on
    return list;
}

void Apply() {
#ifdef AURORA_HAS_BLOOM
    std::vector<const char*> keys;
    for (const auto& key : List()) keys.push_back(key.c_str());
    aurora_set_light_textures(keys.data(), static_cast<uint32_t>(keys.size()));
#endif
}

}  // namespace no100f_light_textures
