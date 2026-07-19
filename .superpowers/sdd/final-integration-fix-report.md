# Bokutachi IR Final Integration Fix Report

## Outcome

Both Important findings in `final-three-plan-integration-review.md` are fixed
on `feature/bokutachi-ir`.

- Implementation/test commit: `fedb49f27df4dddf93fcfd0d308bb7b550609ef1`
  (`fix: isolate IR evidence across credentials`)
- Provider-wide credentials now invalidate receipt-backed succeeded outbox
  rows, submission receipts, and mirrored remote scores for every origin of
  that provider in one SQLite transaction.
- API-key validation is shared with `IrCredentialStore` and runs before
  quiescence, invalidation, credential writes, publication, or reactivation.
- A retained Main Menu observes provider-evidence revisions and rebuilds Song
  Select projections and visible Records models after account evidence is
  cleared.

No deployment or push was performed.

## Implementation

### Shared API-key validation before side effects

`IrCredentialStore::isApiKeyFormatValid()` is the single format rule used by
credential load, save validation, direct replacement, and the Settings action
model. It rejects empty keys, keys above 4096 bytes, ASCII whitespace/control
bytes, and DEL.

`IrSettingsActionModel::replaceCredential()` applies that rule before checking
mutation dependencies or constructing its reactivation guard. Rejected pasted
keys therefore retain the existing credential and produce zero quiesce,
evidence-invalidation, credential-write, commit-publication, and reactivation
calls. Diagnostics remain generic and do not retain the submitted key.

### Atomic provider-wide evidence clearing

`ReplayRepository::ClearIrProviderAccountEvidence(providerId)` performs these
deletes inside one `BEGIN IMMEDIATE` transaction:

1. succeeded outbox rows owned by same-provider submission receipts and their
   linked replay attempts;
2. all submission receipts for the provider across origins;
3. all mirrored remote scores for the provider across origins.

The operation preserves other providers, pending/non-succeeded outbox rows,
and succeeded outbox rows without a submission-owned receipt. Any failure in
the three delete stages rolls the complete transaction back. The existing
origin-scoped API remains available and unchanged.

The Settings dependency is now explicitly provider-scoped. It pauses profile
IR work, invokes the one provider-wide repository mutation, clears all cached
Bokutachi user IDs, invalidates the provider ranking cache, publishes an
account-evidence revision, and only then attempts credential replacement or
removal. The submission service is reactivated through the existing guard on
all post-pause exits.

### Retained-view invalidation

`ApplicationContext::irAccountEvidenceRevision` publishes successful evidence
invalidation to the retained Main Menu. Its projection refresh observes the
revision independently of reconciliation success, rebuilds score/lamp views,
and reloads visible Records models. This prevents old in-memory mirror or badge
state from surviving a credential change after the durable rows are gone.

## TDD Evidence

### Baseline

Before adding regressions, the affected credential, Settings, repository, and
submission-service suites passed, as did
`scripts/check_main_menu_settings_anchor.py`.

### RED

After adding the invalid-key side-effect regression and updating the source
contract, the focused run failed at the intended missing behavior:

```text
requirement failed at line 524: result.status ==
  ir::IrSettingsActionResult::Status::Invalid
FAIL: missing or out-of-order credential invalidation step:
  .invalidateProviderIdentity =
FAIL: credential changes must use one atomic provider-wide account-evidence
  repository call
```

After adding the provider-wide repository, rollback, durable-preservation, and
origin A -> origin B -> credential change/removal -> origin A regressions, the
focused build failed because the required interfaces did not exist:

```text
ReplayRepository has no member named ClearIrProviderAccountEvidence
IrSettingsActionDependencies has no member named invalidateProviderIdentity
```

The retained-view source contract also failed before implementation:

```text
FAIL: the retained Main Menu must observe provider account-evidence revisions
and rebuild projected score and Records views
```

### GREEN

Focused build:

```text
cmake --build cmake-build-debug --target \
  ir_credential_store_tests ir_settings_presentation_tests \
  replay_repository_tests ir_submission_service_tests -j 6
```

Result: exit 0; all four executables built.

Focused CTest:

```text
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(ir_credential_store|ir_settings_presentation|replay_repository|ir_submission_service)_tests$' \
  -j 4
```

Result: **4/4 passed**, 0 failed.

Source contract:

```text
python3 scripts/check_main_menu_settings_anchor.py
```

Result: `main-menu Settings footer audit passed`.

## Final Verification

Complete test gate:

```text
ctest --test-dir cmake-build-debug --output-on-failure -j 6
```

Result: **122/122 passed**, 0 failed, in 17.20 seconds. This includes the
Records marker, result recall, ranking flow, and Settings/Main Menu audits.

Desktop build gate:

```text
cmake --build cmake-build-debug --target main -j 6
```

Result: exit 0; `main` linked successfully. The build emitted only the
repository's existing bgfx GNU variadic-macro warnings.

Diff gates:

- `git diff --check`: clean before commit.
- The implementation commit changes only the scoped build, IR, repository,
  Settings/Main Menu, audit, and test files.
- `src/bms_parser.hpp` and `src/bms_parser.cpp` were not changed.

## Regression Coverage

- Settings rejects empty, whitespace-bearing, control-bearing, DEL-bearing,
  and oversized API keys without any mutation side effect.
- Invalid credential actions preserve seeded mirror, receipt, outbox, and
  uploaded-record state across repository reopen.
- Provider clearing removes two origins atomically while preserving a second
  provider, pending work, and receiptless succeeded work.
- Forced failure at each delete stage preserves all provider evidence.
- Both credential replacement and removal after switching from origin A to B
  leave origin A and B without old mirror, receipt, outbox, or uploaded badge
  state when origin A is revisited.
- Submission-service quiescence remains ordered before provider evidence
  invalidation.
- The source audit requires Settings to use the single provider-wide call and
  requires the retained Main Menu to refresh projections and Records from the
  evidence revision.

## Files Changed

- Build/audit: `CMakeLists.txt`,
  `scripts/check_main_menu_settings_anchor.py`
- Credential/action model: `src/ir/IrCredentialStore.h`,
  `src/ir/IrCredentialStore.cpp`, `src/ir/IrSettingsPresentation.h`,
  `src/ir/IrSettingsPresentation.cpp`
- Durable evidence: `src/repositories/ReplayRepository.h`,
  `src/repositories/ReplayRepositoryIrRemoteScores.cpp`
- Runtime/view invalidation: `src/context.h`,
  `src/scene/SettingsSceneIr.cpp`, `src/scene/MainMenuScene.h`,
  `src/scene/MainMenuScene.cpp`
- Tests: `tests/ir_settings_presentation_tests.cpp`,
  `tests/ir_submission_service_tests.cpp`,
  `tests/replay_repository_tests.cpp`

## Self-Review and Concerns

- The key validator has no logging or diagnostic path containing the key.
- The provider-wide transaction deletes only account evidence; it does not
  delete local replays or local score rows.
- Outbox deletion retains the existing receipt-ownership rule and does not
  broaden deletion to pending, failed, blocked, or receiptless rows.
- Cache and ranking invalidation remain provider-scoped, while cached user IDs
  are cleared for all origins to match provider-only credentials.
- No generated parser, shader, platform deployment, signing, or release file
  was touched.

No new fix-specific concern remains. The prior review's Minor finding—the lack
of an executable smoke test for the real bgfx/readback/PNG result-export
backend—remains outside this credential/evidence fix scope.
