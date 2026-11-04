#pragma once

#include <iterator>
#include <string>

// Who made FrightWorks, as the game shows it (F10 About, the pill on the game's credits).
namespace no100f_credits {

inline constexpr const char* kAuthor = "Loneranger419";
// Everyone who helped, in the order they joined: add new names at the end. The credits pill lists each one on its own
// line, under the first.
inline constexpr const char* kHelpers[] = {"BitsofBitsofBits"};

// "BitsofBitsofBits", "A and B", "A, B and C".
inline std::string HelperList() {
    std::string text;
    const size_t count = std::size(kHelpers);
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) text += i + 1 == count ? " and " : ", ";
        text += kHelpers[i];
    }
    return text;
}

// "Made by Loneranger419 with the help of BitsofBitsofBits"
inline std::string Line() { return std::string("Made by ") + kAuthor + " with the help of " + HelperList(); }

} // namespace no100f_credits
