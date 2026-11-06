#!/usr/bin/env python3
"""Two smoother club palettes, auditioned on MAX before making a reward set."""
import hashlib
import json
from pathlib import Path

import numpy as np
from scipy import signal
import soundfile as sf
from earshot import api
from earshot.forge import bank, dsp, sfx
from earshot.forge.critic import Critic
from compact import kick, hat
from dj import snare
from melodies import encode

OUT = Path(__file__).resolve().parent / 'out'


def muted_tom(seed):
    t = np.arange(round(.14 * dsp.SR)) / dsp.SR
    phase = 2 * np.pi * (135 * t + 40 * .012 * (1 - np.exp(-t / .012)))
    y = (np.sin(phase) + .18 * np.sin(phase * 1.58)) * np.exp(-t / .026)
    y *= 1 - np.exp(-t / .003)
    y = dsp.normalize(dsp.fade(y, .002, .03), -1)
    return dsp.pan_to_stereo(y, -.08 if seed % 2 else .08)


def drum_lift():
    # A compact syncopated fill. Filter movement changes timbre, not pitch.
    layers = [(kick(5100), .14, -1), (hat(5101, .12), .24, -23),
              (muted_tom(5102), .315, -10), (muted_tom(5103), .390, -7),
              (snare(5104), .465, -20), (kick(5105, final=True), .54, 0),
              (snare(5106), .54, -19)]
    x = dsp.stack(layers, length=.94)
    dark = dsp.butter(x, 'low', 700, 2)
    open_sound = dsp.butter(x, 'low', 2900, 3)
    t = np.arange(len(x)) / dsp.SR
    u = np.clip((t - .14) / .38, 0, 1)
    opening = u * u * (3 - 2 * u)
    return dark * (1 - opening[:, None]) + open_sound * opening[:, None]


def echo_out():
    # Rounded clap-like accent, softly filtered, with two dark offbeat echoes.
    accent = dsp.butter(snare(5202), 'low', 2300, 3)
    t = np.arange(len(accent)) / dsp.SR
    accent *= (np.sin(np.pi / 2 * np.clip(t / .005, 0, 1)) ** 2)[:, None]
    echo = dsp.butter(accent, 'low', 1500, 2)
    layers = [(kick(5200), .14, 0), (accent, .14, -11),
              (echo, .29, -15, -.16), (echo, .44, -21, .16)]
    return dsp.stack(layers, length=.94)


def main():
    specs = {
        'drum-lift': (drum_lift, 'A short smooth electronic dance drum fill. Rounded kicks and muted tom taps '
                     'form a tight syncopated rhythm, with a gently opening low-pass filter and a clean beat landing. '
                     'Soft edges, restrained treble, no melody and no record scratching.'),
        'echo-out': (echo_out, 'A short smooth electronic dance drum accent: one rounded kick with a soft muted clap, '
                    'followed by two quieter darker echoes of the clap. A gentle rhythmic finish with restrained treble, '
                    'no melody, no keyboard chord and no record scratching.'),
    }
    report = {'revision': 'smooth-dj-palettes-local', 'prototype_only': True,
              'scope': 'Two MAX palette studies; no approved reward set yet', 'sounds': {}}
    alts = ['A harsh record scratching sound.', 'A scratchy zipper sound.',
            'A bright notification bell melody.', 'An electronic alarm.']
    candidates = {name: dsp.fade(renderer(), .002, .10)
                  for name, (renderer, _) in specs.items()}
    measurements = {name: bank.measure(x) for name, x in candidates.items()}
    target_level = min(-18, *(m['lufs_m_max'] + (-5 - m['peak_db'])
                              for m in measurements.values()))
    report['matched_lufs_m_max'] = target_level
    for name, (_, brief) in specs.items():
        x = candidates[name]
        measure = bank.measure(x)
        gain = target_level - measure['lufs_m_max']
        x *= 10 ** (gain / 20)
        encode(x, name)
        encode(np.pad(x, ((31200, 192000 - 31200 - len(x)), (0, 0))), name + '-preview')
        desc = api.describe(str(OUT / (name + '.wav')))
        critic = Critic(brief, alts=alts, mode='designed', category='logo_sting')
        report['sounds'][name] = {'brief': brief, 'measure': bank.measure(x),
                                  'audit': sfx.check(x, category='logo_sting'),
                                  'critic': critic.score([x])[0], 'description': desc,
                                  'sha256': hashlib.sha256((OUT / (name + '.wav')).read_bytes()).hexdigest()}
        (OUT / 'smooth-dj-feedback.json').write_text(json.dumps(report, indent=2))
        print(name, desc['summary'], flush=True)


if __name__ == '__main__':
    main()
