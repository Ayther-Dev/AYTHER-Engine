#pragma once
// ---------------------------------------------------------------------------
// ayther_diagnostic.h — the ONLY sanctioned way to silence a warning in
// first-party code, and it is deliberately narrow.
//
// The project compiles its own C++ with warnings as errors. That policy is
// worth nothing if the escape hatch is a compiler flag on a whole target,
// because a target-wide exemption also hides the next warning, the one nobody
// meant to accept. Every suppression here is a push/pop around the exact lines
// that need it.
//
// There is one legitimate reason to use these: a test that calls a DEPRECATED
// API ON PURPOSE. The ABI parity oracles exist to compare the legacy accessors
// against the versioned ones, so they must call the legacy accessors; the
// deprecation is aimed at production callers, not at the oracle that proves the
// replacement still agrees with what it replaced. Suppressing the warning there
// is the point, and doing it in three lines around the call keeps it visible.
//
// Anything else -- an unused parameter, a narrowing conversion, a shadowed
// variable -- gets fixed rather than wrapped. If you are reaching for this to
// make a warning go away, it is the wrong tool.
//
// Engine-internal header: not installed.
// ---------------------------------------------------------------------------

#if defined(_MSC_VER) && !defined(__clang__)
// C4996: 'x': was declared deprecated. The same code number covers the CRT's
// "unsafe function" deprecations, which is why production code uses the
// wrappers in ayther_file.h and ayther_env.h instead of reaching for this.
#  define AYTHER_LEGACY_ABI_BEGIN \
      __pragma(warning(push))     \
      __pragma(warning(disable : 4996))
#  define AYTHER_LEGACY_ABI_END __pragma(warning(pop))
#elif defined(__clang__)
#  define AYTHER_LEGACY_ABI_BEGIN                                    \
      _Pragma("clang diagnostic push")                               \
      _Pragma("clang diagnostic ignored \"-Wdeprecated-declarations\"")
#  define AYTHER_LEGACY_ABI_END _Pragma("clang diagnostic pop")
#elif defined(__GNUC__)
#  define AYTHER_LEGACY_ABI_BEGIN                                  \
      _Pragma("GCC diagnostic push")                               \
      _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
#  define AYTHER_LEGACY_ABI_END _Pragma("GCC diagnostic pop")
#else
#  define AYTHER_LEGACY_ABI_BEGIN
#  define AYTHER_LEGACY_ABI_END
#endif
