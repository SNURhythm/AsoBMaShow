#!/usr/bin/env python3
"""Earshot prototypes only. Times are relative to the final resolved judgement."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import numpy as np
import soundfile as sf
from scipy import signal
from earshot.forge import bank, dsp, fx, sfx

SR = 48_000
EARSHOT_COMMIT = "591fa3e028032e6dce56620a20e932e35667e8d8"
ROOT = Path(__file__).resolve().parent
OUT = ROOT / "out"


def render_bank(sound, **kwargs):
    wave, metadata = bank.render(sound, **kwargs)
    return wave, metadata


def crystal(hz, length, seed, decay, width=0.35):
    # Harmonic, open-fifth palette: a clear strike rather than an inharmonic alarm.
    wave = dsp.bell(hz, length, dsp.rng(seed),
                    partials=([1, 2, 3, 4.01, 6], [1, .24, .12, .06, .018]),
                    decay=decay, bright=.34, beat=.8, strike=.06)
    return dsp.stereo(wave, dsp.rng(seed + 1), width=width)


def finish(layers, duration, peak, seed):
    mix = dsp.stack(layers, length=duration)
    # One small shared space; direct transients remain in front of the tail.
    mix = fx.reverb(mix, dsp.rng(seed), rt60=.48, size=.28, damp=.72,
                    bass=.8, wet=.085, early_db=-12, width=.65,
                    lowcut=220, tail=.65, stereo=True)
    mix = mix[:round(duration * SR)]
    mix = signal.sosfilt(signal.butter(2, 42, btype="highpass", fs=SR, output="sos"), mix, axis=0)
    # Finish below the visual hold, with click-free boundaries and headroom.
    mix[:96] *= np.linspace(0, 1, 96)[:, None]
    tail = round(.24 * SR)
    mix[-tail:] *= (.5 + .5 * np.cos(np.linspace(0, np.pi, tail)))[:, None]
    mix = dsp.normalize(mix, peak).astype(np.float32)
    return mix


def make_sound(name):
    sources = []
    def take(sound, **kwargs):
        wave, metadata = render_bank(sound, **kwargs)
        sources.append({"id": sound, "knobs": kwargs, "measurement": metadata})
        return wave
    if name == "full-combo":
        # Lime trails and staggered lettering: a very short upward three-note gesture.
        air = take("swish/soft", dur=.34, apex=.08, seed=12)
        notes = [("E5", .08, -1, -.18), ("B5", .135, -4, 0), ("E6", .19, -8, .18)]
        layers = [(air, 0, -17)]
        for index, (pitch, time, gain, pan) in enumerate(notes):
            layers.append((take("mallet/celesta", pitch=pitch, seed=30+index), time, gain, pan))
        layers.append((crystal(659.255, 1.3, 51, .25), .08, -10))
        return finish(layers, 1.45, -9, 101), sources
    if name == "perfect":
        # One gold pulse at 80 ms: all chord voices strike together, no second hit.
        layers = [(take("swish/bright", dur=.43, apex=.08, seed=14), 0, -19),
                  (take("cut_hit/kick", pitch="E2", seed=17), .067, -19),
                  (take("mallet/glock", pitch="E5", seed=31), .07, -7),
                  (crystal(659.255, 1.72, 61, .34, .48), .08, 0),
                  (crystal(987.767, 1.65, 63, .31, .5), .08, -7),
                  (crystal(1318.51, 1.6, 65, .27, .6), .08, -11)]
        return finish(layers, 1.95, -7.5, 103), sources
    if name == "max":
        # 140 ms contraction -> one broad starburst, then a dissolving sparkle.
        layers = [(take("whoosh_fast/crisp", dur=.48, apex=.14, seed=19), 0, -14),
                  (take("cut_hit/sub", pitch="E2", seed=23), .12, -10),
                  (crystal(329.628, 2.0, 71, .30, .3), .14, -7),
                  (crystal(659.255, 2.05, 73, .40, .55), .14, 0),
                  (crystal(987.767, 2.0, 75, .35, .65), .14, -5),
                  (crystal(1318.51, 1.95, 77, .32, .7), .14, -8),
                  (take("pro_shimmer/m3", pitch="E5", dur=.95, seed=27), .14, -20)]
        return finish(layers, 2.35, -6, 107), sources
    raise ValueError(name)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", choices=["full-combo", "perfect", "max"])
    args = parser.parse_args()
    OUT.mkdir(exist_ok=True)
    manifest = {"earshot_commit": EARSHOT_COMMIT, "bank": bank.VERSION,
                "sample_rate": SR, "origin": "final resolved judgement at t=0",
                "prototype_only": True, "sounds": {}}
    for name in ([args.only] if args.only else ["full-combo", "perfect", "max"]):
        wave, sources = make_sound(name)
        path = OUT / f"{name}.wav"
        sf.write(path, wave, SR, subtype="PCM_24")
        decoded, _ = sf.read(path)
        audit = sfx.check(decoded, category="logo_sting", sound=name)
        report = {"file": path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                  "measurement": bank.measure(decoded), "audit": audit, "sources": sources}
        manifest["sounds"][name] = report
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(path),
                        "-c:a", "aac", "-b:a", "160k", str(OUT / f"{name}.m4a")], check=True)
        # Preview soundtrack contains 650 ms of pre-roll matching frame.mjs.
        sf.write(OUT / f"{name}-preview.wav", np.pad(wave, ((31_200, 48_000), (0, 0))), SR,
                 subtype="PCM_16")
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i",
                        str(OUT / f"{name}-preview.wav"), "-c:a", "aac", "-b:a", "160k",
                        str(OUT / f"{name}-preview.m4a")], check=True)
        print(name, report["measurement"], audit["summary"], flush=True)
    (OUT / "report.json").write_text(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
