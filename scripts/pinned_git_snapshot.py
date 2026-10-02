"""Read historical reference files without changing a developer's checkout."""

from __future__ import annotations

from contextlib import contextmanager
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile
import tempfile


def verify_pinned_commit(repository: Path, commit: str) -> None:
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise RuntimeError("reference commit must be a full SHA-1")
    result = subprocess.run(
        ["git", "-C", str(repository), "rev-parse", "--verify", f"{commit}^{{commit}}"],
        capture_output=True, text=True, check=False,
    )
    if result.returncode != 0 or result.stdout.strip() != commit:
        raise RuntimeError(f"reference repository does not contain pinned commit {commit}")


@contextmanager
def pinned_git_snapshot(repository: Path, commit: str, paths: tuple[str, ...]):
    verify_pinned_commit(repository, commit)
    with tempfile.TemporaryDirectory(prefix="asobmashow-pinned-reference-") as directory:
        root = Path(directory)
        with tempfile.TemporaryFile() as archive:
            result = subprocess.run(
                ["git", "-C", str(repository), "archive", "--format=tar", commit, "--", *paths],
                stdout=archive, stderr=subprocess.PIPE, check=False,
            )
            if result.returncode != 0:
                raise RuntimeError(result.stderr.decode("utf-8", errors="replace"))
            archive.seek(0)
            with tarfile.open(fileobj=archive) as contents:
                for member in contents:
                    relative = PurePosixPath(member.name)
                    if relative.is_absolute() or ".." in relative.parts:
                        raise RuntimeError(f"unsafe reference path: {member.name}")
                    destination = root.joinpath(*relative.parts)
                    if member.isdir():
                        destination.mkdir(parents=True, exist_ok=True)
                    elif member.isfile():
                        destination.parent.mkdir(parents=True, exist_ok=True)
                        with contents.extractfile(member) as source, destination.open("wb") as target:
                            shutil.copyfileobj(source, target)
                    else:
                        raise RuntimeError(f"unsupported reference entry: {member.name}")
        yield root
