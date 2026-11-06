#!/usr/bin/env python3
"""A sampled victory accent, auditioned with and without a recorded crowd lift."""
import hashlib
import json
from pathlib import Path
from math import gcd

import numpy as np
import librosa
import soundfile as sf
from scipy import signal
from earshot import api
from earshot.forge import bank, dsp, sfx
from earshot.forge.critic import Critic
from earshot.forge.recipes.impact import hall_ir
from melodies import encode

OUT = Path(__file__).resolve().parent / 'out'
SOURCES = OUT / 'source-review'


def load(path):
    x, sr = sf.read(path, always_2d=True)
    if x.shape[1] == 1:
        x = np.repeat(x, 2, axis=1)
    if sr != dsp.SR:
        g = gcd(sr, dsp.SR)
        x = signal.resample_poly(x, dsp.SR // g, sr // g, axis=0)
    return x


def main():
    source_hit = SOURCES / 'fanfare.wav'
    source_crowd = SOURCES / 'qubodup-cheer.mp3'
    fanfare = load(source_hit)
    hit = fanfare[round(3.60 * dsp.SR):round(4.70 * dsp.SR)]
    hit = dsp.butter(hit, 'high', 90, 2)
    hit = dsp.butter(hit, 'low', 7500, 3)
    hit = dsp.normalize(dsp.fade(hit, .006, .20), -1)
    # Add the preceding melodic pickup instead of the old reverse swell.
    # Compress its rhythm to fit before the same 140 ms visual accent,
    # preserving the source pitches and the accepted resolution.
    pickup = fanfare[round(3.42 * dsp.SR):round(3.60 * dsp.SR)]
    pickup = librosa.effects.time_stretch(pickup.T, rate=.18 / .14,
                                         n_fft=1024, hop_length=128).T
    pickup = dsp.butter(pickup, 'high', 110, 2)
    pickup = dsp.butter(pickup, 'low', 6500, 3)
    pickup = dsp.normalize(dsp.fade(pickup, .008, .010), -1)
    body = hit
    wet = np.zeros_like(body)
    for ch in range(2):
        ir = hall_ir(dsp.rng(6100 + ch), .52, .85, size=.65,
                     predelay=.012, seconds=.65)
        wet[:, ch] = signal.fftconvolve(body[:, ch], ir)[:len(body)]
    wet = dsp.butter(wet, 'low', 4800, 2)
    wet = dsp.normalize(wet, -1)
    core = dsp.stack([(pickup, 0, -7), (body, .14, 0),
                      (wet, .14, -18)], length=2.90)
    recording = load(source_crowd)
    # The stronger cheer around 1.6 s becomes the immediate crowd reaction.
    roar = recording[round(1.25 * dsp.SR):round(2.05 * dsp.SR)]
    roar = dsp.butter(roar, 'high', 180, 2)
    roar = dsp.butter(roar, 'low', 6500, 2)
    roar = dsp.normalize(dsp.fade(roar, .06, .30), -1)
    # A different continuous section carries the applause after the music.
    applause = recording[round(2.05 * dsp.SR):round(4.65 * dsp.SR)]
    applause = dsp.butter(applause, 'high', 330, 2)
    applause = dsp.butter(applause, 'low', 6200, 2)
    applause = dsp.normalize(dsp.fade(applause, .15, .75), -1)
    crowd_layers = [(roar, .18, -8), (applause, .26, -11)]
    with_crowd = core + dsp.stack(crowd_layers, length=2.90)
    previous = load(OUT / 'v7-celebration/victory-crowd.wav')
    sounds = {'victory-hit': dsp.fade(core, .003, .18),
              'victory-crowd': dsp.fade(with_crowd, .003, .18),
              'victory-previous': previous}
    measured = {n: bank.measure(x) for n, x in sounds.items()}
    target_level = min(-17, *(m['lufs_m_max'] - m['peak_db'] - 4
                              for m in measured.values()))
    target = ('A celebratory arcade victory accent: a short musical pickup resolves '
              'into a bright full fanfare hit, a crowd roars in approval, and applause '
              'continues after the music fades. A joyful performance finish, without record scratching.')
    critic = Critic(target, alts=['An error alert.', 'A plain drum beat.',
                                  'A harsh scratch sound.', 'A sad game over cue.'],
                    mode='designed', category='logo_sting')
    report = {'revision': 'celebration-pickup-roar-applause-local', 'prototype_only': True,
              'matched_lufs_m_max': target_level,
              'edits': {'pickup_source_s': [3.42, 3.60], 'pickup_output_s': [0, .14],
                        'resolution_source_s': [3.60, 4.70], 'roar_source_s': [1.25, 2.05],
                        'roar_start_s': .18, 'applause_source_s': [2.05, 4.65],
                        'applause_start_s': .26, 'master_duration_s': 2.90},
              'sources': [], 'sounds': {}}
    for p, url, author in [(source_hit, 'https://opengameart.org/content/hyper-ultra-fanfare', 'Zane Little Music'),
                           (source_crowd, 'https://freesound.org/people/qubodup/sounds/182572/', 'qubodup')]:
        report['sources'].append({'file': str(p.relative_to(OUT)), 'url': url,
                                  'author': author, 'license': 'CC0-1.0',
                                  'sha256': hashlib.sha256(p.read_bytes()).hexdigest()})
    for name, x in sounds.items():
        x *= 10 ** ((target_level - measured[name]['lufs_m_max']) / 20)
        encode(x, name)
        encode(np.pad(x, ((31200, 192000 - 31200 - len(x)), (0, 0))), name + '-preview')
        if name == 'victory-previous':
            report['sounds'][name] = {'measure': bank.measure(x), 'comparison_only': True}
            (OUT / 'celebration-feedback.json').write_text(json.dumps(report, indent=2))
            continue
        desc = api.describe(str(OUT / (name + '.wav')))
        report['sounds'][name] = {'measure': bank.measure(x),
                                  'audit': sfx.check(x, category='logo_sting'),
                                  'critic': critic.score([x])[0], 'description': desc,
                                  'sha256': hashlib.sha256((OUT / (name + '.wav')).read_bytes()).hexdigest()}
        (OUT / 'celebration-feedback.json').write_text(json.dumps(report, indent=2))
        print(name, desc['summary'], flush=True)


if __name__ == '__main__':
    main()
