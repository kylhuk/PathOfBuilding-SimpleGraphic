# Path of Building Community SimpleGraphic

## Introduction

`SimpleGraphic` is the host environment for Lua.
It contains the API used by the application's Lua logic, as well as a
2D OpenGL ES 2.0 renderer, window management, input handling, and a
debug console.
It retains the historic `RunLuaFileAsWin` C ABI, which is passed a
C-style argc/argv argument list, with the script path as `argv[0]`.

The installed payload is self-contained: the host executable, `SimpleGraphic`
library, Lua modules, and non-system shared-library dependencies are staged
together. `PathOfBuilding-SimpleGraphic --smoke-modules` performs a GUI-free
load test of every shipped native Lua module.

## Supported targets

| Operating system | x86 | x64 / AMD64 | ARMv8 / ARM64 |
| --- | :---: | :---: | :---: |
| Windows 10 | ✓ | ✓ | ✓ |
| Windows 11 | ✓ | ✓ | ✓ |
| Linux | ✓ | ✓ | ✓ |
| macOS | — | ✓ | ✓ |

Windows 10 and 11 use the same supported MSVC runtime family. GitHub does not
offer a Windows 10 hosted image, so opt-in self-hosted Windows 10 smoke jobs
are included for maintainers who set `ENABLE_WINDOWS10_SMOKE=true` (x64/x86
compatibility) and/or `ENABLE_WINDOWS10_ARM64_SMOKE=true` (native ARM64).
The matching self-hosted runners retrieve their just-built artifact through the
pinned GitHub Actions download step, so they do not need the GitHub CLI.
macOS Intel packages target macOS 10.15 and later; Apple-Silicon packages
target macOS 11.0 and later.

## Building

Initialize the checked-in dependency sources first:

```sh
git submodule update --init --recursive
```

Install CMake 3.24+, Ninja (or Visual Studio 2022 on Windows), Python 3, and
the normal build tools for the target platform. The first configure invokes the
bundled vcpkg manifest using its committed baseline, so no global vcpkg setup
is required.

The CMake presets describe every shipped architecture:

```sh
cmake --preset linux-x64
cmake --build --preset linux-x64
ctest --preset linux-x64
cmake --install out/build/linux-x64
```

Replace `linux-x64` with one of:

```text
linux-x86       linux-x64       linux-arm64
macos-x64       macos-arm64
windows-x86     windows-x64     windows-arm64
```

For `linux-x86` on a 64-bit Debian/Ubuntu host, install the 32-bit compiler
and C library headers first (`gcc-multilib g++-multilib libc6-dev-i386`), or
use the native i386 container recipe in the build workflow. The preset adds
the required `-m32` compile and linker flags automatically.

Windows presets use the Visual Studio generator. For example:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --config Release
ctest --preset windows-x64 -C Release
cmake --install out/build/windows-x64 --config Release
```

Unix packages deliberately use the checked-in `*-dynamic` vcpkg triplets:
this guarantees that the executable and every Lua extension share one dynamic
LuaJIT runtime. Do not replace those triplets with static LuaJIT variants.

The `INSTALL` target creates a ready-to-package runtime directory. Its
contents can be copied directly into an installer payload; no build directory
or vcpkg installation is needed at runtime. Run this before packaging:

```sh
./out/stage/linux-x64/PathOfBuilding-SimpleGraphic --version
./out/stage/linux-x64/PathOfBuilding-SimpleGraphic --smoke-modules
```

On Windows, use the `.exe` suffix.

### CI and releases

The repository uses four workflows:

- **Validate source** runs on pull requests and `master` updates.
- **Development runtime builds** runs automatically for every pull request and
  `master` push, and can be manually dispatched. It uploads eight
  installer-ready artifacts for 14 days.
- **Build runtime matrix** is the shared implementation for development and
  release builds. It builds Windows x86/x64/ARM64, macOS x64/ARM64, and Linux
  x86/x64/ARM64, then runs the staged Lua-module smoke test.
- **Publish release** has no automatic trigger. A maintainer manually enters a
  SemVer version; the workflow verifies that the selected revision is the
  current `master` tip, builds all eight packages, produces SHA-256 checksums,
  and creates the GitHub release.

For a prerelease, `CMakeLists.txt` retains the numeric version core while
`config.h` and `vcpkg.json` declare the complete SemVer string (for example
`2.6.0-rc.1+build.5`). The manual workflow checks both forms before it creates
a tag.

Release archives contain one top-level directory named for their target,
which makes them safe inputs to setup/installer tooling. The release includes
`SHA256SUMS.txt` and `release-manifest.json` for automated packagers.

Dependency and GitHub Action updates are tracked weekly by Dependabot. The
vcpkg baseline and custom LuaJIT port are pinned so that each source revision
still resolves reproducibly.

### Debugging

On Windows, `SimpleGraphic.dll` remains dynamically loadable by
`PathOfBuilding.exe`. To debug that integration, run `PathOfBuilding.exe` and
then attach to that process using the
"Debug" > "Attach to Process..." menu option in Visual Studio.

Visual Studio can also be configured to start the Path of Building executable
when debugging a target which troubleshooting of early startup.

## Project dependencies

Runtime and utilities:
* [LuaJIT](https://github.com/LuaJIT/LuaJIT) - fast Lua fork with JIT compilation that has diverged from upstream Lua at version 5.1
* [curl](https://curl.se/) - very common HTTP library, exposed to Lua
* [fmtlib](https://fmt.dev/) - modern string formatting
* [Microsoft GSL](https://github.com/microsoft/GSL) - bounds-aware utility types
* [pkgconf](http://pkgconf.org/) - part of the build process to locate builds of bundled libraries
* [re2](https://github.com/google/re2) - regex library
* [sol2](https://github.com/ThePhD/sol2) - C++ bindings for Lua

Graphics:
* [GLFW](https://www.glfw.org/) - multi-platform windowing library for OpenGL (and other APIs)
* [ANGLE](https://github.com/google/angle) - OpenGL ES runtime from Google built on top of native rendering APIs
* [Glad 2](https://gen.glad.sh/) - OpenGL header generator
* [GLM](https://github.com/g-truc/glm) - graphics mathematics
* [Dear ImGui](https://github.com/ocornut/imgui) - debug and editor user interface

Compression and image formats:
* [stb](https://github.com/nothings/stb) - single-header libraries for many things, here image reading and writing
* [Compressonator](https://github.com/GPUOpen-Tools/compressonator) - GPU texture compression helpers
* [libwebp](https://chromium.googlesource.com/webm/libwebp/) - WebP decoding
* [zlib](https://www.zlib.net/) - zlib compression/decompression
* [zstd](https://facebook.github.io/zstd/) - Zstandard compression/decompression

## Licence

[MIT](https://opensource.org/licenses/MIT)

For 3rd-party licences, see [LICENSE](LICENSE).
The licencing information is considered to be part of the documentation.
