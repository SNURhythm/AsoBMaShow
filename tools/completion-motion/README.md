# Completion motion studies

Throwaway visual prototypes, not a native game feature. Three original directions
have Full Combo and Perfect variants. The reconstructed perspective playfield is
informed by the built-in gameplay fixture, not captured successful gameplay.

## Review

```sh
cd tools/completion-motion
npm ci
npm run review
```

Open `out/review.html`, or present its HTML using the host's supported inline
visual tools. It is self-contained and needs no server. It has direction/result
selectors, replay, pause, time scrubbing, speed, and reduced-motion controls.
It starts paused at a representative frame and does not loop automatically.
The simulated final judgement is at 0.65 seconds; the effect ends before 3.8s.
Sound design is intentionally not included in this visual comparison.

## Remotion

```sh
npm run dev
# Optional export, only when requested:
npx remotion render Impact-Perfect out/impact-perfect.mp4
```

All six 1280×720, 60fps compositions use `src/frame.mjs`, the exact same frame
model as the standalone reviewer. The SVG scene is one editing unit in Remotion;
edit its parameters in source rather than expecting per-particle Studio tracks.
The HUD reuses the repository's Roboto Condensed font. Achievement lettering uses
an ASCII subset of the bold Noto Sans CJK JP companion to the native judgement font.
The achievement has an independent upper-middle anchor at (640, 285) in the
1280×720 study, separate from judgement text and the judgement line. Entrance,
secondary release, readable hold, and exit are staged separately.
The palette and short ring/shard effects follow `JudgementColors.h` and
`KeybombEffect.h`. No slogans, decorative badges, or alternate display typeface
are added. The HUD counters are synthetic and do not define scoring policy.

`npm run lint` checks the source and types; `npm run build` checks the Remotion
bundle. `out/`, `build/`, dependencies, and rendered media remain untracked.
These tools are outside CMake and do not add runtime dependencies to the game.

## Handoff

After a direction is chosen, turn its frame model into a native implementation
spec following [the visual guide](../../docs/design/visual-implementation-guide.md).
The preview's color, geometry, timing, and layering are design evidence; its
browser performance is not a native mobile performance measurement. Immediate
last-note detection, ruleset classification, skin coordination, retries, and
replay timing still require native implementation and validation.

## Font subset

`src/fonts/judgement-ascii.ttf` and `judgement-bold-ascii.otf` are generated from
the repository's existing regular and bold Noto Sans CJK JP fonts; it avoids embedding the full CJK font in the
standalone reviewer. The existing [Noto font license](../../assets/legal/noto-sans-cjk.txt)
also applies to the subset. Regenerate with FontTools (`pyftsubset`):

```sh
pyftsubset ../../assets/fonts/notosanscjkjp.ttf --unicodes=U+0020-007E \
  --output-file=src/fonts/judgement-ascii.ttf --layout-features='*'
pyftsubset ../../assets/fonts/notosanscjkjp-bold.otf --unicodes=U+0020-007E \
  --output-file=src/fonts/judgement-bold-ascii.otf --layout-features='*'
```
