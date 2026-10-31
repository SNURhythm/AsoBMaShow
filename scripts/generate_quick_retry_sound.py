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
from earshot.forge.recipes import zip as recipe

ROOT = Path(__file__).resolve().parents[1]
QUERY = "a soft descending sci-fi pew whoosh with a low rounded tail"
ALTERNATIVES = ["a sharp laser gun blast", "a rising slide whistle",
                "an alarm beep", "a harsh buzzer"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--feedback", action="store_true")
    args = parser.parse_args()
    output = ROOT / "assets/quick-retry.wav"
    # Feedback on the bank version favored a sharp laser/error buzz. Reduce
    # saw/harmonic energy and saturation; give the downward gesture more time.
    parameters = {**bank.get("pro_zip/down_warm")["params"],
                  "length": 0.23, "f0": 1800.0, "f1": 180.0, "saw_db": -36.0,
                  "h2_db": -26.0, "h3_db": -36.0, "h5_db": -48.0, "fm": 0.08,
                  "sat": 0.0, "att_ms": 22.0, "rel_ms": 50.0, "tilt_db": -5.0,
                  "air_db": -25.0, "noise_db": -30.0, "detune": 3.0,
                  "verb": 0.08, "width": 0.1}
    samples = recipe.render(parameters, np.random.default_rng(87989972))
    samples = samples[:round(0.38 * 48000)]
    samples[-960:] *= np.linspace(1, 0, 960)[:, None]
    samples *= 10 ** (-13 / 20) / np.max(np.abs(samples))
    metadata = {"base": "pro_zip/down_warm", "seed": 87989972,
                "parameters": parameters, "duration_s": len(samples) / 48000,
                "sample_rate": 48000, "peak_dbfs": -13}
    sf.write(output, samples, 48000, subtype="PCM_16")
    # Check the quantized asset that ships, not just the in-memory synthesis.
    decoded, rate = sf.read(output)
    metadata["checks"] = sfx.check(decoded, sr=rate, category="pro_zip")
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
