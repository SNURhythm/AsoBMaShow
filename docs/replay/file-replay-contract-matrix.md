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
