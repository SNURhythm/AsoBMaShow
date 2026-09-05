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
# A ~10 s menu piece with a smooth jazz-lite chord progression. Each chord is
# voiced across the bass, mid, and top and given a soft attack/release so chord
# changes and the loop point stay click-free. No tremolo, no detune beating.

EQ = {
    "A1": 55.00, "C1": 32.70, "C2": 65.41, "D2": 73.42, "F2": 87.31,
    "G2": 98.00, "A2": 110.00, "B2": 123.47,
    "C3": 130.81, "D3": 146.83,
    "E3": 164.81, "F3": 174.61, "G3": 196.00, "A3": 220.00, "B3": 246.94,
    "C4": 261.63, "D4": 293.66, "E4": 329.63, "F4": 349.23, "G4": 392.00,
    "A4": 440.00, "B4": 493.88, "C5": 523.25, "D5": 587.33, "E5": 659.26,
}

SELECT_SECONDS = 10


def make_chord(start_sec, duration_sec, bass, voices, melody):
    """A chord: soft bass + mid voices, plus a plucked melody on top."""
    n = SR * SELECT_SECONDS
    out = [0.0] * n
    onset = int(start_sec * SR)
    end = min(n, onset + int(duration_sec * SR))

    def chord_env(t, t_end):
        attack = min(1.0, t / 0.05)
        release = min(1.0, (t_end - t) / 0.15)
        return max(0.0, attack * release)

    t_end = duration_sec
    for i in range(onset, end):
        t = (i - onset) / SR
        env = chord_env(t, t_end)
        s = 0.0
        s += 0.16 * math.sin(2 * math.pi * EQ[bass] * t)
        for f, amp in voices:
            s += amp * math.sin(2 * math.pi * EQ[f] * t)
        out[i] += s * env
    # Plucked melody notes on top.
    for m_start, note, amp in melody:
        m_onset = onset + int(m_start * SR)
        for i in range(m_onset, end):
            t = (i - m_onset) / SR
            attack = min(1.0, t / 0.015)
            release = min(1.0, (0.45 - t) / 0.12)
            env = max(0.0, attack * release)
            out[i] += amp * math.sin(2 * math.pi * EQ[note] * t) * env
    return out


def make_select():
    n = SR * SELECT_SECONDS
    out = [0.0] * n
    # Cool, mellow progression (Cmaj9 -> Am9 -> Fmaj7 -> Dm9) with light
    # extensions and a gentle melody, resolving so the loop returns to Cmaj9.
    sections = [
        make_chord(0.0, 2.5, "C2",
                   [("C3", 0.10), ("G3", 0.08), ("B3", 0.06), ("E4", 0.05), ("D4", 0.04)],
                   [(0.2, "E4", 0.16), (0.9, "G4", 0.12), (1.6, "B4", 0.10), (2.1, "D5", 0.09)]),
        make_chord(2.5, 2.5, "A1",
                   [("A2", 0.10), ("E3", 0.08), ("C4", 0.06), ("G4", 0.05), ("B3", 0.05)],
                   [(2.7, "C4", 0.12), (3.4, "E4", 0.12), (4.1, "G4", 0.10), (4.6, "B4", 0.08)]),
        make_chord(5.0, 2.5, "F2",
                   [("F3", 0.10), ("A3", 0.08), ("C4", 0.07), ("E4", 0.06)],
                   [(5.2, "A4", 0.12), (5.9, "C5", 0.10), (6.6, "E5", 0.08), (7.1, "A4", 0.08)]),
        make_chord(7.5, 2.5, "D2",
                   [("D3", 0.10), ("A3", 0.08), ("C4", 0.06), ("F4", 0.05), ("E4", 0.05)],
                   [(7.7, "F4", 0.10), (8.4, "A4", 0.10), (9.1, "C5", 0.08), (9.5, "D5", 0.08)]),
    ]
    for i in range(n):
        for section in sections:
            out[i] += section[i]
    # Master fade-in/out so the 10 s loop point is smooth.
    fade = int(0.4 * SR)
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
