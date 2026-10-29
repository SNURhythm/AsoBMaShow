# Independent folder goals

Allow multiple saved goals for the same folder/table/level, goal type, and LN mode. Add always creates an independent goal. Detail, edit, cancel, remove, dates, first achievements, and Kombo credit refer to the selected goal's stable identity.

Keep `folder_goals(id TEXT PRIMARY KEY, body TEXT NOT NULL) WITHOUT ROWID` intact. Existing version-1 rows retain their exact keys; loading them assigns that key as their identity without writing. New goals use opaque random IDs, and new/edited payloads use version 2 with an explicit matching ID. This avoids the table rename/rebuild that exposed invalid attached-score triggers during chart migration. No bulk migration, score-trigger change, or private fixture content is needed.

The repository returns the persisted identity and timestamps only after commit. Existing IDs must exist and retain source/mode/type identity; stale edits must not resurrect deleted goals. All writes remain transactional. Existing achievement keys remain stable; new IDs distinguish simultaneous completions.

Source pickers open rows by ID and preserve zero/one/many routing. New drafts may use occupied scopes, keeping entered values. Selector flags stay achieved only when all goals in their scope are achieved. A bubble summarizes a count when multiple pending goals of one type would otherwise overwrite each other; simple one-per-type progress remains available.

Alternative considered: rebuilding into an integer-ID table like chart goals. Rejected because the current text-key table already supports independent identities, and avoiding schema replacement reduces upgrade risk.

Verification covers version-1 add-first/edit-first upgrades with the previously failing trigger, unchanged legacy bytes/dates/keys, isolated same-scope edits/removals, stale and failed saves, picker cancellation, and independent simultaneous credit. Test a temporary copy of the affected iOS profile and run the storage tests with the iOS SQLite runtime if available. Never modify, commit, or upload the private baseline.
