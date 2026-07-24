# Difficulty-Table URL Completion Identity Design

## Goal

Keep the existing success-only URL clearing behavior without erasing a different URL that the user types while a difficulty-table import is still running.

## Completion identity

When `addDifficultyTableFromUrl()` starts an import, the background job retains the submitted URL and includes it in the finished completion mailbox message. Completion processing compares that message-bound value with the current `tableUrlText`.

A successful, finished import clears the URL only when the current text still equals the submitted URL. If the user changes the field before completion, the newer value remains in both the scene model and the visible `TextInputBox`. Failed, cancelled, or unfinished imports never clear either value.

The UI consumes the submitted URL together with the finished completion. Binding identity to the mailbox message prevents a newly started job from overwriting the identity of an older completion that is still waiting for the UI thread.

## Component boundary

Extend `settings_ui::applyDifficultyTableUrlCompletion` to accept the submitted URL and current URL. The pure helper remains responsible for deciding whether to clear; `SettingsScene` transports the submitted value through its existing thread-safe progress mailbox and synchronizes the native/UI text box only when the helper reports a clear.

This keeps the race policy directly testable without introducing a generation counter or disabling edits during import.

## Testing

Extend `difficulty_table_url_completion_tests` before implementation. The regression test must prove that a successful completion for URL A does not clear current URL B. Existing cases must continue to prove that unfinished and failed imports preserve the field and that a successful completion clears an unchanged submitted URL.

After the focused red-green cycle, build `main`, run the focused CTest, and run the complete CTest suite. Commit and push only the identity fix and its documentation; do not reply to or resolve GitHub review threads unless separately requested.
