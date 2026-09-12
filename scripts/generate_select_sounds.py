#!/usr/bin/env python3
"""Generate the original Signal Select BGM and bundled selector sound effects.

The score is 32 bars of 4/4 at 128 BPM (60 seconds), rendered deterministically
with standard-library Python DSP. Note releases and delay tails wrap around the
loop; there is no master fade that drops the groove at each repetition.

BGM is mono 44.1 kHz Ogg Vorbis; the short UI effects remain 16-bit WAVs.
Encoding requires sndfile-convert from libsndfile's command-line tools.
The encoder round-trip is checked for sample count, clipping, clicks and gaps
before publication. An obsolete assets/select.wav is removed only after the
replacement Ogg has passed validation, so the resolver cannot prefer stale BGM.

Run from anywhere. --output-root is relative to the repository root unless
absolute; output filenames already include assets/.
"""

import argparse
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import wave

from select_bgm import render_select

SR = 44100


def out_path(root, path):
    destination = os.path.join(root, path)
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    return destination


def write_wav(root, path, samples):
    peak = max(1e-9, max(abs(sample) for sample in samples))
    gain = 0.8 / peak
    data = bytearray()
    for sample in samples:
        value = int(round(max(-1.0, min(1.0, sample * gain)) * 32767))
        data += struct.pack("<h", value)
    with wave.open(out_path(root, path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(SR)
        output.writeframes(data)
    print(f"wrote {os.path.join(root, path)}: {len(samples)/SR:.3f}s peak={peak:.3f}")


def write_float_wav(path, samples):
    """Keep encoder headroom: sndfile-convert peak-normalizes integer inputs."""
    peak = max(abs(sample) for sample in samples)
    if not math.isfinite(peak) or peak <= 0 or any(not math.isfinite(sample) for sample in samples):
        raise ValueError("BGM must contain finite, non-silent audio")
    gain = .8 / peak
    data = bytearray()
    for sample in samples:
        data += struct.pack("<f", sample * gain)
    with Path(path).open("wb") as output:
        output.write(struct.pack("<4sI4s", b"RIFF", 48 + len(data), b"WAVE"))
        output.write(struct.pack("<4sIHHIIHH", b"fmt ", 16, 3, 1, SR, SR * 4, 4, 32))
        output.write(struct.pack("<4sII", b"fact", 4, len(samples)))
        output.write(struct.pack("<4sI", b"data", len(data)))
        output.write(data)


def exp_env(time, tau):
    return math.exp(-time / tau)


def lin_fade_out(samples, seconds=0.005):
    count = int(seconds * SR)
    for offset in range(count):
        index = len(samples) - 1 - offset
        if index < 0:
            break
        samples[index] *= offset / count
    return samples


def make_select():
    return render_select(SR)


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
    ("assets/decide.wav", make_decide),
    ("assets/f-open.wav", make_folder_open),
    ("assets/f-close.wav", make_folder_close),
    ("assets/o-change.wav", make_option_change),
    ("assets/o-open.wav", make_option_open),
    ("assets/o-close.wav", make_option_close),
    ("assets/scratch.wav", make_scratch),
]


def read_wav(path):
    with wave.open(str(path), "rb") as source:
        if source.getnchannels() != 1 or source.getsampwidth() != 2:
            raise ValueError("loop validation requires mono 16-bit PCM")
        sample_rate = source.getframerate()
        frames = source.readframes(source.getnframes())
    return sample_rate, struct.unpack(f"<{len(frames) // 2}h", frames)


def loop_seam_check(root, select_path):
    sample_rate, samples = read_wav(os.path.join(root, select_path))
    if len(samples) < sample_rate:
        raise ValueError("loop is too short to validate")
    peak = max(abs(sample) for sample in samples)
    if peak == 0 or peak >= 32767:
        raise ValueError("loop is silent or clipped")
    edge = sample_rate // 20
    window = sample_rate // 2

    def energy(values):
        return math.sqrt(sum(value * value for value in values) / len(values))

    for end, body in ((samples[:edge], samples[:window]),
                      (samples[-edge:], samples[-window:])):
        if energy(end) < .3 * energy(body):
            raise ValueError("silent gap or energy dip at the loop boundary")
    boundary_step = abs(samples[0] - samples[-1])
    internal_step = max(abs(after - before)
                        for neighbors in (samples[:edge], samples[-edge:])
                        for before, after in zip(neighbors, neighbors[1:]))
    if boundary_step > min(peak * .2, internal_step * 1.5):
        raise ValueError("click at the loop boundary")
    print(f"loop: {len(samples) / sample_rate:.3f}s, peak={peak / 32768:.3f}, "
          f"boundary step={boundary_step / 32768:.5f}")
    return sample_rate, len(samples)


def write_select_ogg(root, samples, encoder):
    destination = Path(out_path(root, "assets/select.ogg"))
    with tempfile.TemporaryDirectory(prefix=".select-render-", dir=destination.parent) as directory:
        temporary = Path(directory)
        source = temporary / "source.wav"
        encoded = temporary / "select.ogg"
        decoded = temporary / "roundtrip.wav"
        write_float_wav(source, samples)
        subprocess.run([encoder, "-vorbis", str(source), str(encoded)], check=True)
        subprocess.run([encoder, "-pcm16", str(encoded), str(decoded)], check=True)
        rate, count = loop_seam_check(temporary, decoded.name)
        if rate != SR or count != len(samples):
            raise ValueError("Vorbis round-trip changed the loop duration")
        if encoded.stat().st_size >= 1024 * 1024:
            raise ValueError("select BGM exceeds the 1 MiB encoded budget")
        with encoded.open("rb") as stream:
            header = stream.read(64)
        if not header.startswith(b"OggS") or b"vorbis" not in header:
            raise ValueError("encoder did not produce Ogg Vorbis")
        os.replace(encoded, destination)
    legacy = destination.with_suffix(".wav")
    if legacy.exists():
        legacy.unlink()
    print(f"wrote {destination}: {destination.stat().st_size / 1024:.1f} KiB")


def main():
    parser = argparse.ArgumentParser(description="Generate AsoBMaShow selector music and sounds.")
    parser.add_argument("-o", "--output-root", default=".",
                        help="Output root relative to the repository, or an absolute directory.")
    parser.add_argument("--vorbis-encoder", default="sndfile-convert",
                        help="Path to sndfile-convert (requires libsndfile with Vorbis support).")
    args = parser.parse_args()
    encoder = shutil.which(args.vorbis_encoder)
    if encoder is None:
        parser.error("sndfile-convert is required; install libsndfile tools or set --vorbis-encoder")
    root = Path(__file__).resolve().parents[1] / args.output_root
    write_select_ogg(root, make_select(), encoder)
    for path, render in WRITERS:
        write_wav(root, path, render())


if __name__ == "__main__":
    main()
