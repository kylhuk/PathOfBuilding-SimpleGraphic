#!/usr/bin/env python3
"""Create deterministic, installer-ready runtime archives."""

from __future__ import annotations

import argparse
import gzip
import os
from pathlib import Path
import tarfile
import time
import zipfile


# ZIP does not support timestamps before 1980. Using one fixed value makes
# identical staged payloads byte-for-byte reproducible regardless of runner.
ARCHIVE_TIMESTAMP = max(int(os.environ.get("SOURCE_DATE_EPOCH", "0")), 315532800)


def archive_paths(runtime: Path) -> list[Path]:
    paths = [path for path in runtime.rglob("*") if path.is_file() or path.is_symlink()]
    return sorted(paths, key=lambda path: path.relative_to(runtime).as_posix())


def zip_runtime(runtime: Path, output: Path, root_name: str) -> None:
    date_time = time.gmtime(ARCHIVE_TIMESTAMP)[:6]
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in archive_paths(runtime):
            relative = path.relative_to(runtime).as_posix()
            info = zipfile.ZipInfo(f"{root_name}/{relative}", date_time=date_time)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (path.stat().st_mode & 0xFFFF) << 16
            if path.is_symlink():
                # Windows payloads do not use symlinks. Fail explicitly rather
                # than silently serialising a link target as a regular file.
                raise ValueError(f"ZIP runtime unexpectedly contains symlink: {path}")
            with path.open("rb") as source:
                archive.writestr(info, source.read(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)


def tar_runtime(runtime: Path, output: Path, root_name: str) -> None:
    with output.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=ARCHIVE_TIMESTAMP) as compressed:
            with tarfile.open(fileobj=compressed, mode="w") as archive:
                for path in archive_paths(runtime):
                    relative = path.relative_to(runtime).as_posix()
                    info = archive.gettarinfo(str(path), arcname=f"{root_name}/{relative}")
                    info.mtime = ARCHIVE_TIMESTAMP
                    info.uid = 0
                    info.gid = 0
                    info.uname = ""
                    info.gname = ""
                    if info.isfile():
                        with path.open("rb") as source:
                            archive.addfile(info, source)
                    else:
                        archive.addfile(info)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--name", required=True)
    parser.add_argument("--format", required=True, choices=("zip", "tar.gz"))
    args = parser.parse_args()

    runtime = args.runtime.resolve()
    if not runtime.is_dir():
        parser.error(f"runtime directory does not exist: {runtime}")
    if not archive_paths(runtime):
        parser.error(f"runtime directory is empty: {runtime}")

    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    if args.format == "zip":
        zip_runtime(runtime, output, args.name)
    else:
        tar_runtime(runtime, output, args.name)
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
