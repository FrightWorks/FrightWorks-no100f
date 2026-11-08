# Consume a precompiled aurora + third-party package instead of compiling
# aurora-main and its extern/ tree.
#
# Produced by launcher/Prepare-NativePrebuilt.ps1 with the same toolchain and the
# same canonical flag set the consuming build uses, which is what makes a single
# package valid for every user. Nothing about this project's own code changes:
# the runtime, the translated shards and the products still compile locally and
# still link targets named aurora::gx / aurora::pad / aurora::si / aurora::vi /
# aurora::mtx, so cmake/PublicProducts.cmake needs no prebuilt-specific branch.
#
# The aurora and dependency source trees are still present in the workspace - the
# runtime includes aurora headers directly - so the package records include
# directories as tokens (@AURORA@, @RUNTIME@, @DEPS@, @PKG@) that resolve here
# rather than baking a maintainer machine's paths into a redistributable.

get_filename_component(RECOMP_NP_PACKAGE_DIR "${RECOMP_NATIVE_PREBUILT_DIR}" ABSOLUTE)
if(NOT EXISTS "${RECOMP_NP_PACKAGE_DIR}/native_prebuilt.cmake")
    message(FATAL_ERROR
        "RECOMP_NATIVE_PREBUILT_DIR does not contain native_prebuilt.cmake: ${RECOMP_NP_PACKAGE_DIR}")
endif()

# The package ships as <workspace>/Dependencies/native_prebuilt, next to the
# pinned sources it was built against.
get_filename_component(RECOMP_NP_DEPS_DIR "${RECOMP_NP_PACKAGE_DIR}" DIRECTORY)
get_filename_component(RECOMP_NP_AURORA_DIR "${RECOMP_AURORA_DIR}" ABSOLUTE)
get_filename_component(RECOMP_NP_RUNTIME_DIR "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)

include("${RECOMP_NP_PACKAGE_DIR}/native_prebuilt.cmake")

function(recomp_np_resolve out_var)
    set(_resolved "")
    foreach(_item IN LISTS ARGN)
        string(REPLACE "@PKG@" "${RECOMP_NP_PACKAGE_DIR}" _item "${_item}")
        string(REPLACE "@AURORA@" "${RECOMP_NP_AURORA_DIR}" _item "${_item}")
        string(REPLACE "@RUNTIME@" "${RECOMP_NP_RUNTIME_DIR}" _item "${_item}")
        string(REPLACE "@DEPS@" "${RECOMP_NP_DEPS_DIR}" _item "${_item}")
        list(APPEND _resolved "${_item}")
    endforeach()
    set(${out_var} "${_resolved}" PARENT_SCOPE)
endfunction()

recomp_np_resolve(_recomp_np_includes ${RECOMP_NP_INCLUDE_DIRECTORIES})
recomp_np_resolve(_recomp_np_links ${RECOMP_NP_LINK_LIBRARIES})
recomp_np_resolve(_recomp_np_dawn_config ${RECOMP_NP_DAWN_CONFIG_DIR})
if(NOT RECOMP_PLATFORM_WINDOWS)
    if(NOT RECOMP_NP_MBEDTLS_LIBRARIES OR NOT RECOMP_NP_MBEDTLS_INCLUDE_DIR)
        message(FATAL_ERROR
            "The Linux native prebuilt package has no Mbed TLS archives; regenerate it with Prepare-NativePrebuilt.sh")
    endif()
    file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/MbedTLSPin.cmake" _recomp_np_current_mbedtls_fingerprint)
    if(NOT RECOMP_NP_MBEDTLS_FINGERPRINT STREQUAL _recomp_np_current_mbedtls_fingerprint)
        message(FATAL_ERROR "The native prebuilt Mbed TLS version differs from this workspace; regenerate the package")
    endif()
    recomp_np_resolve(_recomp_np_mbedtls_links ${RECOMP_NP_MBEDTLS_LIBRARIES})
    recomp_np_resolve(_recomp_np_mbedtls_include ${RECOMP_NP_MBEDTLS_INCLUDE_DIR})
    foreach(_archive IN LISTS _recomp_np_mbedtls_links)
        if(NOT EXISTS "${_archive}")
            message(FATAL_ERROR "The native prebuilt package is missing Mbed TLS archive: ${_archive}")
        endif()
    endforeach()
    if(NOT IS_DIRECTORY "${_recomp_np_mbedtls_include}")
        message(FATAL_ERROR "The native prebuilt package is missing Mbed TLS headers: ${_recomp_np_mbedtls_include}")
    endif()
    target_include_directories(recomp_mbedtls SYSTEM INTERFACE "${_recomp_np_mbedtls_include}")
    target_link_libraries(recomp_mbedtls INTERFACE ${_recomp_np_mbedtls_links})
endif()

foreach(_dir IN LISTS _recomp_np_includes)
    if(NOT IS_DIRECTORY "${_dir}")
        message(FATAL_ERROR
            "The precompiled package expects an include directory that this workspace "
            "does not have: ${_dir}")
    endif()
endforeach()

# Dawn's own packaged config (DawnTargets.cmake) links dawn::webgpu_dawn against
# Threads::Threads directly. A from-source aurora build resolves that as a side effect of
# add_subdirectory(aurora-main) pulling in Dawn's own CMakeLists.txt; this mode never runs that
# subdirectory at all, so nothing else would ever define it (verified directly: configuring without
# this fails with "The link interface of target dawn::webgpu_dawn contains: Threads::Threads but
# the target was not found"). find_package(Threads) is one of CMake's most basic finder modules and
# is a no-op on Windows (its threading support is already part of the CRT), so this is safe on
# every platform this package format targets, not just the one that first hit the failure.
find_package(Threads REQUIRED)

# Dawn is a prebuilt package on both sides; resolve the same install tree the
# package was built against so its imported target (and therefore
# webgpu_dawn.dll / dxcompiler.dll / dxil.dll) is available exactly as a
# from-source aurora build would provide it.
if(NOT EXISTS "${_recomp_np_dawn_config}/DawnConfig.cmake")
    message(FATAL_ERROR "Prebuilt Dawn package is missing DawnConfig.cmake: ${_recomp_np_dawn_config}")
endif()
set(_recomp_np_saved_find_global ${CMAKE_FIND_PACKAGE_TARGETS_GLOBAL})
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ON)
find_package(Dawn REQUIRED CONFIG PATHS "${_recomp_np_dawn_config}" NO_DEFAULT_PATH)
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ${_recomp_np_saved_find_global})
if(NOT TARGET dawn::webgpu_dawn)
    message(FATAL_ERROR "Prebuilt Dawn package did not provide dawn::webgpu_dawn")
