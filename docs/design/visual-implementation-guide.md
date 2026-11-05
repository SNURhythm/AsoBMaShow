# Visual implementation guide

This is the persistent workflow for future UI, animation, and game-effect work.
Tool choices were researched on 2026-11-05. Recheck upstream documentation when
using an integration; installed versions and API availability can change.

## Intent and references

Aim for intentional, polished motion with a clear emotional target and readable
visual hierarchy. Ask for or use the current task's references rather than
assuming one permanent aesthetic. Session-specific references stay in the
session unless the user explicitly asks to persist them.

Use actual reference clips when matching motion, noting the relevant timestamp
and what to borrow. A still establishes composition, not choreography. Create
original geometry, lettering treatments, and sound; do not copy branding.

The game's native renderer is C++/SDL/bgfx. Use the current playfield, fonts,
palette, skin contracts, and device constraints as context. The synthetic built-in
7K screenshot at `tests/fixtures/gameplay_presentation/builtin_7k_16x9.png` is
useful for layout reference, but is not a successful-play recording.

## Tool decisions

| Need | Preferred tool | Boundary |
| --- | --- | --- |
| Motion direction, staging, timing, emotional intent | LottieFiles `motion-design` skill | Implementation-independent; does not require the Lottie runtime |
| Frame-driven animated concepts and rendered comparisons | Remotion with official `remotion-best-practices` skill | Authoring only; transfer chosen motion to native rendering or bake selected layers |
| Rings, trails, bursts, richer particle assets | Effekseer, when needed | Start with exported sprite sheets; live-engine integration needs a separate justification |
| Editable vector animation through an agent-controlled canvas | Lottie Creator MCP, when needed | Requires Creator plus MCP setup; export/playback in bgfx is a separate task |
| Figma-owned design or motion handoff | Installed Figma motion skills/tools | Discover feature availability first; motion APIs are account-gated |
| Static concept art or individual effect textures | Available image-generation tooling | Useful for art direction/assets; not evidence of temporal consistency |
| Shipping effect | Existing native primitives, batched sprites, and shaders | Preserve portability, skin behavior, and frame-time budget |

Remotion plus `motion-design` is the default starting pair, not a requirement to
introduce a Node project for every small visual edit. Reuse existing authoring
work where possible. Use one shared frame model for previews and renders to
avoid a polished mockup drifting away from the intended implementation.

Emil Kowalski's animation skills and Motion AI Kit can help web UI work, but
are not the default for arcade VFX. Rules such as sub-300ms transitions,
restrained scale, and removing frequent keyboard-triggered motion target web
interfaces. Do not apply them blindly to a rhythm-game achievement. Likewise,
adapt the LottieFiles skill's UI duration and choreography heuristics to the
effect's purpose instead of treating them as physical laws.

## Skill setup and availability

Installed on this machine on 2026-11-05 as user-level Codex skills:

- `motion-design` from `LottieFiles/motion-design-skill`, path
  `skills/motion-design`.
- `remotion-best-practices` from `remotion-dev/skills`, path
  `skills/remotion-best-practices` (includes its referenced guides).

They are external tools, not vendored repository files. On another machine,
discover them through the agent's skill catalog and read their `SKILL.md` before
use. If missing, restore using the available skill installer or upstream CLI:

```sh
npx skills add LottieFiles/motion-design-skill --skill motion-design
npx skills add remotion-dev/skills --skill remotion-best-practices
```

Choose the appropriate agent and installation scope. Avoid silently writing
machine-specific skill copies into this repository. Installation does not install
Remotion itself, Effekseer, or the Creator MCP. Follow the selected tool's setup
instructions only when needed. If unavailable, state that and use a deliberate
fallback (for example, a self-contained frame-driven SVG/Canvas prototype).
Do not claim a skill/tool was used merely because this guide recommends it.

## From request to native effect

1. Inspect the existing view and event lifecycle. Capture or reuse representative
   gameplay; label reconstructed backgrounds and synthetic counters honestly.
2. Load the relevant skills. Establish the emotional target, primary visual,
   secondary motion, color hierarchy, timing, and how the effect ends.
3. For a new direction, create two or three genuinely different moving options.
   Vary motion and silhouette, not just hue. Show them in gameplay context with
   replay, pause/scrub, and normal/slow playback where practical. Keep prototypes
   clearly separated from production code and label them as prototypes.
4. Review the impact, reveal, readable hold, and exit both in motion and as sampled
   frames. Check narrow screens, reduced motion, clipping, legibility, and visual
   competition with notes, judgement information, and the BGA. A screenshot alone
   cannot establish motion quality. Use the host's supported visual presentation
   tools; a file path alone does not show the animation.
5. Once a direction is chosen, record a compact handoff: event/time origin,
   duration, keyframes/easings, coordinate space, layers/blend modes, textures,
   lifecycle/reset behavior, reduced-motion version, and device budget. Do not
   restart approval for an already-selected direction or routine refinements.
6. Port to native rendering. Keep text and simple geometry procedural where
   useful; bake expensive decorative layers when justified. Preload resources,
   bound particle counts/atlas memory, and avoid per-frame allocations. Browser
   glow/blur is not proof of bgfx or mobile performance.
7. Verify the native effect on relevant platforms and layouts. Use focused
   behavioral tests for event semantics and visual capture for appearance; do
   not use a full application rebuild for a documentation-only change.

Audio may strengthen the effect, but is a separate authored layer: evaluate
against the song ending and respect volume/mute behavior. Do not add arbitrary
sound or hide weak visuals behind it.

## Completion-effect semantics

This section is a handoff constraint, not a finished implementation spec.

- Start from authoritative judgement resolution, including the relevant long-note
  tail/release. Do not equate the last timestamp, last input, empty visible lane,
  end of audio, or result-screen transition with the last handled note.
- `GameplaySimulation::maybeLatchChartComplete` currently also waits for the
  final timeline plus a late-judgement margin. Its terminal event therefore needs
  investigation before use for immediate last-note feedback.
- Reuse existing clear/ruleset policy. Establish exactly which result earns
  Perfect versus Full Combo; do not invent another scoring definition or change
  persisted scores to match the animation. Check assist and playback modifiers,
  empty charts, practice ranges, course stages, autoplay, and replay behavior.
- A celebration at the last resolved note and the final persisted clear are
  distinct concerns. Existing policy notes that later empty POOR can affect the
  surviving combo in some modes. Decide presentation behavior explicitly without
  changing that policy.
- Emit once per eligible attempt/stage, with deterministic time. Reset on retry,
  seek, restart, and scene exit; avoid replay/export timing drift. Inspect existing
  custom-skin completion timers so native and authored celebrations do not double.
- Perfect should have a distinct culmination or choreography, not only a recolor.
  A reduced-motion treatment should keep the achievement legible while removing
  aggressive movement, repeated flashes, and screen shake.

## Sources

- [LottieFiles motion-design skill](https://github.com/LottieFiles/motion-design-skill)
- [Remotion agent skills](https://www.remotion.dev/docs/ai/skills)
- [Remotion transparent rendering](https://www.remotion.dev/docs/transparent-videos)
- [Effekseer introduction and export capabilities](https://effekseer.github.io/en/introduction.html)
- [Lottie Creator MCP](https://docs.lottiefiles.com/en/creator/13_ai-tools/lottie-creator-mcp)
- [Motion AI Kit](https://motion.dev/docs/ai-kit)
- [Emil Kowalski animation skill](https://github.com/emilkowalski/skills/blob/main/skills/animate/SKILL.md)
