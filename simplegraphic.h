#pragma once

/*
 * The public ABI deliberately retains the historic RunLuaFileAsWin name.
 * Path of Building resolves that name from SimpleGraphic.dll on Windows;
 * portable hosts link to the identical C ABI on macOS and Linux.
 */
#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(SIMPLEGRAPHIC_EXPORTS)
#    if defined(__GNUC__)
#      define SIMPLEGRAPHIC_API __attribute__((dllexport))
#    else
#      define SIMPLEGRAPHIC_API __declspec(dllexport)
#    endif
#  else
#    if defined(__GNUC__)
#      define SIMPLEGRAPHIC_API __attribute__((dllimport))
#    else
#      define SIMPLEGRAPHIC_API __declspec(dllimport)
#    endif
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define SIMPLEGRAPHIC_API __attribute__((visibility("default")))
#else
#  define SIMPLEGRAPHIC_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

SIMPLEGRAPHIC_API int RunLuaFileAsWin(int argc, char** argv);

// A non-GUI verification entry point used by packaging CI. It exercises Lua's
// native module loader from a staged runtime directory.
SIMPLEGRAPHIC_API int SimpleGraphicRuntimeSmoke(const char* runtimeDirectory);

#ifdef __cplusplus
}
#endif
