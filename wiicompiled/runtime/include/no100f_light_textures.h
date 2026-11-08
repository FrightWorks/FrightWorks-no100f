#pragma once

#include <string>
#include <vector>

// Bloom ([video] bloom): the game's light textures (lit windows, fire, lamp light cones), whose draws glow. Nothing in
// how the game draws marks its lights, so they are listed by texture pack name in no100f_light_textures.cpp.
namespace no100f_light_textures {
// Hands Aurora the list (at start, and after a change).
void Apply();

// Dev tools ("lighttex"): the list for this run, editable; Apply after a change.
std::vector<std::string>& List();
} // namespace no100f_light_textures
