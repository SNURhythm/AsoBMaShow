#!/usr/bin/env python3
"""Synthetic record scrubbing and crossfader cuts; no melody or sampled records."""
import hashlib
import json
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy import signal
from earshot import api
from earshot.forge import bank, dsp, sfx
from earshot.forge.critic import Critic
from compact import kick, hat
from melodies import encode

OUT = Path(__file__).resolve().parent / 'out'
PATTERNS = {
    'full-combo': {'strokes': [.140, .120], 'cuts': [[], []]},
    'perfect': {'strokes': [.105, .080, .115, .100],
                'cuts': [[], [.46], [], [.50]]},
    'max': {'strokes': [.095, .075, .125, .085, .080, .120],
            'cuts': [[], [.48], [.34, .68], [], [.48], [.33, .66]]},
}


def record_source(seed):
    """A fixed gritty, vowel-like synth texture for the virtual record groove."""
    t = np.arange(dsp.SR) / dsp.SR
    r = dsp.rng(seed)
    # Harmonics through broad formants make a nonverbal scratchable texture.
    f0 = 118
    x = np.zeros(len(t))
    for h in range(1, 65):
        f = h * f0
        weight = (.06 / h + 1.0 * np.exp(-.5 * ((f - 700) / 190) ** 2)
                  + .65 * np.exp(-.5 * ((f - 1200) / 260) ** 2)
                  + .25 * np.exp(-.5 * ((f - 2600) / 470) ** 2))
        x += weight * np.sin(2 * np.pi * f * t + r.uniform(-.12, .12))
    x /= np.sqrt(np.mean(x * x))
    noise = dsp.butter(r.standard_normal(len(t)), 'high', 380, 2)
    noise = dsp.butter(noise, 'low', 4200, 2)
    noise /= np.sqrt(np.mean(noise * noise))
    x = .72 * x + .28 * noise
    x *= .84 + .12 * np.sin(2 * np.pi * 19 * t) + .04 * np.sin(2 * np.pi * 43 * t)
    return dsp.butter(np.tanh(x * .65), 'low', 6000, 4)


def scratch(pattern, seed):
    source = record_source(seed)
    cursor = .25 * dsp.SR
    pieces = []
    for i, (duration, cuts) in enumerate(zip(pattern['strokes'], pattern['cuts'])):
        n = round(duration * dsp.SR)
        u = np.linspace(0, 1, n)
        # Speed rises and falls with the hand; alternating signs replay the
        # same groove forward/back, rather than a generic filtered-noise sweep.
        speed = (1 if i % 2 == 0 else -1) * (1.60 + .18 * (i % 3)) * np.sin(np.pi * u) ** .65
        positions = cursor + np.cumsum(speed)
        cursor = positions[-1]
        assert positions.min() >= 0 and positions.max() < len(source) - 1
        y = np.interp(positions, np.arange(len(source)), source)
        gate = np.sin(np.pi * np.clip(u / .065, 0, 1) / 2) ** 2
        gate *= np.sin(np.pi * np.clip((1 - u) / .085, 0, 1) / 2) ** 2
        for cut in cuts:
            # Rounded 2–3 ms fader edges avoid digital discontinuities.
            distance = abs(u - cut) * duration
            gate *= np.sin(np.pi / 2 * np.clip((distance - .004) / .0025, 0, 1)) ** 2
        pieces.append(y * gate)
    y = np.concatenate(pieces)
    y = dsp.butter(y, 'high', 180, 2)
    y = dsp.butter(y, 'low', 5200, 4)
    y = dsp.normalize(dsp.fade(y, .002, .004), -1)
    return np.column_stack((y, y))


def snare(seed):
    t = np.arange(round(.14 * dsp.SR)) / dsp.SR
    r = dsp.rng(seed)
    noise = dsp.butter(r.standard_normal(len(t)), 'high', 950, 2)
    noise = dsp.butter(noise, 'low', 6500, 2)
    x = noise * np.exp(-t / .023)
    x += .32 * np.sin(2 * np.pi * 185 * t) * np.exp(-t / .022)
    x *= 1 - np.exp(-t / .0015)
    x = dsp.normalize(dsp.fade(x, .001, .035), -1)
    return dsp.pan_to_stereo(x, 0)


def main():
    target = ('A short DJ turntable scratch flourish: a gritty nonverbal record '
              'sample is scrubbed forward and backward in rhythmic wah-chik gestures '
              'with tight crossfader cuts, ending with a dry kick and snare accent. '
              'A compact upbeat arcade reward, no tune, no chords, no spoken words.')
    alts = ['An electronic alarm or siren.', 'A melodic notification chime.',
            'A zipper being opened.', 'A synthesizer laser sound.', 'A drum beat without scratching.']
    critic = Critic(target, alts=alts, mode='designed', category='logo_sting')
    report = {'revision': 'dj-scratch-local', 'prototype_only': True,
              'source': 'procedural nonverbal formant texture; no recorded samples',
              'sounds': {}}
    for index, (name, pattern) in enumerate(PATTERNS.items()):
        start = .14 if name == 'max' else .08
        scratch_audio = scratch(pattern, 4100)
        landing = start + len(scratch_audio) / dsp.SR + .025
        layers = [(scratch_audio, start, 0), (kick(4200 + index), start, -15),
                  (kick(4210 + index, final=True), landing, -9),
                  (snare(4300 + index), landing, -19)]
        if index:
            layers.append((hat(4400 + index, .12), landing - .075, -23))
        x = dsp.stack(layers, length=landing + .31)
        x = dsp.fade(x, .002, .06)
        m = bank.measure(x)
        x *= 10 ** (min([-18, -17, -16][index] - m['lufs_m_max'], -5 - m['peak_db']) / 20)
        encode(x, name)
        encode(np.pad(x, ((31200, 192000 - 31200 - len(x)), (0, 0))), name + '-preview')
        old, sr = sf.read(OUT / 'v6-contour' / (name + '.wav'))
        assert sr == dsp.SR
        old *= 10 ** ((bank.measure(x)['lufs_m_max'] - bank.measure(old)['lufs_m_max']) / 20)
        encode(np.pad(old, ((31200, 192000 - 31200 - len(old)), (0, 0))), 'v1-' + name + '-preview')
        description = api.describe(str(OUT / (name + '.wav')))
        report['sounds'][name] = {
            'pattern': pattern, 'landing_s': landing, 'measure': bank.measure(x),
            'audit': sfx.check(x, category='logo_sting'), 'critic': critic.score([x])[0],
            'scratch_match': api.match(str(OUT / (name + '.wav')),
                                       'A DJ scratching a vinyl record on a turntable.', alts=alts),
            'description': description,
            'sha256': hashlib.sha256((OUT / (name + '.wav')).read_bytes()).hexdigest()}
        (OUT / 'dj-feedback.json').write_text(json.dumps(report, indent=2))
        print(name, description['summary'], flush=True)


if __name__ == '__main__':
    main()
