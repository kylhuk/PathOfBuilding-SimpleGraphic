#!/usr/bin/env python3
"""Verify that an installed runtime is self-contained and loadable."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
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


def staged_native_binaries(runtime: Path, platform: str) -> list[Path]:
    def native_binary(path: Path) -> bool:
        if not path.is_file():
            return False
        name = path.name.lower()
        if platform == "windows":
            return name.endswith(".dll")
        if platform == "macos":
            return name.endswith(".dylib") or ".so" in name
        return ".so" in name

    return sorted(path for path in runtime.rglob("*") if native_binary(path))


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


def macos_minimum_version(path: Path) -> str:
    load_commands = subprocess.run(
        ["otool", "-l", str(path)], text=True, capture_output=True, check=False
    )
    if load_commands.returncode:
        raise RuntimeError(f"could not inspect macOS deployment target for {path}")

    command = ""
    for line in load_commands.stdout.splitlines():
        stripped = line.strip()
        if stripped.startswith("cmd "):
            command = stripped[4:]
        elif command == "LC_BUILD_VERSION" and stripped.startswith("minos "):
            return stripped[6:].split(" ", 1)[0]
        elif command == "LC_VERSION_MIN_MACOSX" and stripped.startswith("version "):
            return stripped[8:].split(" ", 1)[0]
    raise RuntimeError(f"could not find a macOS deployment target in {path}")


def normalized_version(value: str) -> tuple[int, ...]:
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){0,2}", value):
        raise RuntimeError(f"invalid platform version emitted by the linker: {value!r}")
    parts = [int(part) for part in value.split(".")]
    while len(parts) > 1 and parts[-1] == 0:
        parts.pop()
    return tuple(parts)


def check_macos_deployment_target(files: list[Path], expected: str | None) -> None:
    if expected is None:
        return
    if not shutil.which("otool"):
        raise RuntimeError("otool is required to validate macOS deployment targets")
    expected_parts = normalized_version(expected)
    for path in files:
        actual = macos_minimum_version(path)
        if normalized_version(actual) > expected_parts:
            raise RuntimeError(
                f"{path} requires macOS {actual}, exceeding the macOS {expected} package baseline"
            )


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


def verify_runtime(
    host: Path,
    working_directory: Path,
    platform: str,
    files: list[Path],
    macos_deployment_target: str | None,
) -> None:
    environment = clean_loader_environment(platform)
    environment["PATH"] = str(host.parent) + os.pathsep + environment.get("PATH", "")
    # POSIX execution uses only the host's basename from an unrelated CWD.
    # This verifies that --smoke-modules finds the actual process image rather
    # than treating argv[0] as an absolute path. Use an absolute path on
    # Windows because subprocess cannot reliably search a replacement PATH
    # when shell=False there; the static contract covers its shared helper.
    executable = str(host) if platform == "windows" else host.name
    run([executable, "--version"], cwd=working_directory, env=environment)
    run([executable, "--smoke-modules"], cwd=working_directory, env=environment)
    check_loader_paths(platform, files)
    if platform == "macos":
        check_macos_deployment_target(files, macos_deployment_target)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--platform", required=True, choices=("windows", "linux", "macos"))
    parser.add_argument("--macos-deployment-target")
    args = parser.parse_args()
    if args.macos_deployment_target and args.platform != "macos":
        parser.error("--macos-deployment-target is only valid with --platform macos")

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

    # CMake stages the complete vcpkg runtime closure alongside the known
    # host/modules. Validate every dynamic binary, not just the ones this
    # script already knows by name, so a dependency cannot quietly require a
    # newer macOS release or a build-machine loader path.
    runtime_files = list(dict.fromkeys([
        host,
        simplegraphic,
        *module_paths,
        *staged_native_binaries(runtime, args.platform),
    ]))
    if args.platform == "windows":
        verify_runtime(
            host,
            runtime,
            args.platform,
            runtime_files,
            args.macos_deployment_target,
        )
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
                args.macos_deployment_target,
            )
    print(f"verified relocatable {args.platform} runtime: {runtime}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"runtime verification failed: {error}", file=sys.stderr)
        raise SystemExit(1)
