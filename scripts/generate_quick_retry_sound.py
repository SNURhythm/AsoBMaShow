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
QUERY = "a delicate glassy two-note interface chime"
ALTERNATIVES = ["an alarm beep", "a harsh buzzer",
                "a cartoon boing", "a metallic impact"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--feedback", action="store_true")
    args = parser.parse_args()
    output = ROOT / "assets/quick-retry.wav"
    first, first_meta = bank.render("mallet/celesta", pitch="D6", dur=0.26, peak_db=-16)
    second, second_meta = bank.render("mallet/celesta", pitch="A6", dur=0.26, peak_db=-17)
    if first.ndim == 1:
        first = np.column_stack((first, first))
    if second.ndim == 1:
        second = np.column_stack((second, second))
    second_offset = round(0.09 * 48000)
    samples = np.zeros((max(len(first), second_offset + len(second)), 2))
    samples[:len(first)] += first
    samples[second_offset:second_offset + len(second)] += second
    samples *= 10 ** (-14 / 20) / np.max(np.abs(samples))
    metadata = {"components": [
                    {**{key: item[key] for key in ("id", "seed", "dur_s", "peak_db")},
                     "pitch": pitch, "offset_s": offset}
                    for item, pitch, offset in ((first_meta, "D6", 0), (second_meta, "A6", 0.09))],
                "duration_s": len(samples) / 48000,
                "sample_rate": 48000, "peak_dbfs": -14}
    sf.write(output, samples, 48000, subtype="PCM_16")
    # Check the quantized asset that ships, not just the in-memory synthesis.
    decoded, rate = sf.read(output)
    metadata["checks"] = sfx.check(decoded, sr=rate, category="mallet")
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
