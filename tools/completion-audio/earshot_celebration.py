#!/usr/bin/env python3
"""Original Earshot/DSP celebration. No recordings or external audio inputs."""
import hashlib
import json
from pathlib import Path

import numpy as np
from scipy import signal
from earshot import api
from earshot.forge import bank, dsp, sfx
from earshot.forge.critic import Critic
from earshot.forge.recipes.impact import hall_ir
from earshot.forge.recipes import mallet
from melodies import encode

OUT = Path(__file__).resolve().parent / 'out'
SR = dsp.SR


def room(x, seed, wet=.18, rt=.55):
    y = np.zeros_like(x)
    for ch in range(2):
        ir = hall_ir(dsp.rng(seed + ch), rt, .8, size=.65,
                     predelay=.017 + ch * .003, seconds=.65)
        y[:, ch] = signal.fftconvolve(x[:, ch], ir)[:len(x)]
    return x + wet * dsp.butter(y, 'low', 5800, 2)


def applause(seed):
    r = dsp.rng(seed)
    layers = []
    # One short collective burst, with little timing differences among hands.
    for event in [.02, .14, .27, .40]:
        for person in range(5):
            t = np.arange(round(.09 * SR)) / SR
            x = dsp.butter(r.standard_normal(len(t)), 'high', 650, 2)
            x = dsp.butter(x, 'low', 4600, 2)
            x *= (1 - np.exp(-t / .0018)) * np.exp(-t / .014)
            x = dsp.normalize(dsp.fade(x, .001, .025), -1)
            layers.append((x, event + r.uniform(0, .026), -9, r.uniform(-.8, .8)))
    return dsp.normalize(dsp.fade(room(dsp.stack(layers, length=.66), seed + 20,
                                     wet=.12, rt=.30), .008, .18), -1)


SCORES = {
    'full-combo': {
        'palette': 'Three short harp strums and plucked bass; C-major cadence',
        'chords': [(.08, [523.251, 659.255, 783.991]),
                   (.27, [391.995, 493.883, 587.33]),
                   (.44, [523.251, 659.255, 783.991, 1046.502])],
        'bass_hz': [130.813, 97.999, 130.813],
        'harmony': [130.813, 195.998, 329.628],
        'accent_s': .08, 'claps_start_s': .32, 'duration_s': 1.35},
    'perfect': {
        'palette': 'Harp-like string cascade and a sustained D-major-six-nine chord',
        'notes': [(293.665, .08), (440.0, .122), (587.33, .164), (739.989, .206),
                  (880.0, .248), (1318.51, .29), (1479.978, .355), (1174.66, .44)],
        'harmony': [146.832, 220.0, 293.665, 369.994, 493.883, 659.255],
        'accent_s': .08, 'claps_start_s': .32, 'duration_s': 1.65},
    'max': {
        'palette': 'Warm harp opening, an upper-register flourish and a full finish on Perfect\'s peak note',
        'notes': [(277.1825, 0, -15), (311.127, .07, -14), (369.9945, .14, -8),
                  (554.365, .19, -12), (739.989, .235, -10), (1108.73, .275, -9),
                  (1479.978, .31, -8), (1244.508, .345, -11), (1108.73, .38, -12),
                  (1244.508, .415, -11), (1396.912, .45, -10), (1479.978, .485, -9)],
        'pans': [-.4, -.2, 0, -.5, -.25, .1, .45, .3, .05, -.25, -.1, .2],
        'harmony_changes': [(.14, [123.471, 184.997, 311.127, 466.164, 554.365]),
                            (.34, [138.591, 207.652, 277.183, 369.994, 493.883])],
        'final_chord': [184.997, 277.183, 415.305, 466.164, 932.328, 1479.978],
        'final_harmony': [184.997, 277.183, 311.127, 415.305, 466.164, 739.989],
        'final_chord_s': .56,
        'accent_s': .14, 'claps_start_s': .56, 'claps_gain_db': -16,
        'duration_s': 1.95},
}


def string_note(freq, seed, bass=False, pan=0, final=False, gate=None):
    params = {**mallet.DEFAULTS, 'style': 'pluck', 'f0': freq,
              'decay': .25 if bass else .55, 'slope': .7, 'tilt': 1.25,
              'pos': .21, 'contact': .0012 if bass else .00065,
              'onset': .004, 'gate': .24 if final else .16, 'gate_tau': .055,
              'body': -18 if bass else -28, 'body_f': 170,
              'hp': 65 if bass else 180, 'lp': 2400 if bass else 6800,
              'room': 0, 'width': .04 if bass else .25, 'pan': pan,
              'strike': -55, 'oct': -60, 'jit': .1}
    if gate is not None:
        params.update(gate=gate, gate_tau=.04)
    return mallet.render(params, dsp.rng(seed))


def soft_chord_note(freq, seed, gate=None):
    r = dsp.rng(seed)
    t = np.arange(round(1.4 * SR)) / SR
    x = np.zeros((len(t), 2))
    env = (1 - np.exp(-t / .065)) * np.exp(-t / .32)
    if gate is not None:
        env *= np.exp(-np.maximum(t - gate, 0) / .06)
    for ch in range(2):
        for detune in [-3, 3]:
            phase = 2 * np.pi * freq * 2 ** ((detune + ch) / 1200) * t + r.uniform(0, 6.28)
            for harmonic, gain in [(1, 1), (2, .28), (3, .13), (4, .04)]:
                x[:, ch] += gain * np.sin(harmonic * phase) * env
    return dsp.normalize(dsp.fade(x, .012, .22), -1)


