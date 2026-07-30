#!/usr/bin/env python3
"""Compile the small host against stubs and exercise its path contracts."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def run(command: list[str], *, cwd: Path, env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, cwd=cwd, env=env, text=True, capture_output=True, check=False)
    if result.returncode:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def main() -> int:
    compiler = shutil.which("g++")
    if compiler is None:
        raise RuntimeError("g++ is required to test the standalone launcher")

    with tempfile.TemporaryDirectory(prefix="simplegraphic-launcher-") as temporary:
        root = Path(temporary)
        include = root / "include"
        runtime = root / "runtime"
        working_directory = root / "working-directory"
        include.mkdir()
        runtime.mkdir()
        working_directory.mkdir()
        (include / "config.h").write_text(
            '#define CFG_VERSION_NUM "2.6.0-rc.1+build.5"\n', encoding="utf-8"
        )
        (working_directory / "main.lua").write_text("-- launcher fixture\n", encoding="utf-8")
        stub = root / "runtime_stub.cpp"
        stub.write_text(
            r'''
#include <cstdlib>
#include <cstring>
#include <filesystem>

extern "C" int RunLuaFileAsWin(int argc, char** argv)
{
    if (argc != 2 || std::strcmp(argv[1], "preserved-argument") != 0) {
        return 81;
    }
    const auto* expected = std::getenv("SIMPLEGRAPHIC_TEST_SCRIPT");
    std::error_code error;
    if (expected == nullptr ||
        !std::filesystem::equivalent(std::filesystem::path(argv[0]), std::filesystem::path(expected), error) ||
        error) {
        return 82;
    }
    return 0;
}

extern "C" int SimpleGraphicRuntimeSmoke(const char* runtime_directory)
{
    const auto* expected = std::getenv("SIMPLEGRAPHIC_TEST_RUNTIME_DIR");
    std::error_code error;
    if (expected == nullptr ||
        !std::filesystem::equivalent(
            std::filesystem::path(runtime_directory), std::filesystem::path(expected), error) ||
        error) {
        return 91;
    }
    return 0;
}
''',
            encoding="utf-8",
        )
        host = runtime / "PathOfBuilding-SimpleGraphic"
        environment = os.environ.copy()
        environment["SIMPLEGRAPHIC_TEST_RUNTIME_DIR"] = str(runtime)
        environment["SIMPLEGRAPHIC_TEST_SCRIPT"] = str(working_directory / "main.lua")
        environment["PATH"] = str(runtime) + os.pathsep + environment.get("PATH", "")

        run(
            [
                compiler,
                "-std=c++17",
                f"-I{include}",
                f"-I{ROOT}",
                str(ROOT / "launcher/main.cpp"),
                str(stub),
                "-o",
                str(host),
            ],
            cwd=root,
            env=environment,
        )
        version = run([host.name, "--version"], cwd=working_directory, env=environment)
        if version.stdout.strip() != "2.6.0-rc.1+build.5":
            raise RuntimeError(f"launcher did not report the complete SemVer: {version.stdout!r}")
        run([host.name, "--smoke-modules"], cwd=working_directory, env=environment)
        run(
            [host.name, "./main.lua", "preserved-argument"],
            cwd=working_directory,
            env=environment,
        )

    print("launcher path and full-version contracts are valid")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"launcher test failed: {error}", file=sys.stderr)
        raise SystemExit(1)
