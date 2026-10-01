#!/usr/bin/env python3
"""Generate the original, soft two-note Guided Access confirmation chime."""
import math
import struct
import wave
from pathlib import Path

RATE = 48000
DURATION = 0.62
OUTPUT = Path(__file__).resolve().parents[1] / "assets/guided-access-enabled.wav"


def sample(t):
    value = 0.0
    for start, frequency, gain in ((0.0, 783.9909, 0.20), (0.11, 1046.5023, 0.24)):
        age = t - start
        if age < 0:
            continue
        envelope = (1 - math.exp(-age / 0.004)) * math.exp(-age / 0.12)
        phase = 2 * math.pi * frequency * age
        value += gain * envelope * (
            math.sin(phase) + 0.20 * math.sin(2.01 * phase) +
            0.04 * math.sin(3.98 * phase))
    fade = min(1.0, max(0.0, (DURATION - t) / 0.10))
    return round(32767 * value * math.sin(fade * math.pi / 2) ** 2)


def main():
    samples = [sample(i / RATE) for i in range(round(RATE * DURATION))]
    samples[0] = samples[-1] = 0
    with wave.open(str(OUTPUT), "wb") as output:
        output.setparams((1, 2, RATE, len(samples), "NONE", "not compressed"))
        output.writeframes(struct.pack(f"<{len(samples)}h", *samples))
    print(f"{OUTPUT.name}: {DURATION:.2f}s, peak {max(map(abs, samples)) / 32767:.3f}")


if __name__ == "__main__":
    main()
