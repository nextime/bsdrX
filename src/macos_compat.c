/* macos_compat.c — osxcross toolchain shim (macOS builds only).
 *
 * The osxcross clang is older than the macOS SDK we build against and ships no compiler-rt builtins
 * library, so ffmpeg's libavutil videotoolbox hwcontext leaves `__isPlatformVersionAtLeast`
 * (clang's `@available` helper) undefined at link. Provide a weak definition — the real compiler-rt
 * symbol wins wherever it exists. We target modern macOS (deployment >= 11.0), so reporting the
 * queried platform version as available is correct for the feature checks ffmpeg performs.
 *
 * Copyright (C) 2026 Stefy Lanza. GNU GPL v3 or later.
 */
#if defined(__APPLE__)
#include <stdint.h>

__attribute__((weak))
int32_t __isPlatformVersionAtLeast(uint32_t platform, uint32_t major, uint32_t minor, uint32_t sub) {
    (void)platform; (void)major; (void)minor; (void)sub;
    return 1;   /* assume the running macOS satisfies the checked version (min deployment >= 11.0) */
}
#endif
