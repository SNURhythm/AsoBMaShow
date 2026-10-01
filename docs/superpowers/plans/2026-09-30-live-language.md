# Live Language Selection Implementation Plan

> **For agentic workers:** Execute the independent UI and scene migration tasks in parallel; root owns integration, builds, and commits.

**Goal:** Apply language selection immediately and refresh retained screens without resetting user or operation state.

**Architecture:** Retain explicit message IDs and owned formatting arguments in UI labels. Refresh bound presentation in place when the language revision changes; do not reinitialize scenes or reverse-match translated strings. Recompute structured dynamic presentation on retained selectors where it is cached.

**Tech Stack:** C++20/23, SDL2, Yoga, existing TextView/DropdownView and scene lifecycle.

**Spec:** User-approved conversation design: immediate current-screen refresh and retained-screen refresh, preserving scroll positions, unsaved input, playback and background tasks.

## Constraints

- Keep existing English/Korean/Japanese IDs and raw chart metadata, typed input, device names, and third-party skin text distinct.
- No worktrees or deployment; preserve unrelated changes; commit and push verified work to the current upstream.
- Root alone runs builds and CTest to avoid concurrent Ninja writes.
- Historical log text remains recorded data; do not restart work to regenerate messages.

## Tasks

- [x] Core localization: add `i18n::Text` (raw literal or explicit message with owned nested named arguments), `i18n::message`, and language revision. Test contextual IDs, argument lifetimes, and raw text identity.
- [x] UI bindings: add `TextView::setLocalizedText`, clear bindings on ordinary `setText`, propagate language changes through View, Button content, and portal overlays. Carry descriptors in dropdown/context-menu labels without rebuilding open menus. Test current text, geometry, selection, and state retention.
- [x] Settings: migrate presentation helpers and static labels to descriptors. On successful preference save, select the language immediately; failures retain the previous preference/language. Refresh structured presentation without reinitializing sessions. Update the restart notice and documentation.
- [x] Retained selectors: migrate MainMenu/native MusicSelect presentation and owned widgets to explicit bindings; invalidate language-sensitive presentation caches without clearing jobs, filters, text input, selection, or scroll.
- [x] Scene lifecycle: refresh current/retained initialized scenes at safe event/frame boundaries. Verify a retained scene refreshes without init/cleanup calls and without stopping work.
- [x] Review final diff, build the desktop app and tests, run relevant regressions and full CTest, then commit/push and update PR #110.

## Review Focus

- Two contexts with identical English display text must keep independent translations.
- User content equal to a catalog value must remain raw.
- Language refresh must not delete views while their event callbacks execute.
- Open dropdown/portal menus, focused input, and scroll must survive.
- Resuming a selector must not restart playback, downloads, exports, or library work solely for localization.
