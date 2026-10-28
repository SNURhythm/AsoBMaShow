# Combo sounds

Synthesized locally with [Earshot](https://github.com/Leonxlnx/earshot), commit
`591fa3e028032e6dce56620a20e932e35667e8d8` (MIT; license included).
The app plays bundled WAVs; it needs no models, network service, or synthesis code.

| File | Design | Duration | Peak |
| --- | --- | --- | --- |
| progress.wav | Short air swish | 0.267 s | −14 dBFS |
| complete.wav | Soft fire whoosh with granular crackle | 0.606 s | −11 dBFS |
| freeze.wav | Crystalline shimmer | 0.750 s | −13 dBFS |
| round-advance.wav | Three rising mallet notes | 0.579 s | −17 dBFS |

## Selection and verification

Screened 79 clean bank candidates, compared them with the previous four cues,
then rendered and queried 14 shorter, softer variants. Earshot's local CLAP
semantic comparisons informed the selection, rather than relying on filenames.
Selected-vs-previous scores for the same queries:

| Cue | Query | Selected z | Previous z |
| --- | --- | --- | --- |
| Progress | a quick smooth soft air whoosh | +3.56 | +2.36 |
| Daily goal | a short soft fire whoosh with gentle crackling | +3.68 | −1.08 |
| Round | a short ascending two note electronic chime | +4.44 | +3.84 |
| Freeze | a delicate crystalline glass chime with a sparkling tail | +2.65 | +1.94 |

These are semantic similarity z-scores, not probabilities or human listening
judgments. The round query shortlisted the three-note version; its second
query (bright playful rising chime) also scored above the old cue (+2.85 vs
+2.50). The freeze choice favors a sparkling texture: the old bell still scores
higher for “short clear icy bell” (+2.85 vs +2.57). Ignition also matches “small
flame igniting with a soft puff” at +2.90; explosion and gunshot alternatives
score +1.82 and +0.57. It is a stylized UI effect, not a fire recording.

The round chime is attenuated a further 5 dB after semantic selection to keep
its sustained notes close to the ignition and freeze loudness.

All four pass Earshot's artifact audit for clicks, clipping, DC, and cut tails.
`generation.json` records deterministic seeds, layer parameters, measurements,
and query results. With the recorded Earshot revision and its dependencies:

```sh
python scripts/generate_combo_sounds.py
# Verify without overwriting assets:
python scripts/generate_combo_sounds.py --output /tmp/combo-sounds
```

Playback uses the application's system audio bus and master volume. Goal
ignition starts once when the daily target is first reached; later bonus plays
use the progress cue. Round promotion waits for the completed-round hold before
playing its chime. Successful freeze purchase plays the shimmer. Simultaneous
promotions play one cue; dismissing the drawer or resuming after the animation
has finished does not play a late cue. Rendering and layout rebuilds never
trigger audio.

## Goal achievement

`goal-achieved.wav` is a separate trumpet-style major-triad fanfare for personal
goals. Reproduce it with `scripts/generate_goal_sound.py` using the Earshot
revision above. It combines deterministic harmonic brass synthesis, Earshot room
DSP, and a quiet `swish/stick` bank layer. It is synthesized, not a recorded
trumpet. The bundled mono PCM cue peaks at -12 dBFS and remains below the
256 KiB loader limit. `goal-achieved-review.json` records Earshot artifact and
semantic feedback. The multi-note tail-rise observation is intentional.
