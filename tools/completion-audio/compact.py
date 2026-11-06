#!/usr/bin/env python3
"""Short rewards with an overall rising melodic contour with rounded kicks and restrained closed hats."""
import hashlib
import json
from pathlib import Path

import numpy as np
import soundfile as sf
from earshot import api
from earshot.forge import bank, dsp, sfx
from earshot.forge.critic import Critic
from ascending import render
from melodies import encode

OUT = Path(__file__).resolve().parent / 'out'
STEP = .075  # Sixteenths at 200 BPM.
SCORES = {
    'full-combo': {
        'lead': ['C5', 'G5', 'E5', 'C6'], 'ticks': [0, 1, 2, 4],
        'chords': [(0, 'C', ['C4', 'E4', 'G4']),
                   (2, 'G7', ['B3', 'D4', 'F4', 'G4']),
                   (4, 'C', ['C4', 'E4', 'G4'])]},
    'perfect': {
        'lead': ['C5', 'G5', 'E5', 'D6', 'E6'], 'ticks': [0, 1, 2, 3, 5],
        'chords': [(0, 'Cmaj7', ['C4', 'E4', 'G4', 'B4']),
                   (2, 'Fmaj9', ['F3', 'A3', 'C4', 'E4', 'G4']),
                   (3, 'G9', ['G3', 'B3', 'D4', 'F4', 'A4']),
                   (5, 'Cmaj9', ['C4', 'E4', 'G4', 'B4', 'D5'])]},
    'max': {
        'lead': ['C5', 'G5', 'E5', 'D6', 'B5', 'G6'], 'ticks': [0, 1, 2, 3, 4, 6],
        'chords': [(0, 'Cmaj7', ['C4', 'E4', 'G4', 'B4']),
                   (2, 'Am7', ['A3', 'C4', 'E4', 'G4']),
                   (3, 'Fmaj9', ['F3', 'A3', 'C4', 'E4', 'G4']),
                   (4, 'G9', ['G3', 'B3', 'D4', 'F4', 'A4']),
                   (6, 'C6/9', ['C4', 'E4', 'G4', 'A4', 'D5'])]},
}


def kick(seed, final=False):
    t = np.arange(round(.22 * dsp.SR)) / dsp.SR
    # A short pitch fall creates a drum transient, not a sustained bass note.
    phase = 2 * np.pi * (58 * t + 92 * .014 * (1 - np.exp(-t / .014)))
    envelope = (1 - np.exp(-t / .0015)) * np.exp(-t / (.048 if final else .040))
    body = np.sin(phase) + .15 * np.sin(2 * phase) * np.exp(-t / .023)
    contact = dsp.butter(dsp.rng(seed).standard_normal(len(t)), 'low', 1600, 2)
    contact *= (1 - np.exp(-t / .0008)) * np.exp(-t / .004)
    x = body * envelope + .07 * contact
    x = dsp.normalize(dsp.fade(x, .001, .025), -1)
    return np.column_stack((x, x))


def hat(seed, pan):
    t = np.arange(round(.060 * dsp.SR)) / dsp.SR
    x = dsp.rng(seed).standard_normal(len(t))
    x = dsp.butter(x, 'high', 5500, 2)
    x = dsp.butter(x, 'low', 10500, 2)
    x *= (1 - np.exp(-t / .001)) * np.exp(-t / .010)
    x = dsp.normalize(dsp.fade(x, .001, .015), -1)
    return dsp.pan_to_stereo(x, pan)


def main():
    report = {'revision': 'contoured-percussion-local', 'tempo_bpm': 200,
              'subdivision': 'sixteenths', 'prototype_only': True, 'sounds': {}}
    target = ('A very short upbeat arcade victory jingle: four to six bright '
              'keyboard notes that leap up, turn briefly downward, and finish higher over quick warm chord changes, with '
              'two rounded electronic kick drum accents and quiet closed hi-hats. '
              'A tight high chord ends the phrase, with a short clean decay.')
    critic = Critic(target, alts=['A single notification beep.', 'A harsh explosion.',
                                  'A long music box melody.', 'An isolated bass drum hit.'],
                    mode='designed', category='logo_sting')
    for index, (name, score) in enumerate(SCORES.items()):
        start = .14 if name == 'max' else .08
        finish = start + score['ticks'][-1] * STEP
        percussion = [(kick(3100 + index), start, -8),
                      (kick(3110 + index, final=True), finish, -6)]
        hats = list(range(1, score['ticks'][-1], 2))
        for j, tick in enumerate(hats):
            percussion.append((hat(3200 + index * 10 + j, -.2 if j % 2 else .2),
                               start + tick * STEP, -23))
        x = render(name, score, index, step=STEP, tail=.56,
                   lead_hold=.17, chord_hold=.20, percussion=percussion)
        encode(x, name)
        encode(np.pad(x, ((31200, 192000 - 31200 - len(x)), (0, 0))), name + '-preview')
        old, sr = sf.read(OUT / 'v5-short-drums' / (name + '.wav'))
        assert sr == 48000
        old *= 10 ** ((bank.measure(x)['lufs_m_max'] - bank.measure(old)['lufs_m_max']) / 20)
        encode(np.pad(old, ((31200, 192000 - 31200 - len(old)), (0, 0))), 'v1-' + name + '-preview')
        description = api.describe(str(OUT / (name + '.wav')))
        report['sounds'][name] = {
            'score': score, 'phrase_span_s': finish - start,
            'percussion': {'kick_times_s': [start, finish],
                           'hat_times_s': [start + tick * STEP for tick in hats]},
            'measure': bank.measure(x), 'audit': sfx.check(x, category='logo_sting'),
            'critic': critic.score([x])[0], 'description': description,
            'sha256': hashlib.sha256((OUT / (name + '.wav')).read_bytes()).hexdigest()}
        (OUT / 'compact-feedback.json').write_text(json.dumps(report, indent=2))
        print(name, description['summary'], flush=True)


if __name__ == '__main__':
    main()