endif()

# One interface target carries the whole compile/link surface. The public aurora
# names then reduce to aliases of it, which keeps the link line free of the
# fivefold duplication that giving each of them the full list would produce.
add_library(recomp_native_prebuilt INTERFACE IMPORTED GLOBAL)
set_target_properties(recomp_native_prebuilt PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_recomp_np_includes}"
    INTERFACE_COMPILE_DEFINITIONS "${RECOMP_NP_COMPILE_DEFINITIONS}"
    INTERFACE_COMPILE_OPTIONS "${RECOMP_NP_COMPILE_OPTIONS}"
    INTERFACE_LINK_LIBRARIES "${_recomp_np_links}")

foreach(_target IN LISTS RECOMP_NP_AURORA_TARGETS)
    add_library(${_target} INTERFACE IMPORTED GLOBAL)
    set_target_properties(${_target} PROPERTIES INTERFACE_LINK_LIBRARIES recomp_native_prebuilt)
endforeach()

# The vendored Crypto++ ships precompiled alongside aurora: under the fixed
# toolchain its archive is identical for every user, so compiling ~150 of its
# translation units locally buys nothing. Only the archive is consumed here -
# the vendored headers still ship in the workspace because first-party TUs
# include them directly (runtime/include/wii_es_crypto.h).
if(NOT RECOMP_CRYPTOPP_INTERFACE_DEFINITIONS)
    message(FATAL_ERROR
        "The prebuilt consumer requires RECOMP_CRYPTOPP_INTERFACE_DEFINITIONS from "
        "the top-level CMakeLists.txt; without them first-party TUs would compile "
        "against Crypto++ headers configured differently than the archive.")
endif()
file(GLOB _recomp_np_cryptopp_archives "${RECOMP_NP_PACKAGE_DIR}/lib/librecomp_cryptopp.a")
list(LENGTH _recomp_np_cryptopp_archives _recomp_np_cryptopp_count)
if(NOT _recomp_np_cryptopp_count EQUAL 1)
    message(FATAL_ERROR
        "The precompiled package must contain exactly one librecomp_cryptopp.a: "
        "${RECOMP_NP_PACKAGE_DIR}")
endif()
if(NOT TARGET recomp_cryptopp)
    add_library(recomp_cryptopp STATIC IMPORTED GLOBAL)
    set_target_properties(recomp_cryptopp PROPERTIES
        IMPORTED_LOCATION "${_recomp_np_cryptopp_archives}"
        INTERFACE_INCLUDE_DIRECTORIES "${RECOMP_NP_RUNTIME_DIR}/third_party"
        INTERFACE_COMPILE_DEFINITIONS "${RECOMP_CRYPTOPP_INTERFACE_DEFINITIONS}")
    add_library(recomp::cryptopp ALIAS recomp_cryptopp)
endif()
# The aggregate link list carries the same archive once more (the harvest probe
# linked it, so the probe-vs-control diff includes it). Repeating a static
# archive on the link line is harmless; the second pass resolves no members.

list(LENGTH _recomp_np_links _recomp_np_link_count)
message(STATUS
    "Aurora and third-party libraries come from the precompiled package "
    "(${_recomp_np_link_count} link inputs); they are not compiled here")
