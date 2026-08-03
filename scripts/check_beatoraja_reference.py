#!/usr/bin/env python3
"""Read-only gate for the Beatoraja source snapshot used by skin evidence."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


PINNED_COMMIT = "c2ed5db1a46145ed10790c3872f717e95b59db9d"


def git(root: Path, *arguments: str) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), *arguments],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as error:
        raise RuntimeError(f"could not execute git: {error}") from error
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "git command failed"
        raise RuntimeError(detail)
    return result.stdout


def check_reference(root: Path, require_clean: bool) -> str:
    if not root.is_dir():
        raise RuntimeError(f"Beatoraja root is not a directory: {root}")
    commit = git(root, "rev-parse", "HEAD").strip()
    if commit != PINNED_COMMIT:
        raise RuntimeError(
            f"Beatoraja reference mismatch: expected {PINNED_COMMIT}, found {commit or '<empty>'}"
        )
    if require_clean:
        status = git(root, "status", "--porcelain")
        if status.strip():
            raise RuntimeError("Beatoraja reference has uncommitted changes")
    return commit


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--require-clean", action="store_true")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        commit = check_reference(arguments.root, arguments.require_clean)
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    suffix = " (clean)" if arguments.require_clean else ""
    print(f"Beatoraja reference verified: {commit}{suffix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
