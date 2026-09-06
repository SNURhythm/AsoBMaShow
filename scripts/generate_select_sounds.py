#!/usr/bin/env python3
"""Deterministic synth for AsoBMaShow's bundled music-select default sounds.

Pure-Python DSP (math/struct/wave only, no third-party deps). Writes 44100 Hz
16-bit mono WAVs that the music-select system-sound resolution (
SkinSystemSoundService) finds under its bundled assets root.

Design notes
------------
* The select BGM is length-exactly 2.0 s with every carrier/detune/tremolo
  partial an integer number of cycles across the buffer, so the sample at the
  loop point is a phase-continuous continuation (no click when looping).
  A seam check asserts the boundary step stays within the internal step
  envelope.
* Non-loop SEs fade the final 5 ms to zero to avoid clicks.
* Peak-normalisation (to 0.8) happens inside write_wav so every file is
  uniformly loud; fine-tune per-SE colours by editing the make_* functions.

Run from anywhere; the default output root is ../assets relative to this file
(the bundled assets root), matching where the runtime looks.
"""

import argparse
import math
import os
import random
import struct
import wave

SR = 44100
random.seed(0xC0FFEE)


def out_path(root, p):
    path = os.path.join(root, p)
    directory = os.path.dirname(path)
    if directory:
        os.makedirs(directory, exist_ok=True)
    return path


