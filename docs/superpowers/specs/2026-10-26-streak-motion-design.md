# Motivational streak UI and result progress

## Intent

Overhaul the functional MVP into a playful, encouraging practice experience.
The user requested Duolingo-like motivation and animation, a result slide-over,
and a reusable animation framework. They explicitly chose automatic opening
once progress is saved, with a dismiss button. Keep user-facing explanations
short; this document is implementation context, not a presentation to the user.

## Visual approach

Use an original rhythm-spark character, rounded layered cards, confident
numerals, lime progress, warm orange celebration, blue freezes and lavender
milestones. Respect light/dark themes and responsive safe areas. Dashboard:
hero daily progress and streak, compact week/month progress, assignment cards
with meters, freeze/savings card, and collectible milestone badges. Complete
states emphasize satisfaction and a clear stopping point; optional bonus
challenges remain collapsed. Restyle routine/assignment editing as coherent
sections with generous controls and concise explanations.

## Approaches

Use native retained views and procedural vector drawing with reusable motion
values. This fits the renderer, supports all existing platforms and needs no
external animation runtime or binary assets. An external vector-animation
library would add build/package complexity before the native approach has
shown a limitation. Keep that option available if implementation requires it.

## Motion architecture

Add a small UI-independent, deterministic timeline/easing layer: finite tracks,
delays/stagger, smooth and spring-like easing, immediate/reduced-motion mode,
cancellation/settling, explicit time input. No global callback registry or raw
view references. Owners retain their tracks and cancel with their lifecycle.
Use it in reusable progress-meter/celebration drawing and the result drawer.
Drawing requests frames only while motion is active; idle screens settle.
Animations should not mutate state, block saving or delay navigation.

Build reusable native progress and spark views using the existing batched
geometry renderer. Animate progress fill, count emphasis, a short spark bounce,
and a bounded burst on completion. Keep hit targets aligned by animating actual
panel positioning during scene updates, not by applying unmatched transforms.

## Result slide-over

Replace the MVP result text block with an application-owned progress drawer.
For a newly played manual chart with a successful score save and an active
routine, open automatically after a short entrance delay. Include current daily
points/goal, streak, relevant assignment progress and an explicit dismiss
control. Preserve ordinary result controls; honor safe areas and resize, with a
compact side panel on wide screens and nearly full-width panel on phones.
Support selected result skins as an application overlay with correct event
priority. Escape/outside interaction should dismiss without also triggering
underlying actions. No panel for remote/saved-history results, autoplay,
practice, course records or failed/pending saves. A later successful persistence
retry may trigger it once. Rebuilding/resizing must not replay a dismissed
celebration. Do not invent per-attempt point gains: show persisted current-day
progress unless a genuine before/after receipt is available.

## Invariants and verification

Economy, daily reset, bonus limits and migration remain unchanged. Ordinary
routine edits still apply tomorrow. The user additionally requested immediate
changes when an assignment is unachievable (for example, too few eligible
charts). Add an explicit "Adjust today" action: today's levels, eligibility,
targets and quota may change; rewards, used charts, partial progress and earned
points are retained. Changes apply only today and never award points by
editing. A reduced quota needs another distinct qualifying chart. Preserve
scheduled routine changes and reject a stale editor after midnight.

The user also requested sound effects and suggested Earshot. Generate a small
matched set offline from its MIT-licensed procedural bank; retain generation
metadata and attribution. Play only on a fresh result drawer opening or a
successful freeze purchase, using the existing system bus/master volume.
No runtime dependency or sound on redraw/rebuild. Shared animation behavior gets deterministic CPU tests;
result eligibility/one-shot dismissal lifecycle gets focused behavioral tests.
Exercise the affected results/UI groups and compile the desktop app. Inspect
actual layouts or a faithful rendered preview for narrow/wide screens, themes,
complete/incomplete/empty states, and drawer dismissal. Use existing component
patterns and localized text. No deployments or new worktrees.
