# File Replay Contract Matrix

`replay::capabilitiesFor` is the executable authority for this table. UI and
service call sites must consume that policy when their delivery slice is
activated; they must not recreate it with local booleans.

| Origin | Replay state | Records | View Result | Replay actions | Delete | IR | Profile transfer |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Modern chart | verified | yes | yes | Watch, Retry Same, G-Battle, practice ghost, video, share/copy | yes | saved snapshot only | record and verified file |
| Modern course | verified | yes | yes | Watch, Retry Same, video, share/copy | yes | saved snapshot only | record and verified file |
| Modern chart/course | missing or user-deleted | yes | yes | none | no | saved snapshot only | record only |
| Modern chart/course | corrupt, mismatched, or unsupported extension | yes | yes | none | yes | saved snapshot only | record only |
| Legacy chart/course summary | every state | yes | no | none | no | no reconstruction | summary only |
| Imported stock BRD | verified | no | no | Watch and share/copy only | yes | never | verified file only |
| Imported stock BRD | missing or user-deleted | no | no | none | no | never | none |
| Imported stock BRD | corrupt, mismatched, or unsupported extension | no | no | none | yes | never | none |
| Imported remote result | every state | yes | existing remote detail | none | no | never | remote record only |

G-Battle and practice ghost are chart-only. A replay file never grants result
or IR authority. A result or saved IR snapshot never makes an absent replay
playable. Invalid-file deletion means deletion of an existing contained file;
missing and user-deleted states are already absent.

## Activated contract authorities

| Contract | Current authority | Replay-file dependency | Delivery status |
| --- | --- | --- | --- |
| Modern chart result facts | `result_persistence::ModernChartResult` validation, fingerprinting, agreement, recall, and schema-v11 repository storage | none | active |
| Modern course result facts | `result_persistence::ModernCourseResult` validation, fingerprinting, ordered-stage agreement, schema-v12 repository storage, and atomic recall | none | active for complete and failed-partial courses |
| Shared result/setup domains | `result_contract` key modes, chart identity agreement, gauge/clear domains, and score arithmetic | none | active for replay setup, modern results, recall, and IR |
| Postponed IR payload | canonical `ir::IrSubmissionSnapshot` captured from a validated modern result and stored atomically with it | none | active for modern charts |
| Modern chart capture | `ReplayInputRecorder`, `ReplaySetupProvenance`, and `ChartReplayCapture` | live logical input only | active; raw input is encoded to BRD and is not written to SQLite |
| Modern chart file ownership | `ReplayRepository::GetResolvedProfileRoot`, `BeatorajaReplayPath`, `ReplayFileStore`, and `ChartReplayPersistence` | contained `replay/*.brd` path and verified metadata | active with recoverable summary-only fallback |
| Modern chart replay availability | `ChartReplayContext`, `makeParsedChartReplayFacts`, and `replay::capabilitiesFor` | verified file only for replay actions | active in Records, Watch, Retry Same, G-Battle, practice ghost, and video |
| Modern chart consumer preparation | `ChartReplayConsumer`, `ReplaySetupAdapter`, shared raw driver, and judged materializer | verified file only | active; all chart replay consumers use one pipeline |
| Modern course capture and continuation | `CourseReplayCapture` and immutable `CourseContinuation` transitions | live stage result facts plus live logical input | active; complete and failed-partial prefixes share checked score, combo, gauge, setup, rest, and ordering rules |
| Modern course file ownership | `ReplayFileAssociationCoordinator`, `BeatorajaReplayPath::courseStem`, `ReplayFileStore`, and `CourseReplayPersistence` | contained `replay/*.brd` path and verified metadata | active with recoverable summary-only fallback and exclusive chart-or-course ownership |
| Modern course replay availability | `CourseReplayContext`, `CourseReplayConsumer`, and `replay::capabilitiesFor` | verified file only for replay actions | active in Records, Watch, Retry Same, and video; result recall remains file-independent |
| Modern course consumer preparation | `CourseReplayConsumer`, shared setup/identity/result agreement, raw playback materialization, and `CourseContinuation` | verified file only | active; scene and video adapters are produced only after the complete saved prefix agrees |
| Modern replay reference agreement | `ReplayReferenceAgreement` consumed by chart/course contexts and `ReplayFileActionService` | none for identity; inspection remains file-dependent | active; actions cannot share, tombstone, or remove a canonical path whose stem does not belong to its saved result |
| Modern replay share and deletion | `ReplayFileActionService`, stable verified snapshots, durable `modern_replay_files.user_deleted`, and `ReplayCapabilities` | share requires verified bytes; delete requires a verified or existing invalid entry | active for chart and course Records; result, IR, and reference history survive file deletion |
| Replay startup reconciliation | `ReplayFileReconciler` after replay schema readiness | exact tombstoned ownership only | active; stale private temporaries and tombstoned entries are retried without touching active or unreferenced files |
| Profile replay duplication | `ReplayProfileTransfer` over `loadAgreedModernReplayFileInventory` and the staged replay database | verified active result-agreed references only | active; missing and user-deleted bytes are omitted, invalid referenced bytes abort staging, and result references always remain in the database snapshot |
| Portable profile replay archive | profile archive format 3 plus `ReplayProfileTransfer` validation | verified active references only | active; v1/v2 imports remain supported without replay members, and extra or disagreeing v3 members fail before atomic install |
| Legacy result and IR bridge | explicitly named legacy projection/adapter functions | legacy in-memory detail only | temporary until the Slice 7 schema cutover |

The activated result and postponed-IR contracts do not accept a replay path,
file reference, playback collection, repository handle, outbox receipt, or
SQLite handle. Rich result recall reads the stored chart path and rejects chart
identity disagreement before applying independently saved display facts.
Missing, corrupt, unsupported, or mismatched chart BRDs change only the shared
replay state and replay-dependent capabilities; the result row and postponed IR
snapshot remain independently usable.
