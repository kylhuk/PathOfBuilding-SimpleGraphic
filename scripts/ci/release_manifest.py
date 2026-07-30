#!/usr/bin/env python3
"""Write checksums and a small machine-readable release manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-sha", required=True)
    args = parser.parse_args()

    directory = args.directory.resolve()
    archives = sorted(
        (path for path in directory.rglob("*") if path.suffix == ".zip" or path.name.endswith(".tar.gz")),
        key=lambda path: path.name,
    )
    if len(archives) != 8:
        parser.error(f"expected exactly 8 runtime archives, found {len(archives)}")

    assets = []
    checksum_lines = []
    for archive in archives:
        checksum = sha256(archive)
        relative = archive.relative_to(directory).as_posix()
        checksum_lines.append(f"{checksum}  {relative}")
        assets.append({"file": relative, "sha256": checksum, "bytes": archive.stat().st_size})

    (directory / "SHA256SUMS.txt").write_text("\n".join(checksum_lines) + "\n", encoding="utf-8")
    (directory / "release-manifest.json").write_text(
        json.dumps(
            {
                "version": args.version,
                "source_sha": args.source_sha,
                "assets": assets,
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
