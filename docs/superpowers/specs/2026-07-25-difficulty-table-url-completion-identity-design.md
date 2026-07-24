# Difficulty-Table URL Completion Identity Design

## Goal

Keep the existing success-only URL clearing behavior without erasing a different URL that the user types while a difficulty-table import is still running.

## Completion identity

When `addDifficultyTableFromUrl()` starts an import, capture the submitted URL in scene state as well as in the background job. Completion processing compares that submitted value with the current `tableUrlText`.

A successful, finished import clears the URL only when the current text still equals the submitted URL. If the user changes the field before completion, the newer value remains in both the scene model and the visible `TextInputBox`. Failed, cancelled, or unfinished imports never clear either value.

The submitted URL is cleared from scene state after a finished completion is consumed, regardless of success, so a later completion cannot reuse stale identity.

## Component boundary

Extend `settings_ui::applyDifficultyTableUrlCompletion` to accept the submitted URL and current URL. The pure helper remains responsible for deciding whether to clear; `SettingsScene` remains responsible for capturing the submitted value and synchronizing the native/UI text box only when the helper reports a clear.

This keeps the race policy directly testable without introducing a generation counter or disabling edits during import.

## Testing

Extend `difficulty_table_url_completion_tests` before implementation. The regression test must prove that a successful completion for URL A does not clear current URL B. Existing cases must continue to prove that unfinished and failed imports preserve the field and that a successful completion clears an unchanged submitted URL.

After the focused red-green cycle, build `main`, run the focused CTest, and run the complete CTest suite. Commit and push only the identity fix and its documentation; do not reply to or resolve GitHub review threads unless separately requested.
