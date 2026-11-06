# Completion sound authoring archive

The approved harp cues are integrated into gameplay. The maintained,
model-free generator is [`../../scripts/generate_completion_sounds.py`](../../scripts/generate_completion_sounds.py);
see [the shipped sound documentation](../../docs/audio/completion-sounds.md)
for reproduction, licensing and the verified asset hashes.

This directory preserves the audition scripts, briefs and design history that
led to those sounds. `earshot_celebration.py` and `make-earshot-review.mjs` are
the final model-feedback audition route; the other generators and players
are earlier experiments. Generated audio, downloaded reference recordings,
model feedback, HTML players and virtual environments remain local in ignored
`out/` or environment directories. Historical A/B players require the retained
revision files described below and are not standalone clean-checkout builds.

The notes below describe each revision **at the time it was auditioned**.
Their prototype-only/no-push instructions are historical and were superseded
by approval and native integration. Earlier CC0 recording experiments are not
part of the shipped procedural sounds.

## Audition history

Local audition work used [Earshot](https://github.com/Leonxlnx/earshot), pinned
to `591fa3e028032e6dce56620a20e932e35667e8d8` (0.4.0). The user rejected the first
take as generic and requested model-backed feedback and a richer description.
See [the revised sound brief](BRIEF-v2.md).

The audition reuses the approved Rise, Wave and MAX Focus frame models from
`../completion-motion/src`. Sound accents remain tied to the final judgement:
80 ms for Full Combo / Perfect, 140 ms for MAX. `out/review.html` embeds all
media and animation code; it needs no server. A/B controls compare the current DJ scratch gestures and
the previous contoured melodies with drums at matched maximum momentary loudness. No song bed is included.

## Feedback workflow

1. `render.py` reproduces the rejected first takes, retained for comparison.
2. `refine.py` makes a small set of glass, alloy and FM material candidates and
   evaluates them with Earshot's full `Critic` plus calibrated `describe`.
3. `kinetic.py` synthesizes a contact-excited material, short moving air, granular
   detail and a diffuse tail. `render_material.py` renders its alternatives.
   These alone were too drum-like according to the independent audio tagger.
4. `search_hybrid.py` uses Earshot **Forge's actual render / critic / mutate
   loop**, with the acoustic brief and named confusion prompts. It searches
   16 candidates per outcome and tests a shortlist on two additional seeds.
   Independent AudioSet descriptions are then collected for the retained takes.
5. `finish_revision.py` keeps the first robust pick per outcome, damps the
   specifically flagged ringing band after the initial contact, sets listening
   levels, re-runs the full critic and independent descriptions, and prepares
   matched A/B media. It retains full feedback in `out/revision-feedback.json`.
6. `make-review.mjs` creates the synchronized audition page.

Model feedback is not an aesthetic verdict. The early pure-material variants
scored well against a prompt while sounding like snare hits to the unrestricted
classifier, so they were rejected. The selected hybrid still receives some
chime/triangle/notification labels; retain that evidence rather than claiming
the models certify sophistication. Human listening decides the direction.

## Reproduce

Use a separate authoring environment. Earshot, Python, models and DSP code do
not belong in the native runtime. Python 3.12, FFmpeg and Node are required.
The model-backed route downloads Earshot's pinned CLAP and AST models once
(about 1.1 GB); all inference then runs locally. No audio is uploaded.

```sh
python3.12 -m venv /tmp/asobmashow-earshot-venv
/tmp/asobmashow-earshot-venv/bin/pip install -r tools/completion-audio/requirements-feedback.txt
export EARSHOT_HOME="$HOME/.cache/earshot"
/tmp/asobmashow-earshot-venv/bin/earshot doctor --fetch
/tmp/asobmashow-earshot-venv/bin/python tools/completion-audio/render.py
mkdir -p tools/completion-audio/out/v1
cp tools/completion-audio/out/full-combo.wav tools/completion-audio/out/perfect.wav tools/completion-audio/out/max.wav tools/completion-audio/out/v1/
/tmp/asobmashow-earshot-venv/bin/python tools/completion-audio/refine.py both
/tmp/asobmashow-earshot-venv/bin/python tools/completion-audio/render_material.py
/tmp/asobmashow-earshot-venv/bin/python tools/completion-audio/refine.py listen --round r3
/tmp/asobmashow-earshot-venv/bin/python tools/completion-audio/search_hybrid.py
/tmp/asobmashow-earshot-venv/bin/python tools/completion-audio/finish_revision.py
PYFTSUBSET=/tmp/asobmashow-earshot-venv/bin/pyftsubset node tools/completion-audio/make-review.mjs
```

The master WAVs are stereo 48 kHz / 24-bit PCM and start at judgement time.
The audition copies add a 650 ms visual lead-in and a quiet end hold. The
animation follows the audio player's clock. M4A copies are for browser playback.

`out/r2` and `out/r3` retain every candidate's parameters, seed, audit and full
listening feedback. `out/r4` retains Forge search reports, robustness checks and
independent descriptions. `out/revision-feedback.json` records the selected
sources, corrective processing, finished measurements, model feedback and hashes.
The generated media and reports remain under ignored `out/` pending selection.

The synthesis uses Earshot code, not recorded samples. Its MIT notice is in
`EARSHOT-LICENSE.txt`. Existing Noto/Roboto licenses in `assets/legal` cover the
preview's font subsets. No new native runtime dependency is introduced.

## Melodic revision

The user found the textured impacts too tough and requested melodies.
`melodies.py` uses a soft celesta-like Earshot mallet voice, a shared rising
G–C–E motif, gentle major harmony, and short room reflections. Full Combo
resolves after five notes; Perfect expands to seven; MAX ends its eight-note
phrase on a high C. Masters are 1.65 / 2.0 / 2.4 seconds.

Before running it, preserve the previous version for the A/B player:

```sh
mkdir -p tools/completion-audio/out/v2
cp tools/completion-audio/out/full-combo.wav tools/completion-audio/out/perfect.wav tools/completion-audio/out/max.wav tools/completion-audio/out/v2/
EARSHOT_HOME="$HOME/.cache/earshot" /tmp/asobmashow-earshot-venv/bin/python tools/completion-audio/melodies.py
PYFTSUBSET=/tmp/asobmashow-earshot-venv/bin/pyftsubset node tools/completion-audio/make-review.mjs
```

`out/melody-feedback.json` retains Earshot match and independent description
results. All three read primarily as success chimes; Full Combo still strongly
associates with notification audio in the forced-choice comparison. These
models do not validate musical quality. Listen to the prototypes before
selecting or integrating them.

## Fast ascending harmony revision

The user requested an ascending melody with fast-paced harmonies.
`ascending.py` uses sixteenths at 180 BPM (83.3 ms), ascending lead notes,
short overlapping gates and changing chord voicings underneath. The lead
pauses for one subdivision before its final high note. Full Combo uses six
notes, Perfect eight, and MAX ten. Warm plucked harmony contrasts with the
brighter rounded celesta lead. No percussion or impact layer is added.

Preserve the previous melodic masters in `out/v3-melody/` before rendering.
Run `ascending.py` in the same Earshot environment, followed by
`make-review.mjs`. The A/B player now compares this revision with those slower
melodies. `out/ascending-feedback.json` retains the exact scores, tempo,
measurements, artifact audit, full Earshot critic feedback, independent
descriptions and master hashes. This is still a local audition, awaiting
listening feedback; there is no native integration or push.

Earshot's first pass flagged upper partials around 4.15–5.23 kHz. The final
render applies a 3.6 kHz low-pass filter to soften those partials while keeping
the highest lead fundamental at 1.57 kHz. Its single-impact audit also flags
the deliberate ascending pitches and repeated attacks; these warnings are
retained rather than treating them as reasons to remove the requested melody.
The first-pass report is preserved separately for comparison.

## Short phrases with percussion

The user requested shorter melodies and non-melodic instruments such as kicks.
`compact.py` reduces Full Combo / Perfect / MAX to 4 / 5 / 6 notes at 200 BPM.
First-to-last-note spans are 300 / 375 / 450 ms. Shorter final-note gates and
decays bring the entire masters to 0.94 / 1.015 / 1.15 seconds.

Each has a centered electronic kick on its first and final notes. A fast pitch
fall and short body envelope keep it drum-like. Quiet high-passed noise hats
mark alternating subdivisions. Percussion stays dry and bypasses the melodic
low-pass/reverb. The melodic harmony remains the ascending revision's palette.

Preserve the previous masters in `out/v4-ascending/`, run `compact.py` in the
Earshot environment, then run `make-review.mjs`. `out/compact-feedback.json`
retains note/chord scores, percussion timings, technical checks, full critic
feedback, independent descriptions and master hashes. The comparison is with
the previous longer ascending version, matched in maximum momentary loudness.
These remain local prototypes; do not push before actual game integration.

## Rising contour with downward turns

The user found an exclusively rising line boring. The latest `compact.py`
keeps the short timing, chord changes and percussion, but gives all three
phrases a shared C5–G5–E5 opening: a leap followed by a downward third.
Full Combo then lands on C6; Perfect ends D6–E6; MAX adds D6–B5–G6 for
a second downward turn before its highest note. The line rises overall
without every successive note ascending.

The previous strict-rise masters and script are preserved in
`out/v5-short-drums/`. The player compares against these at matched loudness.
The current `out/compact-feedback.json` identifies the revision as
`contoured-percussion-local` and includes the new exact scores and checks.

## DJ scratch direction

The user abandoned melody and proposed a DJ direction based on beatmania.
`dj.py` replaces pitched notes/chords with a synthetic record texture played
forward and backward at changing speeds. Its source is a fixed, nonverbal
formant synth/noise mixture; no record sample, speech or copyrighted game audio
is used. Alternating velocity retraces the same waveform. Rounded crossfader
gates create rhythmic cuts without hard sample discontinuities.

Full Combo has two strokes; Perfect four with two fader cuts; MAX six with
six fader cuts. A quieter dry kick/snare accent lands after each gesture.
The first feedback pass found the drums too dominant, particularly in Full
Combo, so the final mix raises the scratch relative to the drums and slightly
extends Full Combo's two strokes. These are sound-design prototypes, not
recordings of a human turntablist.

Preserve the prior masters in `out/v6-contour/`, run `dj.py` in the Earshot
environment, then run `make-review.mjs`. `out/dj-feedback.json` retains the
parameters, measurements, full critic, targeted scratch comparison and
independent descriptions. `out/dj-first-feedback.json` preserves the earlier
mix's feedback. The deliberate fader silence and gap before the final beat
can trigger single-hit audit warnings; keep that evidence. Human listening
remains the selection criterion. No native integration or push yet.

## Smoother DJ palette studies

The user rejected the scratch direction as too scratchy. `smooth_dj.py` makes
two 0.94-second alternatives on MAX before extending another full reward set:
A, rounded kicks and muted toms with a gently opening low-pass filter;
B, a soft drum accent and rounded kick followed by two dark echoes.
Neither contains record scratching or a melodic run. They are matched in maximum momentary loudness at the quieter level allowed
by a -18 LUFS target and -5 dBFS peak ceiling for both tracks.

Run `smooth_dj.py` in the Earshot environment, then `make-smooth-review.mjs`.
`out/smooth-review.html` embeds the two audio tracks and the same MAX animation.
`out/smooth-dj-feedback.json` preserves the briefs, critic/description results,
audit, measurements and hashes. Existing reward WAVs remain the previous
scratch prototype; these new files use distinct names pending selection.

The initial low-register chord read partly as an error buzz in feedback. The
revised version uses an open C4–G4–B♭4–E♭5 voicing, softer upper harmonics,
and quieter kicks. The first feedback report remains alongside the current
report. Model labels are diagnostic hints, not a verdict on whether either
palette fits the game.

The revised chord also retained an alert-like classification, so it was
dropped from the offered pair. The second candidate is now `echo-out`: a
rounded kick and soft filtered snare/clap accent with two darker offbeat
echoes. Both offered directions are percussion-only. The discarded chord
WAV remains local for traceability and is not included in the audition.

## Celebration reset using external sound material

The user rejected both smooth percussion studies because they lacked a
celebratory payoff. `celebration.py` uses a Zane Little Music fanfare ending and a recorded
crowd cheer to build one 1.30-second victory accent, with an optional crowd
layer. See [source credits](SOURCES-celebration.md) for exact assets, licenses
and processing. This intentionally changes source material after repeated
unsatisfactory hand-built synthesis attempts.

The player `make-celebration-review.mjs` compares the same hit with/without
the crowd, matched by maximum momentary loudness. Inputs live under ignored
`out/source-review/`; `out/celebration-feedback.json` records hashes and all
model feedback. Neither model classification nor lack of clipping establishes
that the user will find it celebratory. It remains a local listening study.

The Kenney isolated-hit experiment still read like an alert and was replaced
with the 3.60–4.70 s ending from Zane Little Music’s CC0 Hyper-Ultra-Fanfare.
The source’s composed resolution and instrumentation remain; only the ending
is used. No extra synthesized kick or new melodic sequence is added.

## Accepted direction: melodic pickup, crowd roar and longer applause

The user accepted the sampled celebration direction, requesting a small
melodic revision, a roar and longer crowd claps. The current `celebration.py`
replaces the reversed swell with the source fanfare’s preceding melodic pickup
(3.42–3.60 s), compressed to 140 ms with pitch preserved. The accepted final
accent still begins at 140 ms. A stronger recorded cheer starts at 180 ms,
and a separate continuous applause excerpt starts at 260 ms and fades out
over its last 750 ms. The full revised master lasts 2.90 seconds.

The accepted 1.30-second version and its script are preserved unchanged in
`out/v7-celebration/`. The current player compares it with the revised full
mix at matched maximum momentary loudness. `victory-hit.wav` is also exported
as a music-only reference. `celebration-feedback.json` retains exact source
edit times, technical checks and model feedback. Still no game integration
or push.

## Earshot-only reset: short claps and synthesized “waaaah”

The user explicitly requested Earshot instead of licensed audio sources.
`earshot_celebration.py` is independent of all downloaded recordings and of the
sampled fanfare. It synthesizes an original short major flourish, open-vowel
voices, a rounded kick and a brief 660 ms cluster of claps with Earshot DSP.
No external audio file is loaded. Total mix duration is 1.75 seconds.

The vocal is an approximation from harmonic excitation, moving formants,
independent pitch/onset variation and computed room reflections. Earshot here
is a procedural synthesis and feedback toolkit, not a natural human-voice
generator. Early iterations were interpreted as noise/engines/booing, so the
model reports must not be presented as proof of a realistic crowd. The page
includes the vocal alone for direct listening judgement.

Run `earshot_celebration.py` in the existing environment, followed by
`make-earshot-review.mjs`. The resulting `out/earshot-review.html` contains
only the original synthesis. `out/earshot-celebration-feedback.json` records
all candidate prompts/scores, selected parameters, source method, empty
external-audio-input inventory, timings, audits and output hashes. First and
second feedback passes remain in separate files. Previously downloaded media
remain historical local experiments; none are included in this audition or
its deliverable archive. Nothing is integrated or pushed.

## Earshot reward family without vocals

The user removed the “waaaah” layer and requested Full Combo and Perfect
variants. `earshot_celebration.py` now renders `earshot-full-combo`,
`earshot-perfect` and `earshot-max`. MAX retains the previous six-note music,
kick and 660 ms clap burst unchanged before mastering; the vocal is omitted.
Full Combo uses a compact four-note phrase with lighter harmony; Perfect uses
five notes and the full major voicing. Their accents land at 80 ms, while
MAX retains its 140 ms accent. Durations are 1.35, 1.55 and 1.75 seconds.

All three use the same Earshot instruments and brief claps, with no external
audio inputs. The audition level target is -17 LUFS maximum momentary with
a -4 dBFS sample-peak ceiling. The generator records the exact score, technical
audit, full Earshot critic, independent description and hashes in
`out/earshot-celebration-feedback.json`. The prior vocal version is preserved
under `out/v9-earshot-waah/` for traceability and excluded from this audition.

Run the same generator and `make-earshot-review.mjs` commands. The current
`out/earshot-review.html` offers all three outcomes with their matching
animations and a Play all three control. `out/completion-earshot-family.zip`
contains only these new masters, the self-contained audition, feedback and
authoring/license notes. These remain local prototypes, pending integration.

## Distinct Full Combo and Perfect instruments

The user requested completely different sounds for the variants. Full Combo
now uses three gated brass-style chord stabs (C major, G major, C major),
plucked bass and a quiet kick. Perfect uses a quick harp-like string cascade
in D major over a soft sustained major-six-nine chord, without the brass or
kick. The instruments, harmony, rhythm and decay differ from MAX's synth lead.
Full Combo lasts 1.35 seconds and Perfect 1.65 seconds; both keep the 80 ms
main onset and brief clap layer. MAX's 1.75-second master remains unchanged.

The new string instruments use Earshot's computed `mallet` pluck recipe; brass
and sustained harmony use procedural additive synthesis and Earshot DSP.
No recordings are loaded. `out/v10-earshot-family/` preserves the prior
variants. Run the existing generator and player commands to reproduce the
current audition. `out/completion-earshot-distinct.zip` packages the three
masters and updated audition/feedback; all work remains local.

## Harp palette across all outcomes

Before native integration, the user selected Perfect's harp-like instrument
and requested another prototype using it for the other outcomes. Perfect's
master stays byte-identical to the preceding audition. Full Combo replaces
brass and kicks with three short, higher-register harp strums (C, G, C), light
plucked bass and quiet sustained harmony. MAX keeps its six-note lead contour
and timing, now on the same harp voice, with two soft upper-octave answers and
a fuller F-sharp-major chord. All three retain the short clap burst.

The previous instruments are preserved in `out/v11-distinct-instruments/`.
`out/completion-earshot-harp.zip` contains the current masters, audition and
feedback. The native integration was stopped before any gameplay code or
assets changed. This remains a local prototype; do not commit or push it.

## Expanded MAX harp flourish

The user requested more flourish on MAX. Its early pickup and 140 ms accent
remain, followed by a quicker rising sweep, a falling turn and a rolled high
F-sharp-major chord at 630–654 ms. Faster passing notes use shorter damping
to keep the run clear; the closing chord rings longer over a second soft
harmonic bloom. Total duration is 1.95 seconds. The clap burst remains 660 ms.
Full Combo and Perfect remain byte-identical to the preceding harp audition.

`out/v12-harp-family/` preserves that preceding version. The current package
is `out/completion-earshot-max-flourish.zip`. The player starts on MAX and
still includes all three outcomes. No native integration, commit or push.

## Lower MAX harp register

The user found MAX's pitch too high. Its lead is now one octave lower
(C-sharp 4 through F-sharp 5); the final rolled chord is voiced within
F-sharp 4 through F-sharp 5. The underlying harmony, phrase timing, string
instrument, clap layer and 1.95-second duration remain. Full Combo and
Perfect remain byte-identical. `out/v13-max-flourish/` preserves the high
version; `out/completion-earshot-max-lower.zip` is the new local audition.

## Stronger celebratory resolution for MAX

The user found Perfect more celebratory than MAX. MAX now builds through
changing harmony (IV, suspended V, tonic six-nine) into a fuller six-string
rolled chord at 560–600 ms. The pickup is quieter, the closing strings ring
longer, and the same short clap burst lands on the resolution at 3 dB more
relative gain. The highest lead/chord note remains F-sharp 5, and the full
cue remains 1.95 seconds. Full Combo and Perfect stay byte-identical.

The earlier low-register version is preserved in `out/v14-max-lower/`.
`out/completion-earshot-max-cadence.zip` packages the new audition. Its
Perfect → MAX control plays the two at matched maximum momentary loudness
for human comparison. This remains local, without native integration or push.

## MAX peak matched to Perfect

The user requested a higher maximum pitch matching Perfect. MAX now briefly
reaches F-sharp 6 (1479.978 Hz), exactly the highest note in Perfect. Its three
opening notes and supporting harmony keep their warm register. The upper run
spans farther, and the rolled chord ends at C-sharp 6 (1108.73 Hz), below the
brief peak. Rhythm, gains, cadence, short claps and 1.95-second length remain.
Full Combo and Perfect are unchanged.

`out/v15-max-cadence/` preserves the previous version. The new package is
`out/completion-earshot-max-matched-pitch.zip`, with the same Perfect → MAX
comparison control. This remains a local prototype, without integration or push.

## MAX stays high through the finish

The user asked MAX not to settle after the peak. Its second phrase now stays
between C-sharp 6 and F-sharp 6, rises back to F-sharp 6 and ends the rolled
chord on that same high note. The ceiling still matches Perfect (1479.978 Hz).
The warm opening, underlying harmony, timings, clap burst and 1.95-second
length remain. Full Combo and Perfect remain byte-identical.

`out/v16-max-matched-pitch/` preserves the preceding version. The new local
audition package is `out/completion-earshot-max-high-finish.zip`.
