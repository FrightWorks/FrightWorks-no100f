# Public WiiCompiled product graph.
#
# The translator owns the translated build graph. The game's profile-neutral
# functions are compiled once into recomp_base_shared; only callers whose direct
# ABI differs between profiles receive small base/mod variants.

# FrightWorks ships no game data: the translator's data_sections_init.cpp and its blobs are not compiled;
# src/hle/scooby_dol_loader.cpp loads the sections from the player's disc instead.
# Crash-report symbolization table emitted by generate-data-init. The stub
# (deliberately outside the globbed src/ tree so it is never picked up twice)
# keeps link succeeding when the generated table has not been produced yet.
set(GUEST_SYMBOL_TABLE_FILE "${RECOMP_RUNTIME_SOURCE_DIR}/../generated/guest_symbol_table.cpp")
if(EXISTS "${GUEST_SYMBOL_TABLE_FILE}")
    list(APPEND SOURCES "${GUEST_SYMBOL_TABLE_FILE}")
else()
    list(APPEND SOURCES "${RECOMP_RUNTIME_SOURCE_DIR}/cmake/guest_symbol_table_stub.cpp")
endif()
list(REMOVE_DUPLICATES SOURCES)

if(RECOMP_PLATFORM_MACOS)
    find_library(RECOMP_IOKIT_FRAMEWORK IOKit REQUIRED)
    find_library(RECOMP_COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(RECOMP_COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
endif()

function(recomp_apply_common_compile_options target)
    target_compile_options(${target} PRIVATE -O3 -ffast-math -w -pipe)
endfunction()

function(recomp_apply_translated_compile_options target)
    target_compile_options(${target} PRIVATE
        -O2 ${RECOMP_TRANSLATED_PPC_FP_OPTIONS} -fno-slp-vectorize -w -pipe)
endfunction()

function(recomp_configure_object_target target)
    target_include_directories(${target} PRIVATE
        "${RECOMP_RUNTIME_SOURCE_DIR}/include"
        "${RECOMP_RUNTIME_SOURCE_DIR}/src"
        # Workspace root, so translator output is spelled "generated/<x>.h"
        # instead of a ../ chain whose depth depends on the includer.
        "${RECOMP_RUNTIME_SOURCE_DIR}/.."
        "${RECOMP_RUNTIME_SOURCE_DIR}/../aurora-main/include")
    target_compile_definitions(${target} PRIVATE
        TARGET_PC)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
endfunction()

# Translated shard TUs are the memory-hungry compiles; everything else in the build is
# comparatively small. A dedicated Ninja job pool caps how many of them run at once so the
# global parallelism can use every core for the cheap TUs without the memory ceiling being a
# guess. The pool depth is per-machine (derived from installed RAM by LocalBuild.ps1) and is
# deliberately not part of the canonical flag set: it changes scheduling, never output bytes.
if(RECOMP_TRANSLATED_COMPILE_JOBS GREATER 0)
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS "recomp_translated=${RECOMP_TRANSLATED_COMPILE_JOBS}")
endif()

function(recomp_bound_translated_compiles target)
    if(RECOMP_TRANSLATED_COMPILE_JOBS GREATER 0)
        set_property(TARGET ${target} PROPERTY JOB_POOL_COMPILE recomp_translated)
    endif()
endfunction()

function(recomp_configure_translated_target target)
    recomp_configure_object_target(${target})
    recomp_apply_translated_compile_options(${target})
    recomp_bound_translated_compiles(${target})
endfunction()

add_library(recomp_runtime_common OBJECT ${SOURCES})
recomp_configure_object_target(recomp_runtime_common)

# FrightWorks: the build ID (version + git branch/commit/changes, build_info.h), refreshed on every build, and the
# in-game menu fonts (overlay_theme.cpp), both generated into the build tree. The build runs on a scrubbed PATH, so
# git is looked up here (tools/build-no100f.ps1 passes RECOMP_GIT_EXECUTABLE; the standard installs are hints).
find_program(RECOMP_GIT_EXECUTABLE git HINTS "$ENV{ProgramW6432}/Git/cmd" "$ENV{ProgramFiles}/Git/cmd"
             "$ENV{LOCALAPPDATA}/Programs/Git/cmd")
set(RECOMP_BUILD_INFO_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.cpp")
add_custom_target(recomp_build_info
    COMMAND "${CMAKE_COMMAND}" "-DREPO=${RECOMP_RUNTIME_SOURCE_DIR}/../.." "-DOUT=${RECOMP_BUILD_INFO_SOURCE}"
            "-DGIT_EXECUTABLE=${RECOMP_GIT_EXECUTABLE}" -P "${RECOMP_RUNTIME_SOURCE_DIR}/cmake/build_info.cmake"
    BYPRODUCTS "${RECOMP_BUILD_INFO_SOURCE}"
    COMMENT "Build ID"
    VERBATIM)
set(RECOMP_OVERLAY_FONTS
    "${RECOMP_RUNTIME_SOURCE_DIR}/assets/fonts/LuckiestGuy-Regular.ttf"
    "${RECOMP_RUNTIME_SOURCE_DIR}/assets/fonts/Roboto-Medium.ttf")
set(RECOMP_OVERLAY_FONTS_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/overlay_fonts.cpp")
string(REPLACE ";" "|" recomp_overlay_fonts_arg "${RECOMP_OVERLAY_FONTS}")
add_custom_command(OUTPUT "${RECOMP_OVERLAY_FONTS_SOURCE}"
    COMMAND "${CMAKE_COMMAND}" "-DOUT=${RECOMP_OVERLAY_FONTS_SOURCE}" "-DFONTS=${recomp_overlay_fonts_arg}"
            -P "${RECOMP_RUNTIME_SOURCE_DIR}/cmake/embed_fonts.cmake"
    DEPENDS ${RECOMP_OVERLAY_FONTS} "${RECOMP_RUNTIME_SOURCE_DIR}/cmake/embed_fonts.cmake"
    COMMENT "Overlay fonts"
    VERBATIM)
target_sources(recomp_runtime_common PRIVATE "${RECOMP_BUILD_INFO_SOURCE}" "${RECOMP_OVERLAY_FONTS_SOURCE}")
set_source_files_properties("${RECOMP_BUILD_INFO_SOURCE}" "${RECOMP_OVERLAY_FONTS_SOURCE}" PROPERTIES
    GENERATED TRUE SKIP_UNITY_BUILD_INCLUSION ON)
add_dependencies(recomp_runtime_common recomp_build_info)
target_compile_features(recomp_runtime_common PRIVATE cxx_std_20)
target_compile_definitions(recomp_runtime_common PRIVATE
    SDL_MAIN_HANDLED
    _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
target_link_libraries(recomp_runtime_common PRIVATE
    aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx aurora::card)
target_link_libraries(recomp_runtime_common PRIVATE recomp_platform recomp::pugixml recomp::toml11 recomp::cryptopp recomp::mbedtls)
if(RECOMP_PLATFORM_WINDOWS)
    # ole32/uuid: the first-start game folder picker (game_folder.cpp).
    target_link_libraries(recomp_runtime_common PRIVATE shell32 windowsapp ole32 uuid)
elseif(RECOMP_PLATFORM_LINUX)
    # ${CMAKE_DL_LIBS} for music_attenuation.cpp's dlopen of libdbus-1 (MPRIS
    # media monitoring).
    target_link_libraries(recomp_runtime_common PRIVATE recomp::libco ${CMAKE_DL_LIBS})
elseif(RECOMP_PLATFORM_ANDROID)
    target_link_libraries(recomp_runtime_common PRIVATE android log ${CMAKE_DL_LIBS})
endif()

if(RECOMP_PLATFORM_MACOS)
    # CoreAudio framework is required for automatic music muting on macOS.
    target_link_libraries(recomp_runtime_common PRIVATE "${RECOMP_COREAUDIO_FRAMEWORK}")
    if(RECOMP_PLATFORM_MACOS_X86_64)
        # libco is used by Intel macOS. Apple Silicon uses the local
        # x18-safe assembly backend and therefore does not define recomp::libco.
        target_link_libraries(recomp_runtime_common PRIVATE recomp::libco)
    endif()
endif()

if(RECOMP_CPPWINRT_INCLUDE_DIR)
    if(NOT EXISTS "${RECOMP_CPPWINRT_INCLUDE_DIR}/winrt/base.h")
        message(FATAL_ERROR
            "RECOMP_CPPWINRT_INCLUDE_DIR does not contain winrt/base.h: ${RECOMP_CPPWINRT_INCLUDE_DIR}")
    endif()
    target_include_directories(recomp_runtime_common PRIVATE "${RECOMP_CPPWINRT_INCLUDE_DIR}")
endif()

# Keep runtime unity units small and semantically related. The old generated-TU
# batch size put all 57 native runtime sources into one memory-heavy compiler job.
foreach(source IN LISTS SOURCES)
    get_filename_component(source_name "${source}" NAME_WE)
    string(REPLACE "\\" "/" source_normalized "${source}")
    if(source_normalized MATCHES "/hle/gx/")
        set(runtime_group "gx_bridge")
    elseif(source_name MATCHES "network|socket|dns|dwc|ios")
        set(runtime_group "network_ios")
    elseif(source_name MATCHES "os_|system|memory|fiber|scheduler")
        set(runtime_group "guest_system")
    elseif(source_name MATCHES "debug|trace|prof")
        set(runtime_group "diagnostics")
    else()
        string(SHA256 source_hash "${source_name}")
        string(SUBSTRING "${source_hash}" 0 4 source_hash_prefix)
        math(EXPR runtime_bucket "0x${source_hash_prefix} % 8")
        set(runtime_group "runtime_${runtime_bucket}")
    endif()
    set_source_files_properties("${source}" PROPERTIES UNITY_GROUP "${runtime_group}")
endforeach()
# PPC semantic sources are excluded from unity/PCH and configured in CMakeLists.txt.
set_target_properties(recomp_runtime_common PROPERTIES UNITY_BUILD ON UNITY_BUILD_MODE GROUP)
target_precompile_headers(recomp_runtime_common PRIVATE "${RECOMP_RUNTIME_SOURCE_DIR}/include/recomp_pch.h")
recomp_apply_common_compile_options(recomp_runtime_common)

# Host ISA guard. Windows and Linux x86_64 product targets use x86-64-v3, so
# this object deliberately keeps the plain baseline ISA and checks the CPU
# before any AVX2/FMA code can execute. AArch64 has no equivalent optional ISA
# floor to probe: NEON/FMA are architectural requirements.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
    add_library(recomp_cpu_baseline OBJECT "${RECOMP_CPU_BASELINE_SOURCE}")
    target_compile_features(recomp_cpu_baseline PRIVATE cxx_std_17)
    set_target_properties(recomp_cpu_baseline PROPERTIES UNITY_BUILD OFF)
    target_compile_options(recomp_cpu_baseline PRIVATE -w)
endif()

if(NOT RECOMP_BASE_COMMON_SHARDS)
    message(FATAL_ERROR "Translator build graph contains no shared base shards")
endif()

add_library(recomp_base_shared STATIC ${RECOMP_BASE_COMMON_SHARDS})
recomp_configure_translated_target(recomp_base_shared)
target_precompile_headers(recomp_base_shared PRIVATE "${RECOMP_RUNTIME_SOURCE_DIR}/include/recomp_pch.h")

if(RECOMP_BASE_PORTABLE_SENSITIVE_SHARDS)
    add_library(recomp_base_sensitive OBJECT ${RECOMP_BASE_PORTABLE_SENSITIVE_SHARDS})
    recomp_configure_translated_target(recomp_base_sensitive)
    target_precompile_headers(recomp_base_sensitive REUSE_FROM recomp_base_shared)
endif()

if(RECOMP_HAVE_MOD_PRODUCT)
    if(RECOMP_MOD_PORTABLE_SENSITIVE_SHARDS)
        add_library(recomp_mod_sensitive OBJECT ${RECOMP_MOD_PORTABLE_SENSITIVE_SHARDS})
        recomp_configure_translated_target(recomp_mod_sensitive)
        target_precompile_headers(recomp_mod_sensitive REUSE_FROM recomp_base_shared)
    endif()

    set(RECOMP_MOD_TRANSLATED_SOURCES ${RECOMP_MOD_SHARDS} ${RECOMP_MOD_EXTRA_SOURCES})
    set(RECOMP_MOD_BLOB_OBJECTS)
    foreach(source IN LISTS RECOMP_MOD_EXTRA_SOURCES)
        if(source MATCHES "\\.S$")
            enable_language(ASM)
            set_source_files_properties("${source}" PROPERTIES LANGUAGE ASM SKIP_PRECOMPILE_HEADERS ON)
        endif()
    endforeach()
    add_library(recomp_mod_functions OBJECT ${RECOMP_MOD_TRANSLATED_SOURCES})
    recomp_configure_translated_target(recomp_mod_functions)
    target_precompile_headers(recomp_mod_functions REUSE_FROM recomp_base_shared)
endif()

function(recomp_configure_product target)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:recomp_runtime_common>)
    # Startup CPU check. Must stay a separate object library so it keeps the
    # plain baseline ISA while everything around it is built for x86-64-v3.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
        target_sources(${target} PRIVATE $<TARGET_OBJECTS:recomp_cpu_baseline>)
    endif()
    target_include_directories(${target} PRIVATE
        "${RECOMP_RUNTIME_SOURCE_DIR}/include"
        "${RECOMP_RUNTIME_SOURCE_DIR}/src"
        # Workspace root, so translator output is spelled "generated/<x>.h"
        # instead of a ../ chain whose depth depends on the includer.
        "${RECOMP_RUNTIME_SOURCE_DIR}/.."
        "${RECOMP_RUNTIME_SOURCE_DIR}/../aurora-main/include")
    target_compile_definitions(${target} PRIVATE
        SDL_MAIN_HANDLED _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION TARGET_PC)
    target_compile_features(${target} PRIVATE cxx_std_20)
    recomp_apply_common_compile_options(${target})
    # The dispatch-table and registration shards compile inside the product target itself and
    # include the same fat translated headers; bound them by the same pool.
    recomp_bound_translated_compiles(${target})
    target_link_libraries(${target} PRIVATE
        recomp_platform recomp_base_shared recomp::pugixml recomp::toml11 recomp::cryptopp recomp::mbedtls)

    target_link_libraries(${target} PRIVATE
        aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx aurora::card)
    if(RECOMP_PLATFORM_MACOS)
        target_link_libraries(${target} PRIVATE
            "${RECOMP_IOKIT_FRAMEWORK}" "${RECOMP_COREFOUNDATION_FRAMEWORK}" "${RECOMP_COREAUDIO_FRAMEWORK}")
        target_link_options(${target} PRIVATE
            "LINKER:-U,_OBJC_CLASS_$_MTLLogStateDescriptor")
    endif()
    if(EXISTS "${RECOMP_AURORA_DIR}/cmake/AuroraCopyRuntimeDLLs.cmake")
        include("${RECOMP_AURORA_DIR}/cmake/AuroraCopyRuntimeDLLs.cmake")
        aurora_copy_runtime_dlls(${target})
    endif()
    if(TARGET sqlite3)
        get_target_property(RECOMP_SQLITE_TARGET_TYPE sqlite3 TYPE)
    endif()
    if(TARGET sqlite3 AND
       (RECOMP_SQLITE_TARGET_TYPE STREQUAL "SHARED_LIBRARY" OR
        RECOMP_SQLITE_TARGET_TYPE STREQUAL "MODULE_LIBRARY"))
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:sqlite3> $<TARGET_FILE_DIR:${target}>)
    endif()

    if(RECOMP_PLATFORM_WINDOWS)
        target_link_libraries(${target} PRIVATE
            dbghelp user32 winmm ws2_32 iphlpapi secur32 crypt32 windowsapp)

        set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE TRUE)
    elseif(RECOMP_PLATFORM_LINUX OR RECOMP_PLATFORM_MACOS_X86_64)
        # recomp_runtime_common is an OBJECT library: WiiCompiled/ModProduct only pull in its .o
        # files via $<TARGET_OBJECTS:>, which does not propagate recomp_runtime_common's own
        # target_link_libraries (object libraries don't carry usage requirements to a consumer
        # that isn't itself linked against as a target). fiber_manager.cpp's co_* calls live in
        # those objects, so the actual executable link needs recomp::libco directly, same as it
        # needs it independently of that first `if(WIN32)` branch above. ${CMAKE_DL_LIBS} is
        # here for the same reason: music_attenuation.cpp's dlopen(libdbus-1) lives in those
        # objects (empty string on glibc >= 2.34, where dl* is in libc).
        target_link_libraries(${target} PRIVATE recomp::libco ${CMAKE_DL_LIBS})
    elseif(RECOMP_PLATFORM_ANDROID)
        # The same object-library gap as above: the product link needs the libraries the runtime
        # objects call into. SDL's Android activity loads the product as libmain.so and runs its
        # exported SDL_main (runtime/src/main.cpp). 16 KiB pages keep it loadable on Android 15+
        # devices that use them.
        target_link_libraries(${target} PRIVATE android log ${CMAKE_DL_LIBS})
        target_link_options(${target} PRIVATE "LINKER:-z,max-page-size=16384" "LINKER:--no-undefined")
        set_target_properties(${target} PROPERTIES OUTPUT_NAME main)
    endif()
    if(RECOMP_PLATFORM_WINDOWS)
        foreach(runtime_dll libc++.dll libunwind.dll)
            execute_process(
                COMMAND "${CMAKE_CXX_COMPILER}" "--print-file-name=${runtime_dll}"
                OUTPUT_VARIABLE runtime_dll_path
                OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(NOT EXISTS "${runtime_dll_path}")
                get_filename_component(recomp_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
                set(runtime_dll_path "${recomp_compiler_bin}/${runtime_dll}")
            endif()
            if(NOT EXISTS "${runtime_dll_path}")
                message(FATAL_ERROR "llvm-mingw runtime DLL not found: ${runtime_dll}")
            endif()
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${runtime_dll_path}" $<TARGET_FILE_DIR:${target}>)
        endforeach()
    endif()

    set(RECOMP_WII_BOOTSTRAP_SOURCE_DIR "${RECOMP_RUNTIME_SOURCE_DIR}/assets/wii")
    if(NOT EXISTS "${RECOMP_WII_BOOTSTRAP_SOURCE_DIR}/shared2/wc24")
        message(FATAL_ERROR "Missing Wii first-run bootstrap payload: ${RECOMP_WII_BOOTSTRAP_SOURCE_DIR}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${RECOMP_WII_BOOTSTRAP_SOURCE_DIR}" "$<TARGET_FILE_DIR:${target}>/wii_bootstrap")

    set(RECOMP_DSP_COEFFICIENT_ROM "${RECOMP_RUNTIME_SOURCE_DIR}/assets/dsp/dsp_coef.bin")
    if(NOT EXISTS "${RECOMP_DSP_COEFFICIENT_ROM}")
        message(FATAL_ERROR "Missing Wii DSP coefficient ROM: ${RECOMP_DSP_COEFFICIENT_ROM}")
    endif()
    file(SHA256 "${RECOMP_DSP_COEFFICIENT_ROM}" RECOMP_DSP_COEFFICIENT_ROM_SHA256)
    if(NOT RECOMP_DSP_COEFFICIENT_ROM_SHA256 STREQUAL
       "d7741279c2e8ec5c5fb318f8fbdd6de6bf583520d288e836a5383233a4238179")
        message(FATAL_ERROR "Wii DSP coefficient ROM hash mismatch: ${RECOMP_DSP_COEFFICIENT_ROM_SHA256}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${RECOMP_DSP_COEFFICIENT_ROM}" "$<TARGET_FILE_DIR:${target}>/dsp_coef.bin")

    # Aurora imports this portable recipe database into each user's writable
    # pipeline cache. Keep the upstream filename so its default resourcesPath
    # lookup works without application-specific configuration.
    set(RECOMP_INITIAL_PIPELINE_CACHE
        "${RECOMP_RUNTIME_SOURCE_DIR}/assets/pipeline/initial_pipeline_cache.db")
    if(NOT EXISTS "${RECOMP_INITIAL_PIPELINE_CACHE}")
        message(FATAL_ERROR "Missing transferable Aurora pipeline cache: ${RECOMP_INITIAL_PIPELINE_CACHE}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${RECOMP_INITIAL_PIPELINE_CACHE}"
        "$<TARGET_FILE_DIR:${target}>/initial_pipeline_cache.db")

    # Non-Windows TLS (runtime/src/hle/net/network_ssl.cpp's mbed TLS path) needs a trusted root
    # CA bundle to verify server certificates against - Windows gets this for free from the OS via
    # Schannel, mbed TLS does not ship one itself. Not SHA256-pinned like the DSP ROM above: unlike
    # a fixed hardware ROM, this bundle is expected to be refreshed periodically as CAs rotate.
    # Windows gets its trust store from Schannel, so only the platforms that actually build the
    # mbed TLS path need the bundle beside the executable.
    if(NOT RECOMP_PLATFORM_WINDOWS)
        set(RECOMP_CA_CERTIFICATE_BUNDLE "${RECOMP_RUNTIME_SOURCE_DIR}/assets/certs/cacert.pem")
        if(NOT EXISTS "${RECOMP_CA_CERTIFICATE_BUNDLE}")
            message(FATAL_ERROR "Missing TLS root CA bundle: ${RECOMP_CA_CERTIFICATE_BUNDLE}")
        endif()
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RECOMP_CA_CERTIFICATE_BUNDLE}" "$<TARGET_FILE_DIR:${target}>/cacert.pem")
    endif()
endfunction()

if(RECOMP_PLATFORM_ANDROID)
    add_library(WiiCompiled SHARED "${RECOMP_BASE_PRODUCT_SOURCE}" ${RECOMP_BASE_REGISTRATION_SOURCES})
else()
    add_executable(WiiCompiled "${RECOMP_BASE_PRODUCT_SOURCE}" ${RECOMP_BASE_REGISTRATION_SOURCES})
endif()
recomp_configure_product(WiiCompiled)
target_precompile_headers(WiiCompiled PRIVATE
    "${RECOMP_RUNTIME_SOURCE_DIR}/include/recomp_pch.h")
if(RECOMP_PLATFORM_WINDOWS)
    # The app icon (tools/launcher-art/make_launcher_art.py --icon): Explorer, the taskbar and, through SDL's
    # default of the executable's first icon, the game window.
    enable_language(RC)
    file(TO_CMAKE_PATH "${RECOMP_RUNTIME_SOURCE_DIR}/assets/icon/frightworks.ico" RECOMP_APP_ICON)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc" "1 ICON \"${RECOMP_APP_ICON}\"\n")
    target_sources(WiiCompiled PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc")
endif()
if(TARGET recomp_base_sensitive)
    target_sources(WiiCompiled PRIVATE $<TARGET_OBJECTS:recomp_base_sensitive>)
endif()

if(RECOMP_HAVE_MOD_PRODUCT)
    add_executable(ModProduct "${RECOMP_MOD_PRODUCT_SOURCE}" ${RECOMP_MOD_REGISTRATION_SOURCES})
    recomp_configure_product(ModProduct)
    target_precompile_headers(ModProduct REUSE_FROM WiiCompiled)
    if(TARGET recomp_mod_sensitive)
        target_sources(ModProduct PRIVATE $<TARGET_OBJECTS:recomp_mod_sensitive>)
    endif()
    target_sources(ModProduct PRIVATE $<TARGET_OBJECTS:recomp_mod_functions>)
    if(RECOMP_MOD_BLOB_OBJECTS)
        target_sources(ModProduct PRIVATE ${RECOMP_MOD_BLOB_OBJECTS})
    endif()
    add_custom_target(recomp_release DEPENDS WiiCompiled ModProduct)
else()
    add_custom_target(recomp_release DEPENDS WiiCompiled)
    message(STATUS "ModProduct target disabled (run translate-mod and emit-build-shards)")
endif()

# Windows and Linux x86_64 share the x86-64-v3 floor that the CPU baseline
# object above checks. AArch64 builds are compiled locally for the host that
# will run them, so both Linux and Apple Silicon use the compiler's native CPU
# tuning rather than leaving target-specific performance on the table.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
    set(RECOMP_BASELINE_ARCH_FLAG -march=x86-64-v3)
elseif(RECOMP_PLATFORM_ANDROID)
    # Cross-compiled, so -mcpu=native would describe the build machine. ARMv8.2-A (LSE atomics,
    # RCpc loads) is the floor of every Android phone since Cortex-A55/A75.
    set(RECOMP_BASELINE_ARCH_FLAG -march=armv8.2-a)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(RECOMP_BASELINE_ARCH_FLAG -mcpu=native)
else()
    set(RECOMP_BASELINE_ARCH_FLAG "")
endif()

set(RECOMP_ALL_BUILD_TARGETS
    recomp_runtime_common recomp_base_shared recomp_base_sensitive recomp_mod_sensitive
    recomp_mod_functions WiiCompiled ModProduct)
foreach(target IN LISTS RECOMP_ALL_BUILD_TARGETS)
    if(TARGET ${target} AND RECOMP_BASELINE_ARCH_FLAG)
        target_compile_options(${target} PRIVATE ${RECOMP_BASELINE_ARCH_FLAG})
    endif()
endforeach()
