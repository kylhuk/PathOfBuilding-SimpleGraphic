#!/usr/bin/env python3
"""Fail closed on malformed release version input."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--root", type=Path, default=Path("."))
    args = parser.parse_args()
    if not re.fullmatch(
        r"[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?",
        args.version,
    ):
        print(
            "version must be SemVer without a leading v, for example 2.6.0 or 2.6.0-rc.1+build.5",
            file=sys.stderr,
        )
        return 2
    numeric_version = args.version.split("-", 1)[0].split("+", 1)[0]

    root = args.root.resolve()
    try:
        cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
        config = (root / "config.h").read_text(encoding="utf-8")
        manifest = json.loads((root / "vcpkg.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        print(f"could not read release version metadata: {error}", file=sys.stderr)
        return 2

    cmake_version = re.search(r"project\(PathOfBuildingSimpleGraphic\s+VERSION\s+([^\s)]+)", cmake)
    config_version = re.search(r'^\s*#define\s+CFG_VERSION_NUM\s+"([^"]+)"', config, re.MULTILINE)
    declared = {
        "CMake": cmake_version.group(1) if cmake_version else None,
        "config.h": config_version.group(1) if config_version else None,
        "vcpkg.json": manifest.get("version-semver"),
    }
    expected = {
        "CMake": numeric_version,
        "config.h": args.version,
        "vcpkg.json": args.version,
    }
    mismatched = {
        name: (expected[name], version)
        for name, version in declared.items()
        if version != expected[name]
    }
    if mismatched:
        rendered = ", ".join(
            f"{name}=expected {expected_version!r}, got {actual_version!r}"
            for name, (expected_version, actual_version) in mismatched.items()
        )
        print(f"release version {args.version!r} does not match project metadata ({rendered})", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
