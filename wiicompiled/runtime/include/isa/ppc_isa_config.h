#pragma once

#include <atomic>
#include <cstdint>

#define RECOMP_RESTRICT __restrict
#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#else
#error "ppc_isa_config.h has no SIMD intrinsics header for this architecture"
#endif

inline constexpr bool RecompStateFreeAbiEnabled(uint32_t) noexcept
{
    return true;
}

#if defined(_WIN32)
#define RECOMP_PPC_FORCE_INLINE __forceinline
#define RECOMP_PPC_NO_INLINE __declspec(noinline)
#define RECOMP_PPC_INTERNAL_CALL __regcall
#else
// __forceinline/__declspec are MS-extension keywords Clang only recognizes when targeting
// Windows (MSVC or mingw); native Linux Clang needs the GNU-attribute spellings instead.
// __regcall has no portable non-Windows equivalent worth chasing here - the extra register
// args it saves matter for the hot PPC interpreter loop on Windows, but plain calls are fine
// elsewhere.
#define RECOMP_PPC_FORCE_INLINE __attribute__((always_inline)) inline
#define RECOMP_PPC_NO_INLINE __attribute__((noinline))
#define RECOMP_PPC_INTERNAL_CALL
#endif
#define RECOMP_PPC_ALWAYS_INLINE_BODY __attribute__((always_inline))
#define RECOMP_PPC_COLD __attribute__((cold))


using RecompStateFreeResult2 = uint64_t __attribute__((ext_vector_type(2)));
