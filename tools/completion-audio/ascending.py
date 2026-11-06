#!/usr/bin/env python3
"""Ascending, rapidly harmonized completion auditions. Local authoring only."""
import hashlib
import json
from pathlib import Path

import numpy as np
from scipy import signal
import soundfile as sf
from earshot import api
from earshot.forge import bank, dsp, sfx
from earshot.forge.critic import Critic
from earshot.forge.recipes import mallet
from earshot.forge.recipes.impact import hall_ir
from melodies import hz, encode

OUT = Path(__file__).resolve().parent / 'out'
# Sixteenths at 180 BPM. Last note is held; the rising line never turns down.
STEP = 60 / 180 / 4
SCORES = {
    'full-combo': {
        'lead': ['C5', 'E5', 'G5', 'A5', 'B5', 'C6'],
        'ticks': [0, 1, 2, 3, 4, 6],
        'chords': [(0, 'C', ['C4', 'E4', 'G4']),
                   (3, 'Fmaj7', ['C4', 'E4', 'F4', 'A4']),
                   (4, 'G7', ['B3', 'D4', 'F4', 'G4']),
                   (6, 'C', ['C4', 'E4', 'G4'])],
    },
    'perfect': {
        'lead': ['G4', 'C5', 'E5', 'G5', 'A5', 'C6', 'D6', 'E6'],
        'ticks': [0, 1, 2, 3, 4, 5, 6, 8],
        'chords': [(0, 'Cmaj7', ['C4', 'E4', 'G4', 'B4']),
                   (4, 'Fmaj7', ['C4', 'E4', 'F4', 'A4']),
                   (6, 'G9', ['B3', 'D4', 'F4', 'A4']),
                   (8, 'Cmaj9', ['C4', 'E4', 'G4', 'B4', 'D5'])],
    },
    'max': {
        'lead': ['G4', 'B4', 'C5', 'E5', 'G5', 'A5', 'B5', 'D6', 'E6', 'G6'],
        'ticks': [0, 1, 2, 3, 4, 5, 6, 7, 8, 10],
        'chords': [(0, 'Em7', ['B3', 'D4', 'E4', 'G4']),
                   (2, 'Am7', ['A3', 'C4', 'E4', 'G4']),
                   (4, 'Fmaj9', ['F3', 'C4', 'E4', 'G4', 'A4']),
                   (6, 'G9', ['G3', 'B3', 'D4', 'F4', 'A4']),
                   (8, 'Cmaj9', ['G3', 'C4', 'E4', 'B4', 'D5']),
                   (10, 'C6/9', ['C4', 'E4', 'G4', 'A4', 'D5'])],
    },
}


def note_voice(note, hold, seed, pan=0, harmony=False):
    p = {**mallet.DEFAULTS, 'style': 'pluck' if harmony else 'note',
         'bar': 'celesta', 'f0': hz(note), 'hard': .25, 'strike': -52,
         'onset': .008 if harmony else .0045, 'gate': hold,
         'gate_tau': .025 if harmony else .04, 'decay': .46,
         'slope': .85, 'hp': 130, 'lp': 5200, 'width': .30,
         'pan': pan, 'room': 0, 'oct': -60, 'beat': .55, 'jit': .06,
         'tilt': 1.5, 'pos': .24, 'contact': .0014, 'body': -30,
         'body_f': 260}
    return mallet.render(p, dsp.rng(seed))


def render(name, score, index, *, step=STEP, tail=.90, lead_hold=.34,
           chord_hold=.40, percussion=()):
    start = .14 if name == 'max' else .08
    finish = start + score['ticks'][-1] * step
    length = finish + tail
    layers = []
    count = len(score['lead'])
    for j, (note, tick) in enumerate(zip(score['lead'], score['ticks'])):
        last = j == count - 1
        hold = lead_hold if last else min(.105, step * 1.26)
        velocity = .72 + .24 * j / (count - 1)
        # Quiet overlap connects the line while its attacks remain distinct.
        layers.append((note_voice(note, hold, 2100 + index * 30 + j,
                                  (j / (count - 1) - .5) * .24) * velocity,
                       start + tick * step, 0))
    for j, (tick, label, notes) in enumerate(score['chords']):
        last = j == len(score['chords']) - 1
        hold = chord_hold if last else (score['chords'][j + 1][0] - tick) * step * .78
        for k, note in enumerate(notes):
            # Close voicings move under the lead; each change is a new attack.
            pan = (k / (len(notes) - 1) - .5) * .60
            voice = note_voice(note, hold, 2500 + index * 70 + j * 7 + k,
                               pan, harmony=True)
            layers.append((voice, start + tick * step + k * .002,
                           -9.5 - 10 * np.log10(len(notes))))
    x = dsp.stack(layers, length=length)
    # Short reflections knit the rapid notes together without a smeared wash.
    send = dsp.butter(x, 'high', 450, 2)
    for ch in range(2):
        ir = hall_ir(dsp.rng(2900 + index * 2 + ch), .40, .8,
                     size=.25, predelay=.012, seconds=.55)
        x[:, ch] += .065 * signal.fftconvolve(send[:, ch], ir)[:len(x)]
    # The first critic pass flagged upper partials at 4.15 and 5.23 kHz.
    # Round that edge while retaining the lead fundamentals (up to 1.57 kHz).
    x = dsp.butter(x, 'low', 3600, 4)
    if percussion:
        # Dry percussion keeps its punch and does not enter the pitched reverb.
        x += dsp.stack(percussion, length=length)
    x = dsp.fade(x, .002, .16)
    measure = bank.measure(x)
    gain = min([-18, -17, -16][index] - measure['lufs_m_max'],
               -5 - measure['peak_db'])
    return x * 10 ** (gain / 20)


def main():
    OUT.mkdir(exist_ok=True)
    report = {'revision': 'ascending-harmonies-local', 'tempo_bpm': 180,
              'subdivision': 'sixteenths', 'prototype_only': True, 'sounds': {}}
    target = ('A short upbeat game victory jingle: a bright, rounded keyboard '
              'melody climbs rapidly over changing warm chords, with crisp '
              'closely spaced notes and a sustained major-chord finish.')
    alts = ['A single electronic notification beep.', 'A harsh metallic crash.',
            'A slowly descending music box melody.', 'A telephone ringing.']
    critic = Critic(target, alts=alts, mode='designed', category='logo_sting')
    for index, (name, score) in enumerate(SCORES.items()):
        x = render(name, score, index)
        encode(x, name)
        encode(np.pad(x, ((31200, 192000 - 31200 - len(x)), (0, 0))), name + '-preview')
        old, sr = sf.read(OUT / 'v3-melody' / (name + '.wav'))
        assert sr == 48000
        old *= 10 ** ((bank.measure(x)['lufs_m_max'] - bank.measure(old)['lufs_m_max']) / 20)
        encode(np.pad(old, ((31200, 192000 - 31200 - len(old)), (0, 0))), 'v1-' + name + '-preview')
        feedback = critic.score([x])[0]
        description = api.describe(str(OUT / (name + '.wav')))
        report['sounds'][name] = {'score': score, 'measure': bank.measure(x),
                                  'audit': sfx.check(x, category='logo_sting'),
                                  'critic': feedback, 'description': description,
                                  'sha256': hashlib.sha256((OUT / (name + '.wav')).read_bytes()).hexdigest()}
        (OUT / 'ascending-feedback.json').write_text(json.dumps(report, indent=2))
        print(name, description['summary'], flush=True)
    print('Completed ascending harmonies', flush=True)


if __name__ == '__main__':
    main()
