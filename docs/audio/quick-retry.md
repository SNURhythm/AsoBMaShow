# Quick Retry cue

`assets/quick-retry.wav` is a glassy two-note interface chime generated with
[Earshot](https://github.com/Leonxlnx/earshot), commit
`591fa3e028032e6dce56620a20e932e35667e8d8` (0.4.0). It is 350 ms,
48 kHz stereo PCM16, with a −14 dBFS peak. The game preloads it once and applies
master volume at playback. Earshot is not a runtime dependency.

## Reproduce and review

Install the pinned Earshot checkout in a Python environment, then run:

```sh
python scripts/generate_quick_retry_sound.py --feedback
```

The generator renders `mallet/celesta` at D6 and A6, each rendered to 260 ms.
The second note begins 90 ms after the first and is rendered 1 dB quieter.
The combined cue is normalized to −14 dBFS. The script checks the quantized WAV
with Earshot's SFX audit and runs its local `describe` and `match` feedback.
The first feedback run downloads Earshot's pinned CLAP and AST models.
Omit `--feedback` for generation and the technical audit only.
The script writes [the feedback record](quick-retry-earshot.json).

## Earshot feedback

The selected cue receives a strong match for a delicate glassy two-note chime
(`z = 4.2`). Earshot describes it as a success/notification chime, with a clear
reading; the AudioSet classifier's leading label is Ding. Its alternative
comparison favors the requested description over an alarm, harsh buzzer,
cartoon boing, or metallic impact. This model feedback describes the take; it
does not establish a listener's preference.

The quantized asset passes the technical audit with no clipping, discontinuity,
or tail warnings. The feedback record includes measured timing, levels, and all
alternative interpretations. The generator uses Earshot's MIT-licensed sound
bank; its notice is retained in `assets/legal/earshot.txt`.
