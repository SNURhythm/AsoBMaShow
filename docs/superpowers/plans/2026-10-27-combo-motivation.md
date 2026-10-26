# Combo Motivation Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement the user's selected motivation suggestions 1–5.
**Architecture:** Keep presentation calculations separate from saved state. Connect StreakScene to the existing native and custom selector assignment folders through an explicit callback; use the existing deferred scene lifecycle. Extend existing progress views and native cards.
**Tech Stack:** C++20, SDL, Yoga, bgfx, existing localization and CTest fixtures.
**Spec:** docs/superpowers/specs/2026-10-27-combo-motivation.md

## Constraints
- Work in the current checkout; no worktree or deployment. Commit and push verified changes to feature/streak.
- Preserve scoring, saved-state schema, action scroll/animation continuity, and five-language localization.
- Reuse focused tests and native layout harnesses; one build per build directory.

## Tasks
- [x] 1. Add pure dashboard presentation helpers and behavioral tests for seven local days and the next unearned milestone; add remaining-chart wording to the existing animated round view.
- [x] 2. Add direct assignment navigation callbacks in StreakScene, MainMenuScene and retained MusicSelectScene. Verify requested assignment, existing filters/search, paused/resumed ownership and stale IDs.
- [x] 3. Build the recap, activity strip, milestone preview and localized copy. Verify compact/wide native layout and preservation of existing state.
- [x] 4. Run focused model/scene/selector/localization checks and desktop build. Review the complete change, update feature documentation/PR, commit and push.

## Review focus
- Saved history before the routine existed versus an unplayed configured day.
- Previously earned milestones after a combo reset; all milestones already earned.
- Round transition frame/count consistency and no repeated celebration on rebuild.
- Navigation from both native tabs and a retained custom selector, including lingering filters.
- Compact layouts and long localized text.

## Verification notes
- Model coverage checks seven-day ordering across a year boundary, all activity states, reset/earned milestones and completion of all badges.
- Scene coverage runs both production navigation functions and the real deferred queue. A review found that returning false retained the embedded shortcut; a failing reopen regression confirmed it, then the callback was changed to consume embedded navigation and retain standalone destruction safety.
- Desktop build and focused scene, model, selector projection/bar manager, tab restoration and localization checks passed. Native Metal previews cover phone/wide, light/dark, completed/incomplete/new routines, Korean text and the result pop-up.
