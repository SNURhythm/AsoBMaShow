#!/usr/bin/env python3
"""Sort catalog entry lines without rewriting translation text or staging drafts."""

import argparse
from pathlib import Path
import re
import subprocess
import sys


CATALOG = "src/i18n/Messages.inc"
ENTRY = re.compile(rb'^\s*\{\s*"([^"\r\n]+)"')


def sorted_catalog(data):
    lines = data.splitlines(keepends=True)
    entries = []
    slots = []
    for index, line in enumerate(lines):
        match = ENTRY.match(line)
        if match:
            body = line.rstrip(b"\r\n")
            entries.append((match[1], body))
            slots.append((index, line[len(body):]))
    for (index, ending), (_, body) in zip(slots, sorted(entries, key=lambda entry: entry[0])):
        lines[index] = body + ending
    return b"".join(lines)


def git(*args, data=None):
    return subprocess.check_output(["git", *args], input=data)


def sort_staged():
    changed = subprocess.run(["git", "diff", "--cached", "--quiet", "--", CATALOG])
    if changed.returncode == 0:
        return
    if changed.returncode != 1:
        changed.check_returncode()
    entry = git("ls-files", "--stage", "-z", "--", CATALOG)
    if not entry:  # Deleting the catalog does not require sorting.
        return
    mode, _, stage = entry.split(b"\t", 1)[0].split()
    if stage != b"0" or mode not in (b"100644", b"100755"):
        raise ValueError("the message catalog must be an unconflicted regular file")
    staged = git("show", ":" + CATALOG)
    ordered = sorted_catalog(staged)
    working_file = Path(git("rev-parse", "--show-toplevel").decode().rstrip("\r\n")) / CATALOG
    working = (working_file.read_bytes()
               if working_file.is_file() and not working_file.is_symlink() else None)
    if ordered != staged:
        blob = git("hash-object", "-w", "--stdin", data=ordered).decode().strip()
        git("update-index", "--cacheinfo", mode.decode() + "," + blob + "," + CATALOG)
        print("Sorted staged translation keys in " + CATALOG)
    # Sort the working copy independently: never git-add its unstaged content.
    if working is not None:
        ordered_working = sorted_catalog(working)
        if ordered_working != working:
            working_file.write_bytes(ordered_working)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--staged", action="store_true", help="sort the staged catalog for pre-commit")
    args = parser.parse_args()
    if args.staged:
        sort_staged()
    else:
        catalog = Path(__file__).resolve().parents[1] / CATALOG
        original = catalog.read_bytes()
        ordered = sorted_catalog(original)
        if ordered != original:
            catalog.write_bytes(ordered)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print("Translation key sorting failed: " + str(error), file=sys.stderr)
        sys.exit(1)
