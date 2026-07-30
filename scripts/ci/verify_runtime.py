#!/usr/bin/env python3
"""Verify that an installed runtime is self-contained and loadable."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def run(command: list[str], *, cwd: Path, env: dict[str, str]) -> None:
    result = subprocess.run(command, cwd=cwd, env=env, text=True, capture_output=True)
    if result.returncode:
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        raise RuntimeError(f"command failed ({result.returncode}): {' '.join(command)}")


def require_file(path: Path) -> None:
    if not path.is_file():
        raise RuntimeError(f"required runtime file is missing: {path}")


def macos_loader_paths(path: Path) -> tuple[list[str], list[str]]:
    dependencies = subprocess.run(
        ["otool", "-L", str(path)], text=True, capture_output=True, check=False
    )
    if dependencies.returncode:
        raise RuntimeError(f"could not inspect dylib dependencies for {path}")

    loaded = []
    for line in dependencies.stdout.splitlines()[1:]:
        entry = line.strip().split(" (", 1)[0]
        if entry:
            loaded.append(entry)

    load_commands = subprocess.run(
        ["otool", "-l", str(path)], text=True, capture_output=True, check=False
    )
    if load_commands.returncode:
        raise RuntimeError(f"could not inspect loader paths for {path}")

    rpaths: list[str] = []
    command_is_rpath = False
    for line in load_commands.stdout.splitlines():
        stripped = line.strip()
        if stripped.startswith("cmd "):
            command_is_rpath = stripped == "cmd LC_RPATH"
        elif command_is_rpath and stripped.startswith("path "):
            rpaths.append(stripped[5:].split(" (", 1)[0])
            command_is_rpath = False
    return loaded, rpaths


def check_loader_paths(platform: str, files: list[Path]) -> None:
    if platform == "windows":
        return
    command = "readelf" if platform == "linux" else "otool"
    if not shutil.which(command):
        return
    for path in files:
        if platform == "macos":
            dependencies, rpaths = macos_loader_paths(path)
            for dependency in dependencies:
                if dependency.startswith(("/System/Library/", "/usr/lib/")):
                    continue
                if dependency.startswith("/"):
                    raise RuntimeError(
                        f"{path} loads a non-relocatable absolute dependency: {dependency}"
                    )
            for rpath in rpaths:
                if rpath.startswith("/"):
                    raise RuntimeError(f"{path} contains a non-relocatable absolute rpath: {rpath}")
            # otool echoes its command-line file path as the first output
            # line. Do not scan that heading: the intentional temporary copy
            # lives under the GitHub runner's work directory.
            continue
        args = [command, "-d", str(path)]
        result = subprocess.run(args, text=True, capture_output=True, check=False)
        if result.returncode:
            raise RuntimeError(f"could not inspect loader paths for {path}")
        lowered = result.stdout.lower()
        forbidden = ("vcpkg_installed", "/workspace/", "/users/runner/work/")
        if any(value in lowered for value in forbidden):
            raise RuntimeError(f"{path} contains a build-machine loader path")


def clean_loader_environment(platform: str) -> dict[str, str]:
    environment = os.environ.copy()
    prefix = "DYLD_" if platform == "macos" else "LD_"
    for key in list(environment):
        if key.startswith(prefix):
            environment.pop(key)
    if platform == "macos":
        # An unset fallback asks dyld to restore its default search path.
        # Keep it explicitly empty so the copied package cannot accidentally
        # resolve a library from the original build environment.
        environment["DYLD_FALLBACK_LIBRARY_PATH"] = ""
        environment["DYLD_FALLBACK_FRAMEWORK_PATH"] = ""
    return environment


def relocation_directory() -> tempfile.TemporaryDirectory[str]:
    runner_temp = os.environ.get("RUNNER_TEMP")
    location = runner_temp if runner_temp and Path(runner_temp).is_dir() else None
    return tempfile.TemporaryDirectory(prefix="simplegraphic-runtime-", dir=location)


def verify_runtime(host: Path, runtime: Path, platform: str, files: list[Path]) -> None:
    environment = clean_loader_environment(platform)
    if platform == "windows":
        environment["PATH"] = str(runtime) + os.pathsep + environment.get("PATH", "")
    run([str(host), "--version"], cwd=runtime, env=environment)
    run([str(host), "--smoke-modules"], cwd=runtime, env=environment)
    check_loader_paths(platform, files)


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

    runtime_files = [host, simplegraphic, *module_paths]
    if args.platform == "windows":
        verify_runtime(host, runtime, args.platform, runtime_files)
    else:
        # Execute a byte-for-byte copy from an unrelated working directory.
        # This catches dependencies that happen to resolve only while the
        # original build/stage tree remains available on the CI worker.
        with relocation_directory() as temporary:
            relocated_root = Path(temporary)
            relocated_runtime = relocated_root / "runtime"
            relocated_working_directory = relocated_root / "working-directory"
            shutil.copytree(runtime, relocated_runtime, symlinks=True)
            relocated_working_directory.mkdir()
            relocated_files = [
                relocated_runtime / path.relative_to(runtime) for path in runtime_files
            ]
            verify_runtime(
                relocated_runtime / host.name,
                relocated_working_directory,
                args.platform,
                relocated_files,
            )
    print(f"verified relocatable {args.platform} runtime: {runtime}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"runtime verification failed: {error}", file=sys.stderr)
        raise SystemExit(1)
