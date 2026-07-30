#!/usr/bin/env python3
"""Exercise the release metadata tools against their published layout."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]

ARCHIVES = {
    "windows-x86": "PathOfBuilding-SimpleGraphic-windows-x86.zip",
    "windows-x64": "PathOfBuilding-SimpleGraphic-windows-x64.zip",
    "windows-arm64": "PathOfBuilding-SimpleGraphic-windows-arm64.zip",
    "macos-x64": "PathOfBuilding-SimpleGraphic-macos-x64.tar.gz",
    "macos-arm64": "PathOfBuilding-SimpleGraphic-macos-arm64.tar.gz",
    "linux-x86": "PathOfBuilding-SimpleGraphic-linux-x86.tar.gz",
    "linux-x64": "PathOfBuilding-SimpleGraphic-linux-x64.tar.gz",
    "linux-arm64": "PathOfBuilding-SimpleGraphic-linux-arm64.tar.gz",
}


def run(command: list[str], *, expect_success: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    if (result.returncode == 0) != expect_success:
        raise RuntimeError(
            f"unexpected result ({result.returncode}) for {' '.join(command)}:\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_release_metadata(root: Path, *, cmake: str, config: str, vcpkg: str) -> None:
    (root / "CMakeLists.txt").write_text(
        f"project(PathOfBuildingSimpleGraphic VERSION {cmake} LANGUAGES C CXX)\n",
        encoding="utf-8",
    )
    (root / "config.h").write_text(f'#define CFG_VERSION_NUM "{config}"\n', encoding="utf-8")
    (root / "vcpkg.json").write_text(json.dumps({"version-semver": vcpkg}), encoding="utf-8")


def test_manifest_uses_published_asset_names() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        artifacts = root / "artifacts"
        for target, name in ARCHIVES.items():
            archive = artifacts / f"release-v2.6.0-{target}" / name
            archive.parent.mkdir(parents=True, exist_ok=True)
            archive.write_bytes(f"{target} payload".encode("utf-8"))

        run([
            sys.executable,
            str(ROOT / "scripts/ci/release_manifest.py"),
            "--directory", str(artifacts),
            "--version", "2.6.0",
            "--source-sha", "a" * 40,
        ])

        manifest = json.loads((artifacts / "release-manifest.json").read_text(encoding="utf-8"))
        expected_names = set(ARCHIVES.values())
        manifest_names = {asset["file"] for asset in manifest["assets"]}
        if manifest_names != expected_names or any("/" in name for name in manifest_names):
            raise RuntimeError("manifest paths do not match flat GitHub release asset names")

        checksums = {}
        for line in (artifacts / "SHA256SUMS.txt").read_text(encoding="utf-8").splitlines():
            checksum, name = line.split("  ", 1)
            checksums[name] = checksum
        if set(checksums) != expected_names or any("/" in name for name in checksums):
            raise RuntimeError("checksum paths do not match flat GitHub release asset names")

        published = root / "published"
        published.mkdir()
        for archive in artifacts.rglob("*"):
            if archive.is_file() and archive.name in expected_names:
                shutil.copy2(archive, published / archive.name)
        for name, checksum in checksums.items():
            if digest(published / name) != checksum:
                raise RuntimeError(f"checksum does not validate published asset {name}")


def test_prerelease_versions() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        command = [sys.executable, str(ROOT / "scripts/ci/check_release.py"), "--root", str(root)]

        write_release_metadata(root, cmake="2.6.0", config="2.6.0", vcpkg="2.6.0")
        run([*command, "--version", "2.6.0"])

        write_release_metadata(root, cmake="2.6.0", config="2.6.0-rc.1", vcpkg="2.6.0-rc.1")
        run([*command, "--version", "2.6.0-rc.1"])

        write_release_metadata(
            root,
            cmake="2.6.0",
            config="2.6.0-rc.1+build.5",
            vcpkg="2.6.0-rc.1+build.5",
        )
        run([*command, "--version", "2.6.0-rc.1+build.5"])

        write_release_metadata(root, cmake="2.6.0", config="2.6.0", vcpkg="2.6.0")
        run([*command, "--version", "2.6.0-rc.1"], expect_success=False)

        write_release_metadata(root, cmake="2.6.1", config="2.6.0-rc.1", vcpkg="2.6.0-rc.1")
        run([*command, "--version", "2.6.0-rc.1"], expect_success=False)


def test_semver_syntax() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        command = [sys.executable, str(ROOT / "scripts/ci/check_release.py"), "--root", str(root)]

        valid_versions = [
            "0.0.0",
            "2.6.0",
            "2.6.0-0",
            "2.6.0-0A",
            "2.6.0-rc.1+build.5",
            "2.6.0+001",
        ]
        invalid_versions = [
            "02.6.0",
            "2.06.0",
            "2.6.00",
            "2.6.0-",
            "2.6.0-rc.",
            "2.6.0-.rc",
            "2.6.0-rc..1",
            "2.6.0+",
            "2.6.0+build.",
            "2.6.0+.build",
            "2.6.0+build..5",
            "2.6.0-01",
            "2.6.0-rc.01",
            "2.6.0-01+build.5",
        ]

        for version in valid_versions:
            numeric_version = version.split("-", 1)[0].split("+", 1)[0]
            write_release_metadata(root, cmake=numeric_version, config=version, vcpkg=version)
            run([*command, "--version", version])

        # Keep metadata aligned with each input so an accidental acceptance
        # cannot be hidden by a later metadata-mismatch failure.
        for version in invalid_versions:
            numeric_version = version.split("-", 1)[0].split("+", 1)[0]
            write_release_metadata(root, cmake=numeric_version, config=version, vcpkg=version)
            run([*command, "--version", version], expect_success=False)


def main() -> int:
    test_manifest_uses_published_asset_names()
    test_prerelease_versions()
    test_semver_syntax()
    print("release metadata tools are valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
