#!/usr/bin/env python3
"""Generate the pinned Beatoraja gameplay-skin differential trace."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PINNED_COMMIT = "c2ed5db1a46145ed10790c3872f717e95b59db9d"
HARNESS = ROOT / "tests/fixtures/beatoraja_skin/oracle/GameplaySkinOracle.java"
DEFAULT_OUTPUT = ROOT / "tests/fixtures/beatoraja_skin/traces/gameplay_objects_pinned_v1.json"
LEDGER = ROOT / "docs/skin-compat/beatoraja-gameplay-feature-ledger-v1.json"
FIXTURES = (
    "tests/fixtures/beatoraja_skin/lua/model/all_v1_objects.luaskin",
    "tests/fixtures/beatoraja_skin/json/all_gameplay_fields.json",
    "tests/fixtures/beatoraja_skin/lr2/all_gameplay_objects.lr2skin",
    "tests/fixtures/beatoraja_skin/lr2/all_play_commands.lr2skin",
)
EXCLUDED_SOURCE_SUFFIXES = (
    "/bms/player/beatoraja/MainLoader.java",
    "/bms/player/beatoraja/play/LaneRenderer.java",
)
EXCLUDED_SOURCE_PART = "/bms/player/beatoraja/launcher/"
REQUIRED_SOURCE_MARKERS = {
    "src/bms/player/beatoraja/skin/lr2/LR2PlaySkinLoader.java": (
        "new SkinNoteDistributionGraph(values[1], values[15]",
        "new SkinBPMGraph(values[3], values[4]",
        "new SkinTimingVisualizer(values[4], values[6]",
    ),
    "src/bms/player/beatoraja/skin/SkinTimingVisualizer.java": (
        "this.judgeWidthRate = width / (float) (judgeWidthMillis * 2 + 1)",
        "static String colorStringValidation",
    ),
    "src/bms/player/beatoraja/skin/SkinHitErrorVisualizer.java": (
        "private void updateEMA",
        "MathUtils.clamp(windowLength, 1, 100)",
    ),
}
CASE_TOLERANCE = {
    "selector.master-volume": 1e-7,
    "json.timing-visualizer": 1e-6,
    "json.hit-error-visualizer": 1e-6,
    "lr2.timing": 1e-6,
}

MAIN_LOADER_STUB = """
package bms.player.beatoraja;
public final class MainLoader {
  public static int getIllegalSongCount() { return 0; }
  public static String[] getIllegalSongs() { return new String[0]; }
}
"""

LANE_RENDERER_STUB = """
package bms.player.beatoraja.play;
import bms.model.BMSModel;
import bms.player.beatoraja.PlayConfig;
import bms.player.beatoraja.skin.Skin;
import bms.player.beatoraja.skin.Skin.SkinObjectRenderer;
import bms.player.beatoraja.skin.SkinObject.SkinOffset;
import bms.player.beatoraja.play.SkinNote.SkinLane;
public class LaneRenderer {
  private final PlayConfig config = new PlayConfig();
  public LaneRenderer(BMSPlayer main, BMSModel model) {}
  public void init(BMSModel model) {}
  public float getHispeed() { return 1; }
  public int getDuration() { return 0; }
  public void setDuration(int value) {}
  public int getCurrentDuration() { return 0; }
  public float getHispeedmargin() { return 0; }
  public void setHispeedmargin(float value) {}
  public boolean isEnableLift() { return false; }
  public float getLiftRegion() { return 0; }
  public void setLiftRegion(float value) {}
  public float getLanecover() { return 0; }
  public void resetHispeed(double bpm) {}
  public void setLanecover(float value) {}
  public void setEnableLanecover(boolean value) {}
  public boolean isEnableLanecover() { return false; }
  public float getHiddenCover() { return 0; }
  public void setHiddenCover(float value) {}
  public boolean isEnableHidden() { return false; }
  public void changeHispeed(boolean up) {}
  public PlayConfig getPlayConfig() { return config; }
  public void drawLane(SkinObjectRenderer r, long t, SkinLane[] l, SkinOffset[] o) {}
  public double getNowBPM() { return 120; }
  public double getMinBPM() { return 120; }
  public double getMaxBPM() { return 120; }
  public double getMainBPM() { return 120; }
  public void dispose() {}
}
"""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(*arguments: str, cwd: Path, timeout: int = 90) -> str:
    result = subprocess.run(
        arguments,
        cwd=cwd,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
        check=False,
    )
    if len(result.stdout) + len(result.stderr) > 2 * 1024 * 1024:
        raise RuntimeError(f"subprocess output exceeds 2 MiB: {arguments[0]}")
    output = result.stdout.decode("utf-8", errors="replace")
    if result.returncode != 0:
        errors = result.stderr.decode("utf-8", errors="replace")
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(arguments)}\n{output}{errors}"
        )
    return output


def validate_reference(root: Path) -> list[Path]:
    if run("git", "rev-parse", "HEAD", cwd=root, timeout=10).strip() != PINNED_COMMIT:
        raise RuntimeError(f"Beatoraja reference must be exactly {PINNED_COMMIT}")
    if run("git", "status", "--porcelain", cwd=root, timeout=10).strip():
        raise RuntimeError("Beatoraja reference must be clean")
    for relative, markers in REQUIRED_SOURCE_MARKERS.items():
        text = (root / relative).read_text(encoding="utf-8")
        for marker in markers:
            if marker not in text:
                raise RuntimeError(f"missing pinned source marker {relative}: {marker}")
    jars = sorted((root / "lib").glob("*.jar"), key=lambda path: path.name)
    if not jars:
        raise RuntimeError("pinned Beatoraja lib/*.jar classpath is empty")
    tracked = set(run("git", "ls-files", "lib/*.jar", cwd=root, timeout=10).splitlines())
    if {f"lib/{path.name}" for path in jars} != tracked:
        raise RuntimeError("pinned Beatoraja jar classpath must be fully tracked")
    return jars


def write_stub(root: Path, relative: str, source: str) -> Path:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(source.strip() + "\n", encoding="utf-8")
    return path


def source_files(reference: Path) -> list[Path]:
    result = []
    for path in sorted((reference / "src").rglob("*.java")):
        normalized = path.as_posix()
        if EXCLUDED_SOURCE_PART in normalized:
            continue
        if normalized.endswith(EXCLUDED_SOURCE_SUFFIXES):
            continue
        result.append(path)
    return result


def run_harness(reference: Path, jars: list[Path]) -> dict:
    with tempfile.TemporaryDirectory(prefix="asobmashow-gameplay-oracle-") as temporary:
        temporary_root = Path(temporary)
        classes = temporary_root / "classes"
        classes.mkdir()
        stubs = temporary_root / "stubs"
        stub_sources = [
            write_stub(stubs, "bms/player/beatoraja/MainLoader.java", MAIN_LOADER_STUB),
            write_stub(stubs, "bms/player/beatoraja/play/LaneRenderer.java", LANE_RENDERER_STUB),
        ]
        classpath = os.pathsep.join(str(path) for path in jars)
        run(
            "javac", "-encoding", "UTF-8", "-cp", classpath,
            "-sourcepath", "", "-d", str(classes),
            *(str(path) for path in [*source_files(reference), *stub_sources, HARNESS]),
            cwd=reference,
        )
        runtime_classpath = os.pathsep.join((str(classes), classpath))
        output = run(
            "java", "-cp", runtime_classpath,
            "bms.player.beatoraja.skin.lr2.GameplaySkinOracle",
            str(ROOT / FIXTURES[1]), str(ROOT / FIXTURES[2]), str(ROOT / FIXTURES[3]),
            cwd=ROOT,
            timeout=30,
        ).strip()
        try:
            value = json.loads(output)
        except json.JSONDecodeError as error:
            raise RuntimeError(f"oracle emitted invalid JSON: {output}") from error
        if not isinstance(value, dict) or not value:
            raise RuntimeError("oracle emitted no cases")
        return value


def differential_surface_ids() -> list[str]:
    rows = json.loads(LEDGER.read_text(encoding="utf-8"))["features"]
    tokens = (
        "bpmgraph", "bpmchart", "gauge-graph", "gaugegraph",
        "hit-error-visualizer", "hiterrorvisualizer", "judge-graph", "judgegraph",
        "notechart-1-p", "timing-1-p", "timing-distribution-graph",
        "timingdistributiongraph", "timing-visualizer", "timingvisualizer",
        "dst-slider", "src-slider", "slider-angle", "slider-changeable",
        "slider-range", "slider-type",
    )
    return sorted(
        row["id"] for row in rows
        if (any(token in row["id"] for token in tokens)
            or row["id"] == "lua.property.float-property")
        and row["id"] != "lr2.csv-command.src-slider-refnumber"
    )


def case_for(identifier: str) -> str:
    if identifier == "lua.property.float-property":
        return "selector.master-volume"
    if identifier.startswith("lr2."):
        if "slider" in identifier: return "lr2.slider"
        if "notechart" in identifier: return "lr2.note-chart"
        if "bpmchart" in identifier: return "lr2.bpm-chart"
        if "timing-1-p" in identifier: return "lr2.timing"
    if "timing-distribution" in identifier or "timingdistribution" in identifier:
        return "json.timing-distribution"
    if "hit-error" in identifier or "hiterror" in identifier:
        return "json.hit-error-visualizer"
    if "timing-visualizer" in identifier or "timingvisualizer" in identifier:
        return "json.timing-visualizer"
    if "gauge-graph" in identifier or "gaugegraph" in identifier:
        return "json.gauge-graph"
    if "judge-graph" in identifier or "judgegraph" in identifier:
        return "json.note-distribution"
    if "bpmgraph" in identifier:
        return "json.bpm-graph"
    if "slider" in identifier:
        return "lr2.slider"
    raise RuntimeError(f"differential surface has no oracle owner: {identifier}")


def build_trace(reference: Path) -> dict:
    jars = validate_reference(reference)
    output = run_harness(reference, jars)
    surface_ids = differential_surface_ids()
    owners: dict[str, list[str]] = {case: [] for case in output}
    for identifier in surface_ids:
        owner = case_for(identifier)
        if owner not in owners:
            raise RuntimeError(f"Java oracle omitted case {owner}")
        owners[owner].append(identifier)
    cases = []
    for case_id, value in output.items():
        if case_id != "selector.master-volume" and not owners[case_id]:
            raise RuntimeError(f"unowned Java oracle case: {case_id}")
        tolerance = CASE_TOLERANCE.get(case_id, 0)
        cases.append({
            "id": case_id,
            "surfaceIds": owners[case_id],
            "comparison": {
                "mode": "absolute" if tolerance else "exact",
                "tolerance": tolerance,
            },
            "source": value,
        })
    return {
        "schemaVersion": 1,
        "oracle": "pinned-beatoraja-gameplay-v1",
        "referenceCommit": PINNED_COMMIT,
        "referenceClasspath": {
            "kind": "tracked-ant-lib-wildcard",
            "jars": [
                {"path": f"lib/{path.name}", "sha256": sha256(path)} for path in jars
            ],
        },
        "sourceClosure": {
            "excluded": ["src/bms/player/beatoraja/launcher/**", *(
                "src/" + suffix.removeprefix("/")
                for suffix in EXCLUDED_SOURCE_SUFFIXES
            )],
            "testStubs": [
                "bms.player.beatoraja.MainLoader",
                "bms.player.beatoraja.play.LaneRenderer",
            ],
        },
        "fixtures": [
            {"path": relative, "sha256": sha256(ROOT / relative)} for relative in FIXTURES
        ],
        "frame": {
            "viewport": {"width": 1280, "height": 720},
            "visualTimesMillis": [0, 250, 500, 1500],
            "runtimeState": {
                "masterVolumeRate": 0.5,
                "recentHitErrorsMillis": [40, -20, 10],
            },
        },
        "differentialSurfaceIds": surface_ids,
        "cases": cases,
    }


def serialized(value: dict) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n").encode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--beatoraja-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()
    try:
        reference = arguments.beatoraja_root.expanduser().resolve()
        output = arguments.output.expanduser().resolve()
        content = serialized(build_trace(reference))
        if arguments.check:
            if not output.is_file() or output.read_bytes() != content:
                raise RuntimeError(f"committed oracle trace is stale: {output}")
            print(f"Beatoraja gameplay oracle verified: {PINNED_COMMIT}")
        else:
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(content)
            print(f"Beatoraja gameplay oracle wrote: {output}")
        return 0
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
