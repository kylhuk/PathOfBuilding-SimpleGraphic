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
        require(ROOT / ".github/workflows/build-runtime.yml")
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
        # LuaSocket intentionally has both socket.core and mime.core. Their
        # identical filenames need separate build directories, not only
        # separate install destinations, or Ninja rejects the generated graph.
        for output_dir in ("${CMAKE_CURRENT_BINARY_DIR}/lua/socket", "${CMAKE_CURRENT_BINARY_DIR}/lua/mime"):
            if output_dir not in cmake:
                raise RuntimeError("LuaSocket core modules do not have distinct build output directories")
        if "runtime_smoke.cpp" not in cmake or "SimpleGraphicRuntimeSmoke" not in require(ROOT / "runtime_smoke.cpp") or "--smoke-modules" not in require(ROOT / "launcher/main.cpp"):
            raise RuntimeError("staged runtime smoke coverage is missing")
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