def full_combo_music():
    score = SCORES['full-combo']
    layers = []
    for i, (at, notes) in enumerate(score['chords']):
        for j, freq in enumerate(notes):
            layers.append((string_note(freq, 8400 + i * 10 + j,
                                       pan=-.3 + j * .2, final=i == 2),
                           at + j * .014, -9 if i == 2 else -11))
        layers.append((string_note(score['bass_hz'][i], 8450 + i, bass=True), at, -17))
    for i, freq in enumerate(score['harmony']):
        layers.append((soft_chord_note(freq, 8460 + i), .08, -25))
    return dsp.fade(room(dsp.stack(layers, length=score['duration_s']),
                         8470, wet=.18, rt=.48), .003, .18)


def perfect_music():
    score = SCORES['perfect']
    layers = []
    for i, (freq, at) in enumerate(score['notes']):
        last = i == len(score['notes']) - 1
        layers.append((string_note(freq, 8200 + i, pan=-.45 + i * .12, final=last),
                       at, -5 if last else -8))
    for i, freq in enumerate(score['harmony']):
        layers.append((soft_chord_note(freq, 8250 + i), .08, -19))
    return dsp.fade(room(dsp.stack(layers, length=score['duration_s']),
                         8270, wet=.24, rt=.68), .003, .22)


def music(name='max'):
    if name == 'full-combo':
        return full_combo_music()
    if name == 'perfect':
        return perfect_music()
    # Keep the warm opening and 140 ms accent. The second phrase stays high,
    # rising back to Perfect's F#6 peak and holding that pitch in the final chord.
    score = SCORES[name]
    layers = []
    for i, (freq, at, gain) in enumerate(score['notes']):
        layers.append((string_note(freq, 8700 + i, pan=score['pans'][i],
                                   gate=.075 if i >= 3 else .12), at, gain))
    for i, freq in enumerate(score['final_chord']):
        layers.append((string_note(freq, 8740 + i, bass=i == 0,
                                   pan=0 if i == 0 else -.55 + (i - 1) * .275,
                                   final=True, gate=.34),
                       score['final_chord_s'] + i * .008, -9 if i == 5 else -12))
    for change, (at, notes) in enumerate(score['harmony_changes']):
        for i, freq in enumerate(notes):
            layers.append((soft_chord_note(freq, 8750 + change * 10 + i, gate=.14), at, -23))
    for i, freq in enumerate(score['final_harmony']):
        layers.append((soft_chord_note(freq, 8780 + i), score['final_chord_s'], -19))
    return dsp.fade(room(dsp.stack(layers, length=score['duration_s']),
                         8790, wet=.26, rt=.72), .003, .22)


def main():
    OUT.mkdir(exist_ok=True)
    report = {'revision': 'earshot-max-high-finish', 'prototype_only': True,
              'external_audio_inputs': [], 'source_method': 'Earshot DSP synthesis only',
              'vocal_layers': 0, 'claps_duration_s': .66, 'default_claps_gain_db': -19,
              'preview_lead_in_s': .65, 'sounds': {}}
    claps = applause(7800)
    targets = {
        'full-combo': 'Three bright bouncy harp strums with soft plucked bass, a short joyful major-key victory flourish.',
        'perfect': 'A bright magical harp flourish rising into a soft lush major chord, shimmering and celebratory with a smooth spacious decay.',
        'max': 'A joyful triumphant harp flourish with a warm opening, a fast rising sweep that stays in the upper register, and a full final chord crowned by a ringing high note with a brief applause accent.'}
    for outcome, score in SCORES.items():
        name = 'earshot-' + outcome
        crowd = dsp.stack([(claps, score['claps_start_s'], score.get('claps_gain_db', -19))],
                          length=score['duration_s'])
        x = dsp.fade(music(outcome) + crowd, .003, .18)
        m = bank.measure(x)
        x *= 10 ** (min(-17 - m['lufs_m_max'], -4 - m['peak_db']) / 20)
        encode(x, name)
        encode(np.pad(x, ((31200, 192000 - 31200 - len(x)), (0, 0))), name + '-preview')
        desc = api.describe(str(OUT / (name + '.wav')))
        critic = Critic(targets[outcome], alts=['An error alert.', 'An animal roar.',
                        'A plain drum loop.', 'A harsh scratch sound.'],
                        mode='designed', category='logo_sting')
        report['sounds'][name] = {'score': score, 'brief': targets[outcome], 'measure': bank.measure(x),
                                  'audit': sfx.check(x, category='logo_sting'),
                                  'critic': critic.score([x])[0], 'description': desc,
                                  'sha256': hashlib.sha256((OUT / (name + '.wav')).read_bytes()).hexdigest()}
        (OUT / 'earshot-celebration-feedback.json').write_text(json.dumps(report, indent=2))
        print(name, desc['summary'], flush=True)


if __name__ == '__main__':
    main()
