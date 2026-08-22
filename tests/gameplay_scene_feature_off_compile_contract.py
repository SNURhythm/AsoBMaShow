#!/usr/bin/env python3
"""Compile GamePlayScene with Lua gameplay skins disabled."""

import argparse
import json
from pathlib import Path
import shlex
import subprocess
import sys


SOURCE_SUFFIX = "/src/scene/play/GamePlayScene.cpp"
FEATURE = "ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS"


def compile_arguments(entry: dict) -> list[str]:
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"], posix=True)


def feature_off(arguments: list[str]) -> list[str]:
    rewritten = []
    found = 0
    for argument in arguments:
        if argument.startswith((f"-D{FEATURE}=", f"/D{FEATURE}=")):
            prefix = argument[:2]
            rewritten.append(f"{prefix}{FEATURE}=0")
            found += 1
        else:
            rewritten.append(argument)
    if found != 1:
        raise RuntimeError(
            f"expected one {FEATURE} definition in the GamePlayScene compile command, found {found}"
        )
    return rewritten


def syntax_only(arguments: list[str]) -> list[str]:
    compiler = Path(arguments[0]).name.lower()
    msvc = compiler in {"cl", "cl.exe", "clang-cl", "clang-cl.exe"}
    output = []
    skip_next = False
    paired_outputs = {"-o", "-MF", "-MT", "-MQ", "-MJ"}
    msvc_paired_outputs = {"/Fo", "/Fd", "/Fa", "/sourceDependencies"}
    for argument in arguments:
        if skip_next:
            skip_next = False
            continue
        if msvc:
            lower = argument.lower()
            if lower == "/c":
                continue
            if any(lower == option.lower() for option in msvc_paired_outputs):
                skip_next = True
                continue
            if any(
                lower.startswith(option.lower()) and lower != option.lower()
                for option in msvc_paired_outputs
            ):
                continue
        else:
            if argument in paired_outputs:
                skip_next = True
                continue
            if argument in {"-c", "-MD", "-MMD", "-MP"}:
                continue
        output.append(argument)
    output.append("/Zs" if msvc else "-fsyntax-only")
    return output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    arguments = parser.parse_args()
    database_path = arguments.build_dir / "compile_commands.json"
    database = json.loads(database_path.read_text(encoding="utf-8"))
    matches = [
        entry
        for entry in database
        if Path(entry["file"]).as_posix().endswith(SOURCE_SUFFIX)
    ]
    if len(matches) != 1:
        print(
            f"error: expected one GamePlayScene compile command, found {len(matches)}",
            file=sys.stderr,
        )
        return 1
    entry = matches[0]
    command = syntax_only(feature_off(compile_arguments(entry)))
    result = subprocess.run(
        command,
        cwd=entry["directory"],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
