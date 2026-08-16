# Profiles and data transfer

## Intent and user flow

Profiles isolate player settings, input bindings, scores, replays, and optional
IR state while preserving a shared chart library. Users can create, duplicate,
switch, export, import, and delete profiles. Import/export must be recoverable
even when filesystem, archive, or database validation fails.

## Code map

- Profile managers, bootstrap helpers, archive code, and path utilities live
  at the `src/` root and in settings/repository domains.
- `src/scene/ProfileSettingsController.*`, profile runtime-reapply code, and
  settings scenes own user interactions and application of an active profile.
- Document-handoff services bridge import/export URIs across desktop and mobile
  platforms.
- Repositories and replay lifecycle code provide the profile-scoped durable
  payloads.

## Boundaries and invariants

Each profile has a versioned private directory layout. Create, duplicate, and
non-overwrite import stage their work and report the filesystem-derived
transaction outcome. Overwrite/import validation uses checksums, SQLite backup
or integrity checks, and atomic replacement so active data remains recoverable.
Profile switching rebuilds runtime services through explicit reapply hooks;
scenes do not retain stale profile-owned resources.

## Verification

Use `player_profile_manager_tests`, `profile_switch_tests`,
`profile_archive_tests`, `profile_export_staging_tests`,
`profile_runtime_reapply_tests`, and profile settings/controller tests.

## Related pages

- [Input and controllers](input-and-controllers.md)
- [Results, records, and persistence](results-records-and-persistence.md)
- [Mobile and platform integration](mobile-and-platform-integration.md)
