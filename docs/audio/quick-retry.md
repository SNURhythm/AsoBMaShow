# Quick Retry cue

`assets/quick-retry.wav` is a minimal digital reset cue: a soft click with a
quiet airy tail, generated with [Earshot](https://github.com/Leonxlnx/earshot),
commit `591fa3e028032e6dce56620a20e932e35667e8d8` (0.4.0). It is 200 ms,
48 kHz stereo PCM16, with a −16 dBFS peak. The game preloads it once and applies
master volume at playback. Earshot is not a runtime dependency.

## Reproduce and review

Install the pinned Earshot checkout in a Python environment, then run:

```sh
python scripts/generate_quick_retry_sound.py --feedback
```

The generator combines `pro_click/soft` at −16 dBFS with `swish/flick` at
−29 dBFS. Both start together, giving one immediate gesture without a lead-in.
The air layer lasts 200 ms; the final mix is normalized to −16 dBFS. The script
checks the quantized WAV with Earshot's SFX audit and runs its local `describe`
and `match` feedback. The first feedback run downloads Earshot's pinned CLAP
and AST models. Omit `--feedback` for generation and the technical audit only.
The script writes [the feedback record](quick-retry-earshot.json).

## Earshot feedback

The selected cue receives a strong match for a soft digital reset click with
an airy tail (`z = 3.2`). Earshot's forced-choice comparison favors that
interpretation over a harsh click, cartoon pop, laser, or error buzz. Its general
description reads as a short tick/key-release sound. The AudioSet tagger is
uncertain on this very short clip; the model score does not establish a listener's
preference.

The quantized asset passes the technical audit with no clipping, discontinuity,
or tail warnings. The feedback record includes measured timing, levels, and all
alternative interpretations. The generator uses Earshot's MIT-licensed sound
bank; its notice is retained in `assets/legal/earshot.txt`.
