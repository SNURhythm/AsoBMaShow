# Completion sound prototypes

Three audition sounds made with [Earshot](https://github.com/Leonxlnx/earshot),
pinned to `591fa3e028032e6dce56620a20e932e35667e8d8` (0.4.0).
These are authoring experiments, not shipped assets or gameplay audio wiring.
The standalone listening page pairs them with the existing approved Rise,
Wave and MAX Focus frame models from `../completion-motion/src`.

## Direction

Apply the motion-design skill's primary, secondary and ambient hierarchy:
a clear main strike, supporting air or sparkle, then a diminishing spatial tail.
An open E/B harmonic palette links the three rewards without a long melody.
The final judgement is time zero in each standalone WAV.

| Outcome | Visual cue | Sound | File / audible end | Sample peak |
| --- | --- | --- | --- | --- |
| Full Combo | 80 ms rise, staggered letters, lime trails | Short upward celesta gesture and a light air sweep | 1.45 s / 1.17 s | -9 dBFS |
| Perfect | One gold pulse at 80 ms | One simultaneous crystal chord, small low body, soft space | 1.95 s / 1.68 s | -7.5 dBFS |
| MAX | Gather until the 140 ms burst, white/teal/gold star and shards | Fuller open chord and low impact with a dispersed sparkle tail | 2.35 s / 1.93 s | -6 dBFS |

Perfect has no second strike. MAX gains weight, width and decay as well as
level; Full Combo stays lighter and shorter. The title holds after each sound
has decayed. No FINISH sound, voice, song ducking or runtime changes are included.
The existing prototype fonts and geometry are reused without changing them.

The first pass is a solo audition. Its musical fit under actual chart endings,
phone-speaker translation and preferred reward intensity still need listening.
Earshot's measurements are artifact checks, not a subjective listening verdict.

## Reproduce

Use a separate Earshot checkout and authoring environment. Do not add Earshot,
Python or its DSP code to the native runtime. The bank and DSP route needs no
model downloads, API keys, external audio upload or licensed reference recordings.
Only procedurally synthesized bank entries and Earshot's bell/reverb functions
are used. The Earshot MIT notice is retained in `EARSHOT-LICENSE.txt`; the existing
Noto/Roboto font licenses in `assets/legal` cover the preview font subsets.

```sh
# Choose locations outside the app's tracked source tree.
git clone https://github.com/Leonxlnx/earshot.git /tmp/asobmashow-earshot
git -C /tmp/asobmashow-earshot checkout 591fa3e028032e6dce56620a20e932e35667e8d8
python3.12 -m venv /tmp/asobmashow-earshot-venv
/tmp/asobmashow-earshot-venv/bin/pip install -r tools/completion-audio/requirements.txt
PYTHONPATH=/tmp/asobmashow-earshot /tmp/asobmashow-earshot-venv/bin/python tools/completion-audio/render.py
PYFTSUBSET=/tmp/asobmashow-earshot-venv/bin/pyftsubset node tools/completion-audio/make-review.mjs
```

FFmpeg and Node must be on `PATH`. `out/review.html` is self-contained: embedded
audio, fonts, controls and animation code. Open it directly; no server is needed.
It uses the audio playback clock for the animation, including a 650 ms lead-in.
Choose an outcome, use the native audio controls, or play all three in sequence.
The WAV files have no preview lead-in. They are 48 kHz stereo PCM, 24-bit.
M4A copies are only for convenient browser playback.

`out/report.json` records the bank version, seeds, knobs, source audits, final
measurements and WAV hashes. All three first-pass mixes and their bank sources
passed Earshot's artifact audit (no clipping, discontinuity clicks, DC, cut tails,
comb-ring or rising-glide warnings). Audible-end times above are Earshot's
relative -50 dB envelope boundary, not file trims. No listening-model score was
used or claimed. `node --check make-review.mjs` passed and the self-contained
preview rendered with no console errors.

Generated WAV/M4A files, WOFF2 subsets and reports remain under ignored `out/`.
Keep the selected assets only when the listening direction is approved.
