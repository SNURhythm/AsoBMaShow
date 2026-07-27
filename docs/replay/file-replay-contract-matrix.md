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
| Modern chart result facts | `result_persistence::ModernChartResult` validation, fingerprinting, agreement, and recall | none | active; SQLite ownership is Slice 4 |
| Modern course result facts | `result_persistence::ModernCourseResult` validation, fingerprinting, ordered-stage agreement, and atomic recall | none | active; SQLite ownership is Slice 4 |
| Shared result/setup domains | `result_contract` key modes, chart identity agreement, gauge/clear domains, and score arithmetic | none | active for replay setup, modern results, recall, and IR |
| Postponed IR payload | canonical `ir::IrSubmissionSnapshot` captured from a validated modern result | none | active; outbox persistence is Slice 4 |
| Legacy result and IR bridge | explicitly named legacy projection/adapter functions | legacy in-memory detail only | temporary until the Slice 7 schema cutover |

The activated result and postponed-IR contracts do not accept a replay path,
file reference, playback collection, repository handle, outbox receipt, or
SQLite handle. Rich result recall reads the stored chart path and rejects chart
identity disagreement before applying independently saved display facts.
