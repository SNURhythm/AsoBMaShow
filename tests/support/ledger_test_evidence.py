#!/usr/bin/env python3
"""Share successful native ledger proofs through CTest setup fixtures."""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


EVIDENCE_DIRECTORY_ENV = "ASOBMASHOW_LEDGER_EVIDENCE_DIR"
ROOT = Path(__file__).resolve().parents[2]


def validate_payload(payload, runner):
    assert isinstance(payload, dict) and payload.get("runner") == runner, (
        f"{runner} emitted evidence for a different runner"
    )
    identifiers = payload.get("assertionIds")
    assert isinstance(identifiers, list) and identifiers and all(
        isinstance(identifier, str) and identifier for identifier in identifiers
    ), f"{runner} emitted invalid assertion IDs"
    assert len(identifiers) == len(set(identifiers)), (
        f"{runner} emitted duplicate assertion IDs"
    )
    return payload


def parse_output(output, runner):
    try:
        payload = json.loads(output.splitlines()[-1])
    except (IndexError, json.JSONDecodeError) as error:
        raise AssertionError(
            f"{runner} emitted no machine-readable ledger evidence"
        ) from error
    return validate_payload(payload, runner)


def executable_identity(executable):
    executable = executable.resolve()
    stat = executable.stat()
    return {
        "path": str(executable),
        "size": stat.st_size,
        "mtimeNs": stat.st_mtime_ns,
    }


def run_native(executable, cwd=ROOT):
    return subprocess.run(
        [str(executable), "--list-ledger-assertions"],
        cwd=cwd,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def record_evidence(executable, runner, directory):
    directory.mkdir(parents=True, exist_ok=True)
    destination = directory / f"{runner}.json"
    # A failed or interrupted fixture must never leave yesterday's proof usable.
    destination.unlink(missing_ok=True)
    identity = executable_identity(executable)
    completed = run_native(executable, cwd=None)
    print(completed.stdout, end="", flush=True)
    if completed.returncode != 0:
        return completed.returncode if completed.returncode > 0 else 1
    payload = parse_output(completed.stdout, runner)
    assert executable_identity(executable) == identity, (
        f"{runner} changed while producing ledger evidence"
    )
    destination.write_text(json.dumps({
        "schemaVersion": 1,
        "executable": identity,
        "payload": payload,
    }) + "\n", encoding="utf-8")
    return 0


def executed_coverage(build_dir, runners):
    evidence_directory = os.environ.get(EVIDENCE_DIRECTORY_ENV)
    emitted = {}
    for runner in sorted(runners):
        executable = build_dir / (runner + (".exe" if os.name == "nt" else ""))
        assert executable.is_file(), f"executed ledger evidence is unbuilt: {runner}"
        if evidence_directory is not None:
            # CTest requires this runner's successful setup fixture for this run.
            # Missing or invalid fixture output is a failure, never a cache miss.
            source = Path(evidence_directory) / f"{runner}.json"
            try:
                evidence = json.loads(source.read_text(encoding="utf-8"))
            except (OSError, ValueError) as error:
                raise AssertionError(f"missing or invalid ledger evidence: {source}") from error
            assert isinstance(evidence, dict) and evidence.get("schemaVersion") == 1, (
                f"unsupported ledger evidence: {source}"
            )
            assert evidence.get("executable") == executable_identity(executable), (
                f"ledger evidence is stale or belongs to another executable: {runner}"
            )
            payload = validate_payload(evidence.get("payload"), runner)
        else:
            completed = run_native(executable)
            assert completed.returncode == 0, (
                f"executed ledger evidence failed: {runner}\n{completed.stdout}"
            )
            payload = parse_output(completed.stdout, runner)
        emitted[runner] = payload["assertionIds"]
    return emitted


def main():
    # Native SDL diagnostics use UTF-8 even on non-UTF-8 Windows locales.
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runners", type=Path)
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--runner")
    parser.add_argument("--evidence-dir", type=Path)
    arguments = parser.parse_args()
    if arguments.runners is not None:
        manifest = json.loads(arguments.runners.read_text(encoding="utf-8"))
        print(";".join(sorted({
            row["assertion"]["runner"] for row in manifest["features"]
            if row["status"] == "implemented"
        })))
        return 0
    if not all((arguments.executable, arguments.runner, arguments.evidence_dir)):
        parser.error("--executable, --runner and --evidence-dir are required")
    return record_evidence(
        arguments.executable, arguments.runner, arguments.evidence_dir
    )


if __name__ == "__main__":
    sys.exit(main())
