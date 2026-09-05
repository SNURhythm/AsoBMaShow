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
    # Cmaj9-ish warm pad; every partial is integer Hz so its cycles are integral.
    pad = [
        (66, 0.30),   # C2 sub
        (131, 0.34),  # C3
        (330, 0.24),  # E4
        (392, 0.18),  # G4
        (494, 0.10),  # B4
        (587, 0.05),  # D5
    ]
    for i in range(n):
        t = i / SR
        s = 0.0
        for f, amp in pad:
            s += partial(t, f, amp)
            s += partial(t, f, amp * 0.4, detune_hz=1.0)  # subtle chorus
        tremolo = 0.72 + 0.28 * math.sin(2 * math.pi * 0.5 * t - math.pi / 2)
        out[i] = s * tremolo
    return out


# ---------------------------------------------------------------- decide
def make_decide():
    n = int(0.6 * SR)
    out = [0.0] * n
    for i in range(n):  # low thock for weight
        t = i / SR
        out[i] += 0.5 * math.sin(2 * math.pi * 82 * t) * exp_env(t, 0.14)
    notes = [(0.00, 523, 0.5), (0.07, 784, 0.5), (0.14, 1046, 0.55)]  # C5 G5 C6
    for start, f, amp in notes:
        off = int(start * SR)
        end = min(n, off + int(0.18 * SR))
        for i in range(off, end):
            t = (i - off) / SR
            out[i] += amp * math.sin(2 * math.pi * f * t) * min(1.0, t / 0.004) * exp_env(t, 0.10)
    for i in range(int(0.16 * SR), n):  # bright sustain on top
        t = (i - int(0.16 * SR)) / SR
        out[i] += 0.16 * math.sin(2 * math.pi * 2093 * t) * min(1.0, t / 0.006) * exp_env(t, 0.25)
    return lin_fade_out(out)


# ---------------------------------------------------------------- folder open
def make_folder_open():
    n = int(0.40 * SR)
    tone = swept_tone(n, 220, 1760, 0.5, harmonics=2, vib_depth=18, vib_rate=30)
    noise = swept_noise(n, 400, 3200, 0.45)
    return lin_fade_out([tone[i] + noise[i] for i in range(n)])


# ---------------------------------------------------------------- folder close
def make_folder_close():
    n = int(0.35 * SR)
    tone = swept_tone(n, 1400, 160, 0.55, harmonics=2)
    noise = swept_noise(n, 3000, 400, 0.4)
    out = [tone[i] + noise[i] for i in range(n)]
    for i in range(int(0.16 * SR), n):  # damped thump at the end
        t = (i - int(0.16 * SR)) / SR
        out[i] += 0.35 * math.sin(2 * math.pi * 90 * t) * exp_env(t, 0.05)
    return lin_fade_out(out)


# ---------------------------------------------------------------- option change
def make_option_change():
    n = int(0.12 * SR)
    out = [0.0] * n
    for i in range(n):
        t = i / SR
        out[i] += 0.55 * math.sin(2 * math.pi * 1568 * t) * min(1.0, t / 0.0015) * exp_env(t, 0.030)
        out[i] += 0.22 * math.sin(2 * math.pi * 3136 * t) * min(1.0, t / 0.0015) * exp_env(t, 0.020)
        out[i] += 0.18 * math.sin(2 * math.pi * 196 * t) * min(1.0, t / 0.003) * exp_env(t, 0.045)
    return lin_fade_out(out)


# ---------------------------------------------------------------- option open
def make_option_open():
    n = int(0.40 * SR)
    noise = swept_noise(n, 500, 2400, 0.4)
    dyad = [0.0] * n
    for i in range(n):
        t = i / SR
        dyad[i] = (0.18 * math.sin(2 * math.pi * 523 * t) +
                   0.14 * math.sin(2 * math.pi * 784 * t)) * min(1.0, t / 0.008) * exp_env(t, 0.22)
    return lin_fade_out([noise[i] + dyad[i] for i in range(n)])


# ---------------------------------------------------------------- option close
def make_option_close():
    n = int(0.30 * SR)
    noise = swept_noise(n, 2400, 500, 0.38)
    dyad = [0.0] * n
    for i in range(n):
        t = i / SR
        dyad[i] = (0.16 * math.sin(2 * math.pi * 392 * t) +
                   0.12 * math.sin(2 * math.pi * 330 * t)) * min(1.0, t / 0.006) * exp_env(t, 0.18)
    return lin_fade_out([noise[i] + dyad[i] for i in range(n)])


# ---------------------------------------------------------------- scratch
def make_scratch():
    n = int(0.5 * SR)
    stops = [(0.0, 300, 900), (0.15, 900, 350), (0.30, 350, 800)]
    out = []
    for _start, f0, f1 in stops:
        out.extend(swept_tone(int(0.15 * SR), f0, f1, 0.6, harmonics=3,
                              vib_depth=40, vib_rate=45))
    out = out[:n]
    noise = swept_noise(len(out), 800, 4000, 0.3)
    for i in range(len(out)):
        out[i] += noise[i]
    return lin_fade_out(out)


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