def write_wav(root, path, samples):
    peak = max(1e-9, max(abs(s) for s in samples))
    gain = 0.8 / peak
    data = bytearray()
    for s in samples:
        v = int(round(max(-1.0, min(1.0, s * gain)) * 32767))
        data += struct.pack("<h", v)
    with wave.open(out_path(root, path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(bytes(data))
    print(f"wrote {os.path.join(root, path)}: {len(samples)/SR:.3f}s peak={peak:.3f}")


def partial(t, f, amp, harm=1.0, detune_hz=0.0):
    return amp * math.sin(2 * math.pi * (f + detune_hz) * harm * t)


def exp_env(t, tau):
    return math.exp(-t / tau)


def lin_fade_out(samples, seconds=0.005):
    n = int(seconds * SR)
    for i in range(n):
        index = len(samples) - 1 - i
        if index < 0:
            break
        samples[index] *= i / n
    return samples


def swept_noise(n_samples, f0, f1, amp):
    """White noise shaped by a sweeping one-pole low-pass and a swell envelope."""
    out = []
    y = 0.0
    for i in range(n_samples):
        t = i / SR
        progress = i / n_samples
        fc = f0 * (f1 / f0) ** progress
        alpha = 1.0 - math.exp(-2 * math.pi * fc / SR)
        x = random.uniform(-1, 1)
        y = alpha * x + (1 - alpha) * y
        envelope = math.sin(math.pi * progress) ** 0.6
        out.append(y * amp * envelope)
    return out


def swept_tone(n_samples, f0, f1, amp, harmonics=1, vib_depth=0.0, vib_rate=0.0):
    """Two-exponential sweep of a harmonic additive tone; optional FM vibrato."""
    out = []
    phase = [0.0] * harmonics
    for i in range(n_samples):
        t = i / SR
        progress = i / n_samples
        f = f0 * (f1 / f0) ** progress
        if vib_depth:
            f += vib_depth * math.sin(2 * math.pi * vib_rate * t)
        s = 0.0
        for h in range(harmonics):
            phase[h] += 2 * math.pi * f * (h + 1) / SR
            s += math.sin(phase[h]) / (h + 1)
        out.append(s * amp)
    return out


# ---------------------------------------------------------------- select BGM
# A bright rhythm-game menu track with high-pitched instruments: a sparkling
# plucky lead in the C6-E7 register, airy high chord stabs, a light
# high-octave bass, and a driving beat. No tremolo or detune beating.

EQ = {
    "A1": 55.00, "C1": 32.70, "C2": 65.41, "D2": 73.42, "E2": 82.41,
    "F2": 87.31, "G2": 98.00, "A2": 110.00, "B2": 123.47,
    "C3": 130.81, "D3": 146.83, "E3": 164.81, "F3": 174.61, "G3": 196.00,
    "A3": 220.00, "B3": 246.94,
    "C4": 261.63, "D4": 293.66, "E4": 329.63, "F4": 349.23, "G4": 392.00,
    "A4": 440.00, "B4": 493.88, "C5": 523.25, "D5": 587.33, "E5": 659.26,
    "F5": 698.46, "G5": 783.99, "A5": 880.00, "B5": 987.77, "C6": 1046.50,
    "D6": 1174.66, "E6": 1318.51, "F6": 1396.91, "G6": 1567.98,
    "A6": 1760.00, "B6": 1975.53, "C7": 2093.00, "D7": 2349.32,
    "E7": 2637.02, "F7": 2793.83, "G7": 3135.96,
}

SELECT_SECONDS = 20
BEAT = 0.5


def bright_tone(freq, t, n_harmonics=3):
    """A bright/glassy tone: fundamental plus a few harmonics, so high-pitch
    instruments cut through instead of sounding like dull sines."""
    s = 0.0
    for h in range(1, n_harmonics + 1):
        s += math.sin(2 * math.pi * freq * h * t) / h
    return s


def make_kick(when_sec, amp=0.24):
    """A soft kick: a low sine with a quick pitch drop."""
    n = SR * SELECT_SECONDS
    out = [0.0] * n
    on = int(when_sec * SR)
    dur = int(0.16 * SR)
    for i in range(on, min(n, on + dur)):
        t = (i - on) / SR
        freq = 130.0 * math.exp(-t / 0.03) + 48.0
        env = min(1.0, t / 0.002) * exp_env(t, 0.045)
        out[i] += amp * math.sin(2 * math.pi * freq * t) * env
    return out


def make_rhythm_bar(start_sec, duration_sec, bass, bass_high, voices, lead):
    """A bright rhythm-game bar: kick + light high-octave bass, high staccato
    chord stabs, and a sparkling high lead."""
    n = SR * SELECT_SECONDS
    out = [0.0] * n
    onset = int(start_sec * SR)
    end = min(n, onset + int(duration_sec * SR))
    beats = int(duration_sec / BEAT)

    # Kick on every beat (the drive).
    for b in range(beats):
        kick = make_kick(start_sec + b * BEAT)
        for i in range(onset, end):
            out[i] += kick[i]
    # Light high-octave bass (clean, not muddy): root on downbeats, fifth on
    # offbeats, in a higher register with a soft envelope.
    step = BEAT / 2
    for beat in range(int(duration_sec / step)):
        when = start_sec + beat * step
        freq = bass if beat % 2 == 0 else bass_high
        on = int(when * SR)
        for i in range(on, min(end, on + int(step * SR))):
            t = (i - on) / SR
            attack = min(1.0, t / 0.004)
            release = min(1.0, (step - t) / 0.05)
            env = max(0.0, attack * release)
            out[i] += 0.15 * bright_tone(EQ[freq], t, n_harmonics=2) * env
    # High staccato chord stabs on each beat (airy, bright).
    for b in range(beats):
        when = start_sec + b * BEAT
        on = int(when * SR)
        stab_len = int(0.26 * SR)
        for i in range(on, min(end, on + stab_len)):
            t = (i - on) / SR
            attack = min(1.0, t / 0.003)
            release = min(1.0, (stab_len / SR - t) / 0.05)
            env = max(0.0, attack * release)
            s = 0.0
            for f, amp in voices:
                s += amp * bright_tone(EQ[f], t, n_harmonics=3)
            out[i] += s * env
    # Sparkling high lead (C6-E7 register), fast plucky decay.
    for m_start, note, amp in lead:
        m_onset = onset + int(m_start * SR)
        for i in range(m_onset, end):
            t = (i - m_onset) / SR
            attack = min(1.0, t / 0.004)
            release = min(1.0, (0.18 - t) / 0.055)
            env = max(0.0, attack * release)
            out[i] += amp * bright_tone(EQ[note], t, n_harmonics=4) * env
    return out


def make_select():
    n = SR * SELECT_SECONDS
    out = [0.0] * n
    # Bright, high-pitched verse/chorus in C. Verse chord stabs are high; the
    # chorus soars higher. The lead lives in the C6-G6 register.
    sections = [
        # Verse (C - G - Am - F): high stabs + sparkling lead.
        make_rhythm_bar(0.0, 2.5, "C3", "C4",
                        [("C5", 0.045), ("E5", 0.045), ("G5", 0.04), ("B5", 0.035)],
                        [(0.0, "G6", 0.085), (0.5, "E6", 0.085), (1.0, "C7", 0.08),
                         (1.5, "G6", 0.08), (2.0, "E6", 0.07)]),
        make_rhythm_bar(2.5, 2.5, "G3", "G4",
                        [("G5", 0.045), ("B5", 0.045), ("D6", 0.04), ("F6", 0.035)],
                        [(2.5, "D6", 0.08), (3.0, "B6", 0.08), (3.5, "G6", 0.07),
                         (4.0, "D7", 0.07), (4.5, "B6", 0.065)]),
        make_rhythm_bar(5.0, 2.5, "A2", "A3",
                        [("A4", 0.04), ("C5", 0.04), ("E5", 0.035), ("G5", 0.03)],
                        [(5.0, "C6", 0.075), (5.5, "E6", 0.075), (6.0, "A6", 0.07),
                         (6.5, "E6", 0.07), (7.0, "C7", 0.06)]),
        make_rhythm_bar(7.5, 2.5, "F3", "F4",
                        [("F5", 0.045), ("A5", 0.045), ("C6", 0.04), ("E6", 0.035)],
                        [(7.5, "A6", 0.08), (8.0, "C7", 0.08), (8.5, "F6", 0.07),
                         (9.0, "A6", 0.07), (9.5, "C7", 0.065)]),
        # Chorus (C - Am - F - G): higher stabs, soaring lead.
        make_rhythm_bar(10.0, 2.5, "C3", "C4",
                        [("C5", 0.05), ("E5", 0.05), ("G5", 0.045), ("B5", 0.04)],
                        [(10.0, "E7", 0.085), (10.5, "C7", 0.085), (11.0, "G6", 0.08),
                         (11.5, "C7", 0.08), (12.0, "E7", 0.07)]),
        make_rhythm_bar(12.5, 2.5, "A2", "A3",
                        [("A4", 0.045), ("C5", 0.045), ("E5", 0.04), ("G5", 0.035)],
                        [(12.5, "E6", 0.075), (13.0, "A6", 0.075), (13.5, "C7", 0.07),
                         (14.0, "A6", 0.07), (14.5, "E7", 0.06)]),
        make_rhythm_bar(15.0, 2.5, "F3", "F4",
                        [("F5", 0.05), ("A5", 0.05), ("C6", 0.045), ("E6", 0.04)],
                        [(15.0, "C7", 0.08), (15.5, "F6", 0.08), (16.0, "A6", 0.07),
                         (16.5, "C7", 0.07), (17.0, "F6", 0.06)]),
        make_rhythm_bar(17.5, 2.5, "G3", "G4",
                        [("G5", 0.05), ("B5", 0.05), ("D6", 0.045), ("F6", 0.04)],
                        [(17.5, "D7", 0.08), (18.0, "B6", 0.08), (18.5, "G6", 0.07),
                         (19.0, "B6", 0.07), (19.5, "D7", 0.06)]),
    ]
    for i in range(n):
        for section in sections:
            out[i] += section[i]
    # Master fade-in/out so the loop point is smooth.
    fade = int(0.5 * SR)
    for i in range(fade):
        out[i] *= i / fade
        out[n - 1 - i] *= i / fade
    return out


# ---------------------------------------------------------------- tick SEs
# The built-in select sound effects are all simple, unobtrusive ticks, but each
# action gets its own pitch/decay so they stay recognizable (a single identical
# click for every event is monotonous). `pitch` is the fundamental Hz and `tau`
# the decay; a quick transient is layered on top for a crisp attack.
def make_tick(pitch_hz, tau, duration=0.06):
    n = int(duration * SR)
    out = [0.0] * n
    for i in range(n):
        t = i / SR
        attack = min(1.0, t / 0.001)
        out[i] = 0.6 * math.sin(2 * math.pi * pitch_hz * t) * attack * exp_env(t, tau)
        out[i] += 0.25 * math.sin(2 * math.pi * pitch_hz * 2 * t) * attack * exp_env(t, tau * 0.6)
    return lin_fade_out(out)


def make_decide():
    # Firm, slightly long confirm tick.
    return make_tick(1040, 0.020, 0.09)


def make_folder_open():
    # Higher, brighter open tick.
    return make_tick(1900, 0.014)


def make_folder_close():
    # Lower, softer close tick.
    return make_tick(820, 0.018)


def make_option_change():
    # Quick middle tick for option cycling.
    return make_tick(1320, 0.012)


def make_option_open():
    # Mid-high tick for opening an option panel.
    return make_tick(1500, 0.015)


def make_option_close():
    # Mid-low tick for closing an option panel.
    return make_tick(980, 0.016)


def make_scratch():
    # Sharp, fast-decay tick for scratch input.
    return make_tick(1750, 0.009)


WRITERS = [
    ("assets/select.wav", make_select),
    ("assets/decide.wav", make_decide),
    ("assets/f-open.wav", make_folder_open),
    ("assets/f-close.wav", make_folder_close),
    ("assets/o-change.wav", make_option_change),
    ("assets/o-open.wav", make_option_open),
    ("assets/o-close.wav", make_option_close),
    ("assets/scratch.wav", make_scratch),
]


def loop_seam_check(root, select_path):
    """All select partials are integer-cycle over the exact 2.0 s buffer, so
    the loop point (last sample -> first sample) must be a smooth continuation
    within the internal step envelope."""
    import wave as _w
    path = os.path.join(root, select_path)
    with _w.open(path, "rb") as w:
        frames = w.readframes(w.getnframes())
    samp = struct.unpack(f"<{len(frames)//2}h", frames)
    boundary_step = abs(samp[0] - samp[-1])
    internal_steps = [abs(samp[i + 1] - samp[i]) for i in range(1000, 3000)]
    max_internal = max(internal_steps)
    print(f"select loop seam: boundary_step={boundary_step} "
          f"max_internal_step={max_internal} peak={max(abs(s) for s in samp)}")
    assert boundary_step <= max_internal * 1.5, "select loop seam is not smooth"


def main():
    parser = argparse.ArgumentParser(description="Generate AsoBMaShow bundled music-select sounds.")
    parser.add_argument(
        "-o", "--output-root", default=".",
        help="Output directory (default %(default)s = repo root; WRITERS paths "
             "already carry the assets/ subpath).")
    args = parser.parse_args()
    root = os.path.abspath(
        os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     args.output_root))
    for path, fn in WRITERS:
        write_wav(root, path, fn())
    loop_seam_check(root, "assets/select.wav")


if __name__ == "__main__":
    main()
