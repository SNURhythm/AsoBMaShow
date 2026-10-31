#!/usr/bin/env python3
"""Render and audit the automatic-retry cue with Earshot 0.4.0.

Install Earshot from commit 591fa3e028032e6dce56620a20e932e35667e8d8:
https://github.com/Leonxlnx/earshot
Pass --feedback to also run its local description and text-match models.
Earshot is an authoring dependency only; the app ships the rendered WAV.
"""
import argparse
import json
from pathlib import Path

import numpy as np
import soundfile as sf
from earshot.forge import bank, sfx

ROOT = Path(__file__).resolve().parents[1]
QUERY = "a soft digital interface reset click with a short airy tail"
ALTERNATIVES = ["a sharp harsh click", "a cartoon bubble pop",
                "a laser gun blast", "an error buzz"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--feedback", action="store_true")
    args = parser.parse_args()
    output = ROOT / "assets/quick-retry.wav"
    click, click_meta = bank.render("pro_click/soft", peak_db=-16)
    air, air_meta = bank.render("swish/flick", dur=0.20, peak_db=-29)
    if click.ndim == 1:
        click = np.column_stack((click, click))
    if air.ndim == 1:
        air = np.column_stack((air, air))
    # The quiet air layer starts with the click: one gesture, with no lead-in.
    samples = np.zeros((max(len(click), len(air)), 2))
    samples[:len(click)] += click
    samples[:len(air)] += air
    samples *= 10 ** (-16 / 20) / np.max(np.abs(samples))
    metadata = {"components": [
                    {key: item[key] for key in ("id", "seed", "dur_s", "peak_db")}
                    for item in (click_meta, air_meta)],
                "duration_s": len(samples) / 48000,
                "sample_rate": 48000, "peak_dbfs": -16}
    sf.write(output, samples, 48000, subtype="PCM_16")
    # Check the quantized asset that ships, not just the in-memory synthesis.
    decoded, rate = sf.read(output)
    metadata["checks"] = sfx.check(decoded, sr=rate, category="pro_click")
    if not metadata["checks"]["ok"]:
        raise SystemExit(json.dumps(metadata["checks"], indent=2))
    report = {"earshot_commit": "591fa3e028032e6dce56620a20e932e35667e8d8",
              "render": metadata}
    if args.feedback:
        from earshot import api
        description = api.describe(str(output))
        match = api.match(str(output), QUERY, alts=ALTERNATIVES)
        report["feedback"] = {
            "description": description["summary"],
            "measurements": description["measurements"],
            "query": QUERY, "verdict": match["verdict"], "z": match["z"],
            "alternatives": match["alts"],
        }
    report_path = ROOT / "docs/audio/quick-retry-earshot.json"
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(metadata["checks"]["summary"])
    print(output)


if __name__ == "__main__":
    main()
