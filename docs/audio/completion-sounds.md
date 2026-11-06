# Completion sounds

The built-in Full Combo, Perfect and MAX celebrations use the approved harp
and short-clap cues in `assets/audio/completion/`. Their durations are 1.35,
1.65 and 1.95 seconds. MAX keeps its upper register through the final chord,
ending on F-sharp 6, the same highest note as Perfect. These WAVs are identical
to the approved `earshot-max-high-finish` audition (without its preview lead-in).

The scene preloads the 48 kHz stereo PCM24 assets and plays the matching cue
once when its built-in animation starts, at the current master volume. The
sound's 80 ms Full Combo/Perfect accent and 140 ms MAX accent align with the
animation. Finish and practice do not play these cues. Authored skin playback
keeps its own sounds. Retry resets the trigger; scene exit stops and releases
the loaded sounds. Returning to an already elapsed celebration does not start
a delayed cue.

All source audio is procedural synthesis using
[Earshot](https://github.com/Leonxlnx/earshot), commit
`591fa3e028032e6dce56620a20e932e35667e8d8`. There are no recorded samples or vocal
layers. Earshot is an authoring dependency only; its MIT notice is retained in
`assets/legal/earshot.txt`. The level target is −17 LUFS maximum momentary with
a −4 dBFS sample-peak ceiling.

With the pinned Earshot checkout, NumPy, SciPy and SoundFile installed, run:

```sh
python scripts/generate_completion_sounds.py --output /tmp/completion-sounds
```

The generator prints the score, measurements and SHA-256 hashes. The shipped
assets and a fresh render were compared byte-for-byte with the approved masters;
the hashes and format details are recorded in `completion-sounds.json`.
