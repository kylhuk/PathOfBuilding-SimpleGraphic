#!/usr/bin/env python3
"""Verify that an installed runtime is self-contained and loadable."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


def run(command: list[str], *, cwd: Path, env: dict[str, str]) -> None:
    result = subprocess.run(command, cwd=cwd, env=env, text=True, capture_output=True)
    if result.returncode:
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        raise RuntimeError(f"command failed ({result.returncode}): {' '.join(command)}")


def require_file(path: Path) -> None:
    if not path.is_file():
        raise RuntimeError(f"required runtime file is missing: {path}")


def check_loader_paths(platform: str, files: list[Path]) -> None:
    command = "readelf" if platform == "linux" else "otool"
    if platform == "windows" or not shutil.which(command):
        return
    for path in files:
        args = [command, "-d", str(path)] if platform == "linux" else [command, "-l", str(path)]
        result = subprocess.run(args, text=True, capture_output=True, check=False)
        if result.returncode:
            raise RuntimeError(f"could not inspect loader paths for {path}")
        lowered = result.stdout.lower()
        forbidden = ("vcpkg_installed", "/workspace/", "/users/runner/work/")
        if any(value in lowered for value in forbidden):
            raise RuntimeError(f"{path} contains a build-machine loader path")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--platform", required=True, choices=("windows", "linux", "macos"))
    args = parser.parse_args()

    runtime = args.runtime.resolve()
    if not runtime.is_dir():
        parser.error(f"runtime directory does not exist: {runtime}")
    extension = ".dll" if args.platform == "windows" else ".so"
    host = runtime / ("PathOfBuilding-SimpleGraphic.exe" if args.platform == "windows" else "PathOfBuilding-SimpleGraphic")
    simplegraphic = runtime / (
        "SimpleGraphic.dll" if args.platform == "windows" else "SimpleGraphic.dylib" if args.platform == "macos" else "SimpleGraphic.so"
    )
    module_paths = [
        runtime / "lua" / f"lua-utf8{extension}",
        runtime / "lua" / f"lzip{extension}",
        runtime / "lua" / f"lcurl{extension}",
        runtime / "lua" / "lcurl" / f"safe{extension}",
        runtime / "lua" / "socket" / f"core{extension}",
        runtime / "lua" / "mime" / f"core{extension}",
    ]
    if args.platform != "windows":
        module_paths.append(runtime / "lua" / "socket" / f"unix{extension}")
    lua_paths = [runtime / "lua" / name for name in ("ltn12.lua", "mime.lua", "socket.lua", "cURL.lua")]
    for path in [host, simplegraphic, *module_paths, *lua_paths]:
        require_file(path)

    environment = os.environ.copy()
    if args.platform == "windows":
        environment["PATH"] = str(runtime) + os.pathsep + environment.get("PATH", "")
    else:
        # This must be a genuinely relocatable package test. Supplying a
        # library-path override would hide a missing $ORIGIN/@loader_path from
        # the staged payload and let an unpacked release fail for users.
        environment.pop("LD_LIBRARY_PATH", None)
        environment.pop("DYLD_LIBRARY_PATH", None)
        environment.pop("DYLD_FALLBACK_LIBRARY_PATH", None)
    run([str(host), "--version"], cwd=runtime, env=environment)
    run([str(host), "--smoke-modules"], cwd=runtime, env=environment)
    check_loader_paths(args.platform, [host, simplegraphic, *module_paths])
    print(f"verified staged {args.platform} runtime: {runtime}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"runtime verification failed: {error}", file=sys.stderr)
        raise SystemExit(1)
