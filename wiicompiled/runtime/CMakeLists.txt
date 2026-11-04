cmake_minimum_required(VERSION 3.25)

# Dawn's pinned macOS artifacts target 12.0. Set the same floor before project()
# initializes the Apple toolchain so direct developer CMake invocations cannot
# accidentally inherit the running SDK's deployment version. This cache entry
# is harmless on non-Apple platforms and remains overridable by a caller.
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING
        "Minimum macOS version supported by WiiCompiled" FORCE)
endif()
project(recomp_runtime)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "WiiCompiled requires a 64-bit Clang toolchain")
endif()

if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES RECOMP_OSX_ARCHITECTURE_COUNT)
    if(RECOMP_OSX_ARCHITECTURE_COUNT GREATER 1)
        message(FATAL_ERROR
            "WiiCompiled supports one macOS architecture per build directory; "
            "configure separate arm64 and x86_64 build directories")
    endif()
    list(GET CMAKE_OSX_ARCHITECTURES 0 _recomp_osx_arch)
    set(CMAKE_SYSTEM_PROCESSOR "${_recomp_osx_arch}" CACHE STRING "Target processor architecture" FORCE)
    set(CMAKE_SYSTEM_PROCESSOR "${_recomp_osx_arch}")
endif()

if(WIN32 AND MINGW AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
    set(RECOMP_PLATFORM_WINDOWS TRUE)
elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64|arm64|ARM64)$")
    set(RECOMP_PLATFORM_MACOS TRUE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
        set(RECOMP_PLATFORM_MACOS_X86_64 TRUE)
    else()
        set(RECOMP_PLATFORM_MACOS_ARM64 TRUE)
    endif()
elseif(ANDROID AND CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    # Cross-built with the NDK. The game is a shared library (libmain.so) that SDL's Android
    # activity loads; see cmake/PublicProducts.cmake.
    set(RECOMP_PLATFORM_ANDROID TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64|aarch64|arm64|ARM64)$")
    set(RECOMP_PLATFORM_LINUX TRUE)
else()
    message(FATAL_ERROR
        "WiiCompiled supports 64-bit LLVM-MinGW Clang on Windows, native Linux x86_64/aarch64, "
        "Apple Clang on macOS x86_64/arm64, or the NDK for Android arm64-v8a")
endif()

if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "WiiCompiled only supports Release builds")
endif()

option(RECOMP_BUILD_PRODUCTS "Build translated WiiCompiled product targets" ON)
option(RECOMP_BUILD_PSQ_TESTS "Build focused PSQ ISA tests" OFF)

# Preprocessor definitions that belong to this project's own code (the runtime,
# the translated shards and the product glue) and to nothing else. They are
# applied directory-scoped, immediately after the aurora add_subdirectory() call,
# so every target created afterwards (recomp_runtime_common, the translated shard
# libraries and the products in cmake/PublicProducts.cmake) inherits them while
# aurora-main and its third-party tree stay unaffected.
set(RECOMP_PROJECT_COMPILE_DEFINITIONS NOMINMAX)

set(CMAKE_CXX_STANDARD 17)

# Declared before the vendored libraries so the third-party block below can
# branch on it and so cmake/NativePrebuilt.cmake is included after the names it
# must provide are known.
set(RECOMP_NATIVE_PREBUILT_DIR "" CACHE PATH
    "Precompiled aurora/third-party package to link instead of building aurora-main")
# Maintainer-only counterpart: describe the configured aurora build so the
# package can be harvested from it.
set(RECOMP_NATIVE_PREBUILT_EXPORT_DIR "" CACHE PATH
    "Maintainer-only: write a description of the configured aurora build here")

# The two Crypto++ configuration macros are load-bearing for ABI: they shape the
# structs and inline code the headers generate, so a first-party TU including
# <cryptopp/...> must see exactly what the archive was compiled with. Both the
# from-source target below and the prebuilt import in cmake/NativePrebuilt.cmake
# consume this one definition.
set(RECOMP_CRYPTOPP_INTERFACE_DEFINITIONS CRYPTOPP_DISABLE_ASM CRYPTOPP_NO_GLOBAL_BYTE)

# Riivolution XML is parsed with pugixml, matching the parser used by Dolphin.
# The four upstream files from v1.15 (ee86beb30e4973f5feffe3ce63bfa4fbadf72f38)
# are vendored so builds remain usable with FETCHCONTENT_FULLY_DISCONNECTED.
add_library(recomp_pugixml STATIC third_party/pugixml/pugixml.cpp)
add_library(recomp::pugixml ALIAS recomp_pugixml)
target_include_directories(recomp_pugixml PUBLIC third_party/pugixml)
target_compile_features(recomp_pugixml PUBLIC cxx_std_17)
set_target_properties(recomp_pugixml PROPERTIES UNITY_BUILD OFF)

# POSIX x86-64 guest-fiber scheduling (runtime/src/host_context.cpp) needs a symmetric
# stackful-coroutine primitive to stand in for Win32 Fibers. libco's co_switch() transfers
# directly to any other created coroutine, matching SwitchToFiber's semantics exactly (unlike
# asymmetric resume/yield coroutine libraries, which would need every call site restructured).
# Vendored from upstream (higan-emu/libco @ e18e09d, 2019-10-16, ISC license; valgrind.h is
# separately BSD-style licensed, see third_party/libco/LICENSE). Windows keeps native Fibers;
# Apple Silicon uses the project's x18-safe AArch64 assembly backend, while Intel macOS uses
# libco's existing System V AMD64 backend.
if(RECOMP_PLATFORM_LINUX OR RECOMP_PLATFORM_MACOS_X86_64)
    add_library(recomp_libco STATIC third_party/libco/libco.c)
    add_library(recomp::libco ALIAS recomp_libco)
    target_include_directories(recomp_libco PUBLIC third_party/libco)
    set_target_properties(recomp_libco PROPERTIES UNITY_BUILD OFF)
endif()

# Runtime configuration is real TOML, parsed by toml11 rather than a project-
# specific line parser. Keep it header-only and vendored so disconnected release
# builds have exactly the same parser as developer builds.
add_library(recomp_toml11 INTERFACE)
add_library(recomp::toml11 ALIAS recomp_toml11)
target_include_directories(recomp_toml11 SYSTEM INTERFACE third_party/toml11)
target_compile_features(recomp_toml11 INTERFACE cxx_std_17)

# Wii ES signatures use the standard binary-field sect233r1 curve. Crypto++
# supplies that curve, SHA-1, ECDSA and an OS-seeded CSPRNG. The complete 8.9.0
# source distribution is vendored. Assembly and algorithm-specific SIMD units
# are disabled to keep the library portable and independent of the runtime's
# x86-64-v3 policy; the static linker retains only the algorithms we reference.
#
# Crypto++ is by far the most expensive vendored library to compile (~150
# translation units), and under the shipped fixed toolchain it is bit-for-bit
# identical for every user, so the prebuilt package harvests its archive and
# this block is skipped entirely in that mode (see cmake/NativePrebuilt.cmake).
if(NOT RECOMP_NATIVE_PREBUILT_DIR)
    file(GLOB RECOMP_CRYPTOPP_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/third_party/cryptopp/*.cpp")
    list(FILTER RECOMP_CRYPTOPP_SOURCES EXCLUDE REGEX
        "/(adhoc|bench[123]|datatest|dlltest|fipsalgt|fipstest|pch|regtest[1234]|simple|test|validat[0-9]+)\\.cpp$")
    list(FILTER RECOMP_CRYPTOPP_SOURCES EXCLUDE REGEX
        "(_avx|_ppc|_simd|_sse)\\.cpp$")
    add_library(recomp_cryptopp STATIC ${RECOMP_CRYPTOPP_SOURCES})
    add_library(recomp::cryptopp ALIAS recomp_cryptopp)
    target_include_directories(recomp_cryptopp SYSTEM PUBLIC third_party)
    target_compile_features(recomp_cryptopp PUBLIC cxx_std_17)
    target_compile_definitions(recomp_cryptopp PUBLIC ${RECOMP_CRYPTOPP_INTERFACE_DEFINITIONS})
    target_compile_options(recomp_cryptopp PRIVATE -w)
    set_target_properties(recomp_cryptopp PROPERTIES UNITY_BUILD OFF)
    if(RECOMP_PLATFORM_ANDROID)
        # On Android Crypto++ asks the NDK's cpufeatures helper which ARM extensions exist.
        set(RECOMP_NDK_CPUFEATURES_DIR "${ANDROID_NDK}/sources/android/cpufeatures")
        target_sources(recomp_cryptopp PRIVATE "${RECOMP_NDK_CPUFEATURES_DIR}/cpu-features.c")
        target_include_directories(recomp_cryptopp PRIVATE "${RECOMP_NDK_CPUFEATURES_DIR}")
    endif()
endif()

# TLS for non-Windows guest network HLE (runtime/src/hle/net/network_ssl.cpp) - the Windows path
# uses Schannel (a Windows-only OS API), which has no equivalent on Linux/Android, so this project
# needs its own TLS library there. mbed TLS was chosen over OpenSSL specifically because it cross-
# compiles cleanly for Android with nothing beyond a plain C toolchain (no perl/asm build-script
# dependency the way OpenSSL's build has), matching how this project already prefers toolchain-
# simple libraries (see Crypto++ above, similarly stripped of ASM/SIMD for portability).
# The from-source build fetches a pinned upstream tarball with a checked SHA-256.
# The Linux native prebuilt package instead ships its compiled archives and headers.
# Bump both values in cmake/MbedTLSPin.cmake when updating; the hash comes from
# upstream's signed `mbedtls-<version>-sha256sum.txt` release asset.
#
# The alias exists on every platform so the link lines in cmake/PublicProducts.cmake stay
# platform-independent, but it is only populated where network_ssl.cpp actually compiles the mbed
# TLS path (`#ifndef _WIN32`). Windows keeps Schannel and must not fetch anything: its builds run
# with FETCHCONTENT_FULLY_DISCONNECTED=ON against the offline dependency set prepared by
# Launcher/Prepare-Dependencies.ps1, so an unconditional fetch would fail a clean configure there
# and would also add a dependency Windows never links.
add_library(recomp_mbedtls INTERFACE)
add_library(recomp::mbedtls ALIAS recomp_mbedtls)
if(NOT RECOMP_PLATFORM_WINDOWS)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/MbedTLSPin.cmake")
    if(NOT RECOMP_NATIVE_PREBUILT_DIR)
        include(FetchContent)
        FetchContent_Declare(recomp_mbedtls_upstream
            URL "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${RECOMP_MBEDTLS_VERSION}/mbedtls-${RECOMP_MBEDTLS_VERSION}.tar.bz2"
            URL_HASH SHA256=${RECOMP_MBEDTLS_SHA256}
            DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        )
        # Subproject mode already disables testing and codegen; it still enables programs.
        set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
        set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(recomp_mbedtls_upstream)
        target_link_libraries(recomp_mbedtls INTERFACE MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
    endif()
endif()

set(RECOMP_TRANSLATED_COMPILE_JOBS 0 CACHE STRING
    "Cap on concurrently compiling translated shard TUs via a Ninja job pool (0 = uncapped). \
Scheduling only - never affects output bytes, so it is deliberately outside the canonical flag fingerprint.")
set(RECOMP_CPPWINRT_INCLUDE_DIR "" CACHE PATH "Optional self-contained C++/WinRT include directory")

# Precompiled aurora + third-party package (see launcher/Prepare-NativePrebuilt.ps1).
# Empty - the default - keeps the from-source behaviour every developer build
# uses. When set, aurora-main, its extern/ tree and the vendored Crypto++ are
# not compiled at all; imported targets with the same names are wired up from
# the package instead, so this project's own code compiles and links exactly
# as before.
set(RECOMP_AURORA_DIR "${CMAKE_CURRENT_LIST_DIR}/../aurora-main")

# The translator's explicit float casts are PPC single-precision rounding points.
# Fast-math may erase them and change guest-visible integer conversions.
set(RECOMP_TRANSLATED_PPC_FP_OPTIONS -fno-fast-math -ffp-contract=off)

# kpad.cpp derives the accelerometer values the game reads (magnitude, speed, acc_vertical) in
# native float math; network play needs them bit-identical on x86 and ARM, as the translated
# code is.
set(RECOMP_PPC_SEMANTIC_RUNTIME_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/src/ppc_helpers.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/fpu_helpers.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/ppc_quantized.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/hle/input/kpad.cpp")
set_source_files_properties(${RECOMP_PPC_SEMANTIC_RUNTIME_SOURCES} PROPERTIES
    SKIP_UNITY_BUILD_INCLUSION ON
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_OPTIONS "${RECOMP_TRANSLATED_PPC_FP_OPTIONS}")
# The sampling profiler includes <windows.h>; keep it out of the unity batches.
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/src/guest_profiler.cpp" PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
# Map guards must reject guest NaN/Inf before constructing backdrop geometry.
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/src/hle/no100f_map_backdrop.cpp" PROPERTIES
    SKIP_UNITY_BUILD_INCLUSION ON
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_OPTIONS "-fno-finite-math-only")
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/src/no100f_resources.cpp" PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
# Match the optimization policy of the translated callers as well as their FP policy.
set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/ppc_quantized.cpp" APPEND PROPERTY
    COMPILE_OPTIONS -O2 -fno-slp-vectorize)

# ----------------------------------------------------------------------
# Third-party: aurora-main (provides SDL3 + GPU backends)
# ----------------------------------------------------------------------
if(RECOMP_NATIVE_PREBUILT_DIR)
    # Precompiled aurora + third-party archives. The source trees are still
    # present (the runtime includes aurora headers directly), they are simply
    # not compiled here.
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/NativePrebuilt.cmake")
else()
    if(NOT EXISTS ${RECOMP_AURORA_DIR}/CMakeLists.txt)
        message(FATAL_ERROR "Requested aurora-main but ${RECOMP_AURORA_DIR} is missing")
    endif()
    set(DAWN_ENABLE_D3D11 OFF CACHE BOOL "" FORCE)
    if(RECOMP_PLATFORM_WINDOWS)
        set(DAWN_ENABLE_D3D12 ON CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_METAL OFF CACHE BOOL "" FORCE)
        set(TINT_BUILD_HLSL_WRITER ON CACHE BOOL "" FORCE)
    elseif(RECOMP_PLATFORM_MACOS)
        set(DAWN_ENABLE_D3D12 OFF CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_VULKAN OFF CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_METAL ON CACHE BOOL "" FORCE)
        set(TINT_BUILD_HLSL_WRITER OFF CACHE BOOL "" FORCE)
    else()
        set(DAWN_ENABLE_D3D12 OFF CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_METAL OFF CACHE BOOL "" FORCE)
        set(TINT_BUILD_HLSL_WRITER OFF CACHE BOOL "" FORCE)
    endif()
    set(DAWN_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    set(DAWN_BUILD_TESTS OFF CACHE BOOL "" FORCE)

    # Provide a tiny stub for DXProgrammableCapture when the SDK/PIX headers are
    # missing (common on MinGW). Dawn only includes the header; no symbols are
    # referenced when PIX isn't present.
    if(RECOMP_PLATFORM_WINDOWS)
        set(RECOMP_DX_STUB_DIR "${CMAKE_BINARY_DIR}/aurora_dx_stubs")
        if(NOT EXISTS "${RECOMP_DX_STUB_DIR}/DXProgrammableCapture.h")
            file(MAKE_DIRECTORY ${RECOMP_DX_STUB_DIR})
            file(WRITE "${RECOMP_DX_STUB_DIR}/DXProgrammableCapture.h"
"#pragma once\n// Stubbed PIX capture header for Dawn; no functionality when PIX isn't present.\n")
        endif()
    endif()
    # Deliberately NOT injected project-wide. Only a from-source Dawn build ever
    # includes DXProgrammableCapture.h, and this tree consumes Dawn as a prebuilt
    # package (AURORA_DAWN_PROVIDER=package), so the old
    # include_directories()/add_compile_options(-I...) pair only added a dead
    # -I to every one of the ~500 third-party and runtime compile lines. The
    # aurora_* targets that do want it get it from the
    # target_include_directories() call further down; a from-source Dawn build
    # should add it to the Dawn targets the same way.

    # Keep SDL3 lean when aurora is enabled.
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)

    # MinGW/WSL can choke on SDL precompiled headers; disable PCH for aurora/SDL.
    set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
    # Isolate aurora build products to a subdirectory under the main build dir.
    add_subdirectory(${RECOMP_AURORA_DIR} ${CMAKE_BINARY_DIR}/aurora-build EXCLUDE_FROM_ALL)
    set(CMAKE_DISABLE_PRECOMPILE_HEADERS OFF)

    # Keep upstream Aurora split: several sources intentionally use file-local
    # names that collide under this repo's aggressive unity build settings.
    set(AURORA_TARGETS aurora_core aurora_gx aurora_pad aurora_si aurora_mtx aurora_vi aurora_card)
    foreach(t ${AURORA_TARGETS})
        if(TARGET ${t})
            set_target_properties(${t} PROPERTIES UNITY_BUILD OFF)
            target_compile_options(${t} PRIVATE -O3 -ffast-math -w -pipe)
            if(RECOMP_PLATFORM_WINDOWS)
                target_include_directories(${t} PRIVATE ${RECOMP_DX_STUB_DIR})
            endif()
            # Aurora's own sources include Windows headers and call std::min/max;
            # they relied on the old project-wide NOMINMAX that no longer leaks
            # into this subtree, so the define is applied per target here.
            target_compile_definitions(${t} PRIVATE NOMINMAX)
        endif()
    endforeach()

    # Frame interpolation filters guest NaN/Inf matrices out of a std::sort
    # comparator, which makes its std::isfinite guards load-bearing for memory
    # safety, not just match quality. -ffinite-math-only (part of -ffast-math
    # above) folds those guards to constants and poisons NaN float arguments
    # via nofpclass, so this one file opts back out of the finite-math
    # assumption; the file #errors if built without this. Source-scoped
    # COMPILE_OPTIONS append after the target options, so this overrides the
    # target's -ffast-math for exactly this TU.
    if(TARGET aurora_gx)
        set_source_files_properties("${RECOMP_AURORA_DIR}/lib/gx/frame_interpolation.cpp"
            TARGET_DIRECTORY aurora_gx
            PROPERTIES COMPILE_OPTIONS "-fno-finite-math-only")
    endif()

    if(RECOMP_NATIVE_PREBUILT_EXPORT_DIR)
        include("${CMAKE_CURRENT_LIST_DIR}/cmake/NativePrebuiltExport.cmake")
    endif()
endif()

# Everything from here down is this project's own code, so this is where the
# project definitions enter the directory scope. A directory-scoped definition
# seeds the COMPILE_DEFINITIONS of targets as they are created, which means:
#   * aurora-main and its third-party tree were added above -> unaffected;
#   * recomp_runtime_common, the translated shard libraries and the products in
#     cmake/PublicProducts.cmake are all created below (that file is include()d,
#     not add_subdirectory()d, so it shares this directory scope) -> they pick
#     the definitions up exactly as before.
if(RECOMP_PROJECT_COMPILE_DEFINITIONS)
    add_compile_definitions(${RECOMP_PROJECT_COMPILE_DEFINITIONS})
endif()

# Runtime sources. CONFIGURE_DEPENDS costs one glob re-check per build (measured
# at ~55 ms across the 100+ globs this build already re-verifies for SDL), and it
# buys correctness: without it, a newly added src/*.cpp that is not referenced by
# a registration file is silently never compiled and never errors. The stale-glob
# failure mode is worth far more than the milliseconds.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")
if(RECOMP_PLATFORM_MACOS)
    list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/guest_flat_memory.cpp")
    if(RECOMP_PLATFORM_MACOS_ARM64)
        # HostContext's Apple Silicon backend is implemented in a small assembly
        # companion. It must be part of the product runtime as well as the
        # standalone context test; otherwise the final executable is missing
        # recomp_co_init/recomp_co_switch at link time.
        enable_language(ASM)
        list(APPEND SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/platform/macos/co_switch.S")
    endif()
else()
    list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/guest_flat_memory_macos.cpp")
    if(RECOMP_PLATFORM_ANDROID)
        # Android shares HostContext's AArch64 assembly backend with Apple Silicon, in its ELF
        # spelling (and without touching x18, the thread's shadow-call-stack register).
        enable_language(ASM)
        list(APPEND SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/platform/android/co_switch.S")
    endif()
endif()
set(RECOMP_PLATFORM_SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/platform/host_platform.cpp")
set(RECOMP_BASE_PRODUCT_SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/product/base_product.cpp")
set(RECOMP_MOD_PRODUCT_SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/product/mod_product.cpp")
# The host ISA guard is the one translation unit that must not receive the
# x86-64-v3 flags, so it gets its own object library instead of riding along in
# recomp_runtime_common. See cmake/PublicProducts.cmake and the file's own header.
set(RECOMP_CPU_BASELINE_SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/host_cpu_baseline.cpp")
list(REMOVE_ITEM SOURCES ${RECOMP_BASE_PRODUCT_SOURCE} ${RECOMP_MOD_PRODUCT_SOURCE}
     ${RECOMP_CPU_BASELINE_SOURCE} ${RECOMP_PLATFORM_SOURCE})

# This deliberately small library contains host services that are safe to
# validate before guest memory and fiber work makes a full runtime build viable.
add_library(recomp_platform STATIC "${RECOMP_PLATFORM_SOURCE}")
target_include_directories(recomp_platform PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(recomp_platform PUBLIC cxx_std_17)
set_target_properties(recomp_platform PROPERTIES UNITY_BUILD OFF)

# Keep these independent from Aurora's BUILD_TESTING option: they validate the
# project's host-platform contracts, not Aurora's third-party test suite.
enable_testing()
if(RECOMP_BUILD_PSQ_TESTS)
    add_executable(recomp_psq_helpers_tests
        "${CMAKE_CURRENT_LIST_DIR}/tests/psq_helpers_tests.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/src/ppc_quantized.cpp")
    target_include_directories(recomp_psq_helpers_tests PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/tests/psq_memory"
        "${CMAKE_CURRENT_LIST_DIR}/include/isa"
        "${CMAKE_CURRENT_LIST_DIR}/include")
    target_compile_features(recomp_psq_helpers_tests PRIVATE cxx_std_17)
    target_compile_options(recomp_psq_helpers_tests PRIVATE
        -O2 ${RECOMP_TRANSLATED_PPC_FP_OPTIONS} -fno-slp-vectorize)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
        target_compile_options(recomp_psq_helpers_tests PRIVATE -march=x86-64-v3)
    endif()
    set_target_properties(recomp_psq_helpers_tests PROPERTIES UNITY_BUILD OFF)
    add_test(NAME recomp_psq_helpers_tests COMMAND recomp_psq_helpers_tests)
    add_test(NAME recomp_psq_reserved_tests COMMAND "${CMAKE_COMMAND}"
        "-DPSQ_TEST_EXECUTABLE=$<TARGET_FILE:recomp_psq_helpers_tests>"
        -P "${CMAKE_CURRENT_LIST_DIR}/tests/psq_reserved_tests.cmake")
endif()

add_executable(recomp_platform_paths_tests "${CMAKE_CURRENT_LIST_DIR}/tests/platform_paths_tests.cpp")
target_link_libraries(recomp_platform_paths_tests PRIVATE recomp_platform)
target_compile_features(recomp_platform_paths_tests PRIVATE cxx_std_17)
add_test(NAME recomp_platform_paths_tests COMMAND recomp_platform_paths_tests)

add_executable(recomp_runtime_config_tests "${CMAKE_CURRENT_LIST_DIR}/tests/runtime_config_tests.cpp")
target_include_directories(recomp_runtime_config_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/toml11")
target_compile_features(recomp_runtime_config_tests PRIVATE cxx_std_20)
add_test(NAME recomp_runtime_config_tests COMMAND recomp_runtime_config_tests)

add_executable(recomp_monitor_labels_tests "${CMAKE_CURRENT_LIST_DIR}/tests/monitor_labels_tests.cpp")
target_include_directories(recomp_monitor_labels_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../aurora-main/lib")
target_compile_features(recomp_monitor_labels_tests PRIVATE cxx_std_20)
add_test(NAME recomp_monitor_labels_tests COMMAND recomp_monitor_labels_tests)

add_executable(recomp_nand_save_tests "${CMAKE_CURRENT_LIST_DIR}/tests/nand_save_tests.cpp")
target_include_directories(recomp_nand_save_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(recomp_nand_save_tests PRIVATE cxx_std_17)
add_test(NAME recomp_nand_save_tests COMMAND recomp_nand_save_tests)

add_executable(recomp_audio_poll_clock_tests "${CMAKE_CURRENT_LIST_DIR}/tests/audio_poll_clock_tests.cpp")
target_compile_features(recomp_audio_poll_clock_tests PRIVATE cxx_std_17)
add_test(NAME recomp_audio_poll_clock_tests COMMAND recomp_audio_poll_clock_tests)

add_executable(recomp_nand_settings_tests "${CMAKE_CURRENT_LIST_DIR}/tests/nand_settings_tests.cpp")
find_package(Threads REQUIRED)
target_link_libraries(recomp_nand_settings_tests PRIVATE Threads::Threads)
target_include_directories(recomp_nand_settings_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(recomp_nand_settings_tests PRIVATE cxx_std_17)
add_test(NAME recomp_nand_settings_tests COMMAND recomp_nand_settings_tests)

add_executable(recomp_sc_serial_tests "${CMAKE_CURRENT_LIST_DIR}/tests/sc_serial_tests.cpp")
target_include_directories(recomp_sc_serial_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(recomp_sc_serial_tests PRIVATE cxx_std_17)
add_test(NAME recomp_sc_serial_tests COMMAND recomp_sc_serial_tests)

add_executable(no100f_window_tests "${CMAKE_CURRENT_LIST_DIR}/tests/no100f_window_tests.cpp")
target_include_directories(no100f_window_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(no100f_window_tests PRIVATE cxx_std_17)
add_test(NAME no100f_window_tests COMMAND no100f_window_tests)

add_executable(no100f_camera_aspect_tests "${CMAKE_CURRENT_LIST_DIR}/tests/no100f_camera_aspect_tests.cpp")
target_include_directories(no100f_camera_aspect_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(no100f_camera_aspect_tests PRIVATE cxx_std_17)
add_test(NAME no100f_camera_aspect_tests COMMAND no100f_camera_aspect_tests)

add_executable(no100f_ax_address_sync_tests "${CMAKE_CURRENT_LIST_DIR}/tests/no100f_ax_address_sync_tests.cpp")
target_compile_features(no100f_ax_address_sync_tests PRIVATE cxx_std_17)
add_test(NAME no100f_ax_address_sync_tests COMMAND no100f_ax_address_sync_tests)

add_executable(no100f_devmenu_state_tests "${CMAKE_CURRENT_LIST_DIR}/tests/no100f_devmenu_state_tests.cpp")
target_include_directories(no100f_devmenu_state_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(no100f_devmenu_state_tests PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(no100f_devmenu_state_tests PRIVATE /UNDEBUG)
else()
    target_compile_options(no100f_devmenu_state_tests PRIVATE -UNDEBUG)
endif()
add_test(NAME no100f_devmenu_state_tests COMMAND no100f_devmenu_state_tests)
add_executable(no100f_model_culling_tests "${CMAKE_CURRENT_LIST_DIR}/tests/no100f_model_culling_tests.cpp")
target_include_directories(no100f_model_culling_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(no100f_model_culling_tests PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(no100f_model_culling_tests PRIVATE /UNDEBUG)
else()
    target_compile_options(no100f_model_culling_tests PRIVATE -UNDEBUG)
endif()
add_test(NAME no100f_model_culling_tests COMMAND no100f_model_culling_tests)
add_executable(no100f_resources_tests "${CMAKE_CURRENT_LIST_DIR}/tests/no100f_resources_tests.cpp")
target_include_directories(no100f_resources_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(no100f_resources_tests PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(no100f_resources_tests PRIVATE /UNDEBUG)
else()
    target_compile_options(no100f_resources_tests PRIVATE -UNDEBUG)
endif()
add_test(NAME no100f_resources_tests COMMAND no100f_resources_tests)
add_executable(no100f_xbox_layout_tests "${CMAKE_CURRENT_LIST_DIR}/tests/no100f_xbox_layout_tests.cpp")
target_include_directories(no100f_xbox_layout_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(no100f_xbox_layout_tests PRIVATE cxx_std_17)
add_test(NAME no100f_xbox_layout_tests COMMAND no100f_xbox_layout_tests)

add_executable(no100f_title_backdrop_tests "${CMAKE_CURRENT_LIST_DIR}/tests/no100f_title_backdrop_tests.cpp")
target_include_directories(no100f_title_backdrop_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(no100f_title_backdrop_tests PRIVATE cxx_std_17)
add_test(NAME no100f_title_backdrop_tests COMMAND no100f_title_backdrop_tests)

add_executable(no100f_map_backdrop_tests "${CMAKE_CURRENT_LIST_DIR}/tests/no100f_map_backdrop_tests.cpp")
target_include_directories(no100f_map_backdrop_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(no100f_map_backdrop_tests PRIVATE cxx_std_17)
target_compile_options(no100f_map_backdrop_tests PRIVATE -ffast-math -fno-finite-math-only)
add_test(NAME no100f_map_backdrop_tests COMMAND no100f_map_backdrop_tests)

# The input expression engine is self-contained, so it can be exercised without
# linking the runtime or SDL.
add_executable(recomp_input_expr_tests
    "${CMAKE_CURRENT_LIST_DIR}/tests/test_expr.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/input_expr.cpp")
target_include_directories(recomp_input_expr_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(recomp_input_expr_tests PRIVATE cxx_std_17)
add_test(NAME recomp_input_expr_tests COMMAND recomp_input_expr_tests)

if(WIN32)
    add_executable(no100f_window_owner_tests
        "${CMAKE_CURRENT_LIST_DIR}/tests/window_owner_tests.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../aurora-main/lib/window_owner.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/src/host_context.cpp")
    target_include_directories(no100f_window_owner_tests PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/include"
        "${CMAKE_CURRENT_LIST_DIR}/../aurora-main/include")
    target_compile_features(no100f_window_owner_tests PRIVATE cxx_std_20)
    target_link_libraries(no100f_window_owner_tests PRIVATE SDL3::SDL3 Threads::Threads)
    foreach(case lifecycle snapshot command-queue event-overflow async-error fiber-close deferred-close motion idle-cpu)
        add_test(NAME no100f_window_owner_${case} COMMAND no100f_window_owner_tests ${case})
        set_tests_properties(no100f_window_owner_${case} PROPERTIES
            TIMEOUT 15 RUN_SERIAL TRUE ENVIRONMENT "SDL_VIDEODRIVER=dummy")
    endforeach()
endif()

# HostContext deliberately keeps the platform-specific context primitive out
# of fiber_manager.cpp. Exercise the Linux libco handoff directly so future
# refactors cannot silently remove its headers, implementation, or link edge.
if(RECOMP_PLATFORM_LINUX)
    add_executable(recomp_linux_host_context_tests
        "${CMAKE_CURRENT_LIST_DIR}/tests/host_context_tests.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/src/host_context.cpp")
    target_include_directories(recomp_linux_host_context_tests PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/include"
        "${CMAKE_CURRENT_LIST_DIR}/third_party/libco")
    target_compile_features(recomp_linux_host_context_tests PRIVATE cxx_std_17)
    target_link_libraries(recomp_linux_host_context_tests PRIVATE recomp::libco)
    add_test(NAME recomp_linux_host_context_tests COMMAND recomp_linux_host_context_tests)
endif()

if(RECOMP_PLATFORM_MACOS)
    # Exercise the public host-memory contracts separately from translated products.
    if(RECOMP_PLATFORM_MACOS_ARM64)
        # Apple Silicon's context ABI is implemented by the local assembly backend.
        enable_language(ASM)
        add_executable(recomp_macos_context_abi_tests
            "${CMAKE_CURRENT_LIST_DIR}/tests/macos_context_abi_tests.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/src/platform/macos/co_switch.S")
        target_compile_features(recomp_macos_context_abi_tests PRIVATE cxx_std_17)
        add_test(NAME recomp_macos_context_abi_tests COMMAND recomp_macos_context_abi_tests)

        add_executable(recomp_macos_host_context_tests
            "${CMAKE_CURRENT_LIST_DIR}/tests/host_context_tests.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/src/host_context.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/src/platform/macos/co_switch.S")
        target_include_directories(recomp_macos_host_context_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
    else()
        # Intel macOS follows the same System V AMD64 libco path as Linux.
        add_executable(recomp_macos_host_context_tests
            "${CMAKE_CURRENT_LIST_DIR}/tests/host_context_tests.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/src/host_context.cpp")
        target_include_directories(recomp_macos_host_context_tests PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/include"
            "${CMAKE_CURRENT_LIST_DIR}/third_party/libco")
        target_link_libraries(recomp_macos_host_context_tests PRIVATE recomp::libco)
    endif()
    target_compile_features(recomp_macos_host_context_tests PRIVATE cxx_std_17)
    add_test(NAME recomp_macos_host_context_tests COMMAND recomp_macos_host_context_tests)

    add_executable(recomp_macos_guest_flat_memory_tests
        "${CMAKE_CURRENT_LIST_DIR}/tests/macos_guest_flat_memory_tests.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/src/guest_flat_memory_macos.cpp")
    target_include_directories(recomp_macos_guest_flat_memory_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
    target_compile_features(recomp_macos_guest_flat_memory_tests PRIVATE cxx_std_17)
    add_test(NAME recomp_macos_guest_flat_memory_tests COMMAND recomp_macos_guest_flat_memory_tests)

    add_executable(recomp_macos_external_audio_tests
        "${CMAKE_CURRENT_LIST_DIR}/tests/macos_external_audio_tests.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/src/external_audio_macos.cpp")
    target_include_directories(recomp_macos_external_audio_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
    target_compile_features(recomp_macos_external_audio_tests PRIVATE cxx_std_17)
    add_test(NAME recomp_macos_external_audio_tests COMMAND recomp_macos_external_audio_tests)
endif()

# The translator emits the complete, content-addressed source graph. Consuming
# this one manifest keeps configure independent of the 28k generated function
# files and of optional mod artifacts such as code.map.
if(RECOMP_BUILD_PRODUCTS)
    set(RECOMP_TRANSLATED_SHARD_MANIFEST
        "${CMAKE_CURRENT_LIST_DIR}/../generated/build_shards/shards.cmake"
        CACHE FILEPATH "Translator-owned aggregate shard manifest")
    # The prebuilt export only needs the aurora/third-party closure configured above, so a
    # packaging machine without a translation stops here instead of failing.
    if(RECOMP_NATIVE_PREBUILT_EXPORT_DIR AND NOT EXISTS "${RECOMP_TRANSLATED_SHARD_MANIFEST}")
        message(STATUS "No translator shard manifest; configuring the native prebuilt export only")
        return()
    endif()
    if(NOT EXISTS "${RECOMP_TRANSLATED_SHARD_MANIFEST}")
        message(FATAL_ERROR
            "Missing translator-owned shard manifest: ${RECOMP_TRANSLATED_SHARD_MANIFEST}. "
            "Run Translator.Cli emit-build-shards first; see translator/README.md.")
    endif()
    include("${RECOMP_TRANSLATED_SHARD_MANIFEST}")
    set(RECOMP_HAVE_MOD_PRODUCT ${RECOMP_HAVE_MOD_SHARDS})
    message(STATUS
        "Translator graph: ${RECOMP_SHARED_BASE_FUNCTION_COUNT}/${RECOMP_BASE_FUNCTION_COUNT} base functions shared; "
        "${RECOMP_PROFILE_SENSITIVE_CALLER_COUNT} profile-sensitive callers; "
        "${RECOMP_MOD_FUNCTION_COUNT} mod functions")

    set(RECOMP_RUNTIME_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/PublicProducts.cmake")
else()
    if(RECOMP_PLATFORM_MACOS)
        # Compile-only audit of native runtime sources. It deliberately avoids
        # translated products until their host dependencies are portable.
        # These sources depend on generated/RuntimeConfig.h, which is emitted for
        # a particular game by the translator and is intentionally unavailable
        # in this platform-only configuration.
        set(RECOMP_MACOS_NATIVE_AUDIT_SOURCES ${SOURCES})
        list(REMOVE_ITEM RECOMP_MACOS_NATIVE_AUDIT_SOURCES
            "${CMAKE_CURRENT_LIST_DIR}/src/abi_bridge.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/src/hle/os/os_alarm.cpp")
        add_library(recomp_macos_native_compile OBJECT ${RECOMP_MACOS_NATIVE_AUDIT_SOURCES})
        target_include_directories(recomp_macos_native_compile PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/include" "${CMAKE_CURRENT_LIST_DIR}/src"
            "${CMAKE_CURRENT_LIST_DIR}/.." "${CMAKE_CURRENT_LIST_DIR}/../aurora-main/include")
        target_compile_features(recomp_macos_native_compile PRIVATE cxx_std_20)
        target_compile_definitions(recomp_macos_native_compile PRIVATE SDL_MAIN_HANDLED TARGET_PC)
        target_link_libraries(recomp_macos_native_compile PRIVATE
            aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx
            recomp::pugixml recomp::toml11 recomp::cryptopp recomp::mbedtls)
        if(RECOMP_PLATFORM_MACOS_X86_64)
            target_link_libraries(recomp_macos_native_compile PRIVATE recomp::libco)
            # Keep this compile-only audit on the same Haswell-era x86-64-v3
            # baseline as the translated product. PPC paired FMA helpers use
            # FMA intrinsics and intentionally cannot compile for plain x86-64.
            target_compile_options(recomp_macos_native_compile PRIVATE -march=x86-64-v3)
        endif()
        set_target_properties(recomp_macos_native_compile PROPERTIES UNITY_BUILD OFF)
    endif()
    add_custom_target(recomp_platform_paths_check DEPENDS recomp_platform)
    message(STATUS "Translated product targets disabled (RECOMP_BUILD_PRODUCTS=OFF)")
endif()
