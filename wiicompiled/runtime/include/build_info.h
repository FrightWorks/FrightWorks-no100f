#pragma once

// What this build was made from, generated on every build by runtime/cmake/build_info.cmake.
namespace BuildInfo {
// FrightWorks version: <Version> in launcher/FrightWorks.Launcher.csproj ("1.0.3").
const char* Version() noexcept;
// Git branch ("detached" for a detached HEAD), short commit, and a 4-digit hash of the uncommitted changes to tracked
// files ("" when there are none). All "" when git wasn't found at build time.
const char* Branch() noexcept;
const char* Commit() noexcept;
const char* Changes() noexcept;
// A clean checkout at the tag v<Version>.
bool Release() noexcept;
// One line for logs, the F10 footer and the DEV mark: "1.0.3" for a release, else "1.0.3 feat/x@1a2b3c4[+9f0e]".
const char* Id() noexcept;
} // namespace BuildInfo
