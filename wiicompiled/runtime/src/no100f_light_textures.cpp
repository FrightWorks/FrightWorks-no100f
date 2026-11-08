// Bloom's light textures (see no100f_light_textures.h).
//
// Finding them (dev tools on): NO100F_TEXLOG=1 logs every texture a level draws to devmenu_texlog.csv; "lighttex add KEY"
// makes a texture glow for this run and "bloomview 2" shows what glows, so a candidate can be checked in place.

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
