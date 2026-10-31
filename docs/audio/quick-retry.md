# Quick Retry cue

`assets/quick-retry.wav` is a short descending “피유웅” cue generated with
[Earshot](https://github.com/Leonxlnx/earshot), commit
`591fa3e028032e6dce56620a20e932e35667e8d8` (0.4.0). It is 380 ms,
48 kHz stereo PCM16, with a −13 dBFS peak. The game preloads it once and applies
master volume at playback. Earshot is not a runtime dependency.

## Reproduce and review

Install the pinned Earshot checkout in a Python environment, then run:

```sh
python scripts/generate_quick_retry_sound.py --feedback
```

This renders the modified `pro_zip/down_warm` recipe, checks the quantized WAV
with Earshot's SFX audit, and runs its local `describe` and `match` feedback.
The first feedback run downloads Earshot's pinned CLAP and AST models.
Omit `--feedback` for generation and the model-free technical audit only.
The script writes [the feedback record](quick-retry-earshot.json).

## Feedback and refinement

The initial bank render (320 ms) received a plausible target match (`z = 2.8`),
but its description favored an error buzz and the alternative comparison
strongly favored a sharp laser. Its measured attack/decay was 8/133 ms.

The revision reduces sawtooth and harmonic energy, removes saturation, lowers
frequency modulation, and lengthens the descending glide. Earshot measures a
14 ms attack and 221 ms decay, and gives the requested description a strong
match (`z = 3.1`). The quantized asset has zero detected clicks, clipping runs,
or over-range samples, and no tail warnings.

The model still favors the laser alternative in forced choice and calls an
error buzz its closest general vocabulary item. The target score therefore
does not establish that a listener will prefer this take. The design keeps that
short synthetic descending character, at a restrained level; the WAV is the
listening reference. The full model results and synthesis parameters are saved
in the feedback record, including the unfavorable alternatives.

The generator uses Earshot's MIT-licensed recipe; its notice is retained in
`assets/legal/earshot.txt`.
