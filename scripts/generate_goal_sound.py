#!/usr/bin/env python3
"""Render a short trumpet-style goal fanfare with Earshot DSP and its sound bank.

Uses Earshot 591fa3e028032e6dce56620a20e932e35667e8d8, numpy and soundfile.
"""
import argparse
from pathlib import Path
import numpy as np
import soundfile as sf
from earshot.forge import bank, dsp


def render(brightness=1.0):
    sr = 48000
    mix = np.zeros(int(1.25 * sr))
    # Tongued major-triad fanfare, with a longer final note.
    for at, freq, duration in [(0, 392, .14), (.17, 523.25, .14),
                               (.34, 659.25, .16), (.54, 783.99, .43)]:
        t = np.arange(int(duration * sr)) / sr
        envelope = dsp.env_curve([(0, 0), (.012, 1), (.035, .82),
                                  (duration-.055, .78), (duration, 0)], duration)
        phase = 2*np.pi*np.cumsum(freq*(1 + .002*np.sin(2*np.pi*5.2*t)
                                      * np.minimum(t/.15, 1)))/sr
        tone = np.zeros(len(t))
        for harmonic in range(1, 18):
            # Brass has strong upper harmonics that brighten with the attack.
            amplitude = np.exp(-harmonic/(4.5*brightness))/harmonic**.45
            tone += amplitude*np.sin(harmonic*phase) * envelope**(1+harmonic*.07)
        start = int(at*sr)
        mix[start:start+len(tone)] += tone
    mix = dsp.room(mix, dsp.rng(7), rt60=.19, wet=.055)
    entry = bank.get('swish/stick')
    entry['params'].update(rise=.025, hold=.005, fall=.07, rt60=.1,
                           room_wet=.03, tone_db=-60, eq_hi_db=-9)
    air, _ = bank.render(entry, seed=7, peak_db=-31)
    if air.ndim > 1:
        air = air.mean(axis=1)
    mix = bank.normalize(mix, -12)
    if mix.ndim > 1:
        mix = mix.mean(axis=1)
    mix[:len(air)] += air
    mix[-240:] *= np.linspace(1, 0, 240)
    return bank.normalize(mix, -12).astype(np.float32)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[1] /
                        'assets/audio/kombo/goal-achieved.wav')
    parser.add_argument('--brightness', type=float, default=1.0)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    sf.write(args.output, render(args.brightness), 48000, subtype='PCM_16')
    assert args.output.stat().st_size <= 256*1024
