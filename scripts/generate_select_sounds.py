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


# ---------------------------------------------------------------- select
def make_select():
    n = SR * 2  # exactly 2.000 s -> seamless loop
    out = [0.0] * n
    # A calm, melodic select BGM: a soft warm chord bed with a gentle arpeggio
    # on top. No tremolo and no detune beating (both were disorienting), and
    # every sustained partial is integer Hz so the 2.0 s loop is seamless.
    #
    # Chord bed (Cmaj6-ish, wide and soft): every tone is integer Hz -> integer
    # cycles over the buffer, so it sustains seamlessly.
    bed = [
        (131, 0.16),  # C3
        (196, 0.12),  # G3
        (330, 0.10),  # E4
        (440, 0.06),  # A4
    ]
    for i in range(n):
        t = i / SR
        s = 0.0
        for f, amp in bed:
            s += partial(t, f, amp)
        out[i] = s
    # Arpeggio (calm harp/music-box motif). Each note is plucked with a soft
    # attack and release so it starts and ends near zero, keeping the loop
    # click-free. Frequencies are integer Hz.
    notes = [
        (0.00, 330, 0.22),  # E4
        (0.35, 523, 0.18),  # C5
        (0.70, 440, 0.16),  # A4
        (1.05, 392, 0.16),  # G4
        (1.40, 330, 0.14),  # E4
        (1.70, 587, 0.12),  # D5
    ]
    for start, freq, amp in notes:
        onset = int(start * SR)
        for i in range(onset, n):
            t = (i - onset) / SR
            attack = min(1.0, t / 0.015)
            release = min(1.0, (0.30 - t) / 0.12)
            env = max(0.0, attack * release)
            out[i] += amp * math.sin(2 * math.pi * freq * t) * env
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
