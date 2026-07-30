#!/usr/bin/env python3
"""Fast, dependency-free checks for the reproducible build contract."""

from __future__ import annotations

import json
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]


def require(path: Path) -> str:
    if not path.is_file():
        raise RuntimeError(f"required file is missing: {path.relative_to(ROOT)}")
    return path.read_text(encoding="utf-8")


def main() -> int:
    try:
        cmake = require(ROOT / "CMakeLists.txt")
        presets = json.loads(require(ROOT / "CMakePresets.json"))
        configuration = json.loads(require(ROOT / "vcpkg-configuration.json"))
        build_runtime = require(ROOT / ".github/workflows/build-runtime.yml")
        require(ROOT / ".github/workflows/dev-build.yml")
        release = require(ROOT / ".github/workflows/release.yml")
        require(ROOT / ".github/workflows/validate.yml")
        require(ROOT / ".github/dependabot.yml")

        baseline = configuration["default-registry"]["baseline"]
        if not re.fullmatch(r"[0-9a-f]{40}", baseline):
            raise RuntimeError("vcpkg baseline must be an immutable 40-character commit")
        if "vcpkg-triplets" not in configuration.get("overlay-triplets", []):
            raise RuntimeError("vcpkg dynamic triplet overlay is not configured")
        if "RUNTIME_DEPENDENCY_SET simplegraphic_runtime_dependencies" not in cmake:
            raise RuntimeError("CMake does not stage transitive runtime dependencies")
        if "if (UNIX AND NOT APPLE)\n    target_compile_definitions(SimpleGraphic PRIVATE _POSIX_C_SOURCE=200809L)" not in cmake:
            raise RuntimeError("Darwin must not receive Linux POSIX feature macros")
        if "MACOSX_RPATH ON" not in cmake or 'INSTALL_NAME_DIR "@rpath"' not in cmake:
            raise RuntimeError("macOS dylibs do not have a relocatable install name")
        if "GLM_ENABLE_EXPERIMENTAL" not in cmake:
            raise RuntimeError("CMake does not opt in to GLM extensions required by GLI")
        gli_portfile = require(ROOT / "vcpkg-ports/ports/gli/2021-07-06_3/portfile.cmake")
        gli_patch = require(ROOT / "vcpkg-ports/ports/gli/2021-07-06_3/qualify-make-vec4.patch")
        if "qualify-make-vec4.patch" not in gli_portfile or gli_patch.count("gli::make_vec4") != 4:
            raise RuntimeError("GLI is not patched for the current GLM make_vec4 API")
        if "#include <fmt/format.h>" not in require(ROOT / "engine/core/core_config.cpp"):
            raise RuntimeError("core config does not include fmt's formatting API")
        common = require(ROOT / "engine/common/common.cpp")
        if "VFormatString" not in common or "vasprintf" in common:
            raise RuntimeError("shared printf formatting is not portable")
        if any("vasprintf" in require(path) for path in (
            ROOT / "engine/common/console.cpp",
            ROOT / "engine/system/win/sys_main.cpp",
        )):
            raise RuntimeError("GNU-only vasprintf remains in a portable source path")
        system_main = require(ROOT / "engine/system/win/sys_main.cpp")
        if system_main.count("std::nullopt, std::string{") != 2:
            raise RuntimeError("user-path errors do not use portable optional construction")
        if 'Error("%s", threadError);' not in system_main:
            raise RuntimeError("thread errors are passed as an unsafe format string")
        if "sys->Sleep(1);" not in require(ROOT / "engine/render/r_main.cpp"):
            raise RuntimeError("renderer does not use the platform-neutral sleep API")
        # LuaSocket intentionally has both socket.core and mime.core. Their
        # identical filenames need separate build directories, not only
        # separate install destinations, or Ninja rejects the generated graph.
        for output_dir in ("${CMAKE_CURRENT_BINARY_DIR}/lua/socket", "${CMAKE_CURRENT_BINARY_DIR}/lua/mime"):
            if output_dir not in cmake:
                raise RuntimeError("LuaSocket core modules do not have distinct build output directories")
        if "runtime_smoke.cpp" not in cmake or "SimpleGraphicRuntimeSmoke" not in require(ROOT / "runtime_smoke.cpp") or "--smoke-modules" not in require(ROOT / "launcher/main.cpp"):
            raise RuntimeError("staged runtime smoke coverage is missing")
        luajit_configure = require(ROOT / "vcpkg-ports/ports/luajit/2026-07-20_1/configure")
        if "'LJ_TARGET_ARM 1'" not in luajit_configure or "'LJ_TARGET_X86 1'" not in luajit_configure:
            raise RuntimeError("LuaJIT's 32-bit manual buildvm architecture tokens are incomplete")
        if '"-DCMAKE_INSTALL_PREFIX=$stage"' not in build_runtime:
            raise RuntimeError("Windows runtime staging does not expand its CMake install prefix")
        if "workflow_dispatch:" not in release or "release:" in release.split("on:", 1)[1].split("permissions:", 1)[0]:
            raise RuntimeError("release workflow must be manual-only")
        if (ROOT / ".github/workflows/main.yml").exists():
            raise RuntimeError("legacy automatic release workflow remains")
        if "lukka/run-vcpkg" in "\n".join(
            path.read_text(encoding="utf-8") for path in (ROOT / ".github/workflows").glob("*.yml")
        ):
            raise RuntimeError("mutable run-vcpkg action remains")
        names = {preset["name"] for preset in presets["configurePresets"] if not preset.get("hidden")}
        expected = {
            "windows-x86", "windows-x64", "windows-arm64",
            "macos-x64", "macos-arm64",
            "linux-x86", "linux-x64", "linux-arm64",
        }
        if names != expected:
            raise RuntimeError(f"CMake preset matrix is incomplete: expected {sorted(expected)}, got {sorted(names)}")
        print("source, presets, package staging, and manual-release contract are valid")
        return 0
    except (KeyError, RuntimeError, json.JSONDecodeError) as error:
        print(f"validation failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
