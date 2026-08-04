#!/usr/bin/env python3
"""Verify the exact portable shader artifacts for one gameplay skin shader."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path
from typing import NoReturn


KNOWN_BACKENDS = ("metal", "spirv", "essl", "dx11")
PINNED_BEATORAJA_COMMIT = "c2ed5db1a46145ed10790c3872f717e95b59db9d"


def fail(message: str) -> NoReturn:
    print(f"skin shader verification failed: {message}", file=sys.stderr)
    raise SystemExit(1)


def digest(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def require_file(root: Path, relative: str) -> Path:
    path = root / relative
    if not path.is_file():
        fail(f"missing required file: {relative}")
    if path.stat().st_size == 0:
        fail(f"required file is empty: {relative}")
    return path


def changed_shader_paths(root: Path) -> set[str]:
    if not (root / ".git").exists() and not any(
        parent.joinpath(".git").exists() for parent in root.parents
    ):
        return set()
    commands = (
        ["git", "diff", "--name-only", "--", "shader_src", "shaders"],
        [
            "git",
            "diff",
            "--cached",
            "--name-only",
            "--",
            "shader_src",
            "shaders",
        ],
        [
            "git",
            "ls-files",
            "--others",
            "--exclude-standard",
            "--",
            "shader_src",
            "shaders",
        ],
    )
    result: set[str] = set()
    for command in commands:
        completed = subprocess.run(
            command, cwd=root, text=True, capture_output=True, check=False
        )
        if completed.returncode != 0:
            fail(f"could not inspect shader-tree changes: {completed.stderr.strip()}")
        result.update(line for line in completed.stdout.splitlines() if line)
    return result


def manifest_data(
    shader: str,
    sources: dict[str, dict[str, object]],
    outputs: dict[str, dict[str, object]],
) -> dict[str, object]:
    return {
        "format": 1,
        "shader": shader,
        "beatoraja_reference": {
            "commit": PINNED_BEATORAJA_COMMIT,
            "paths": [
                "src/bms/player/beatoraja/skin/Skin.java",
                "src/bms/player/beatoraja/skin/SkinObject.java",
                "src/bms/player/beatoraja/skin/SkinText.java",
            ],
            "compatibility_notes": [
                "Blend ID 3 uses the documented source-minus-destination state; the pinned renderer restores ADD before the current sprite and is treated as an upstream state-order bug.",
                "Nearest/linear sampling is isolated per command with clamp-to-edge instead of mutating a shared texture filter.",
                "Glyph runs bind blend state explicitly instead of inheriting the preceding sprite state.",
                "Destination rotation and clip geometry are consumed after command lowering and are not applied a second time by the backend.",
            ],
        },
        "sources": sources,
        "outputs": outputs,
    }


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--shader", required=True)
    parser.add_argument(
        "--require-backends",
        default=",".join(KNOWN_BACKENDS),
        help="comma-separated backend directories",
    )
    manifest = parser.add_mutually_exclusive_group()
    manifest.add_argument("--write-manifest", type=Path)
    manifest.add_argument("--manifest", type=Path)
    parser.add_argument("--changed-path", action="append", default=[])
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    root = arguments.root.resolve()
    shader = arguments.shader
    backends = tuple(
        backend.strip()
        for backend in arguments.require_backends.split(",")
        if backend.strip()
    )
    if not backends:
        fail("at least one backend is required")
    unknown = sorted(set(backends) - set(KNOWN_BACKENDS))
    if unknown:
        fail(f"unknown backend(s): {', '.join(unknown)}")

    source_paths = (
        f"shader_src/vs_{shader}.sc",
        f"shader_src/fs_{shader}.sc",
    )
    output_paths = tuple(
        f"shaders/{backend}/{stage}_{shader}.bin"
        for backend in backends
        for stage in ("vs", "fs")
    )
    sources = {relative: digest(require_file(root, relative)) for relative in source_paths}
    outputs = {relative: digest(require_file(root, relative)) for relative in output_paths}

    allowed_changes = set(source_paths)
    allowed_changes.update(
        f"shaders/{backend}/{stage}_{shader}.bin"
        for backend in KNOWN_BACKENDS
        for stage in ("vs", "fs")
    )
    # Explicit paths are additive evidence for fixtures/CI; they must never
    # suppress inspection of the real worktree when one is available.
    actual_changes = set(arguments.changed_path)
    actual_changes.update(changed_shader_paths(root))
    unexpected = sorted(actual_changes - allowed_changes)
    if unexpected:
        fail("unexpected shader-tree change: " + ", ".join(unexpected))

    current = manifest_data(shader, sources, outputs)
    if arguments.write_manifest is not None:
        path = arguments.write_manifest
        if not path.is_absolute():
            path = root / path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps(current, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )

    if arguments.manifest is not None:
        path = arguments.manifest
        if not path.is_absolute():
            path = root / path
        if not path.is_file():
            fail(f"manifest is missing: {path}")
        try:
            recorded = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            fail(f"manifest could not be read: {error}")
        if recorded != current:
            fail("manifest hashes or metadata do not match current shader outputs")

    for relative in output_paths:
        print(f"verified {relative} ({outputs[relative]['bytes']} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
