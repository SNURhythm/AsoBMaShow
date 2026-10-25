# bmson charts

The library imports and plays `.bmson` files from folders and supported archives,
including Android folder imports. Extension matching is case-insensitive.
Normal launch refresh discovers existing bmson files without forcing a library
rebuild. The additive database update preserves chart rows and archive caches.
Previously scanned archives reconcile newly recognized entries (including
previously unsupported modes) once using their metadata indexes, reading only
missing chart files; later refreshes use the cache. The pop’n layout hint is
persisted so five-key charts retain their active lanes after a library reload.

Both bmson 1.0 and legacy 0.21/0.2.1 are parsed in `../bms-parser-cpp`; the app
uses that repository's generated amalgamation. Versionless files use the legacy
schema. Modern fields and legacy camelCase fields are interpreted separately.

Supported layouts cover all eight modes in jbms-parser's `Mode` enum:

| Mode hint | Playable lanes | App mode identity |
| --- | --- | --- |
| `beat-5k` | 5 keys + scratch | 5 |
| `beat-7k` | 7 keys + scratch | 7 |
| `beat-10k` | 10 keys + 2 scratches | 10 |
| `beat-14k` | 14 keys + 2 scratches | 14 |
| `popn-5k` | 5 keys | 9 (pop'n rules and profile) |
| `popn-9k` | 9 keys | 9 |
| `keyboard-24k` | 24 keys + 2 wheel lanes | 24 |
| `keyboard-24k-double` | 48 keys + 4 wheel lanes | 48 |

Keyboard lanes preserve the reference's 26-lane namespace per player. Wheel
lanes are independently playable and bindable; player two starts at lane 26.
Pop'n 5-key shares the reference's pop'n mode identity while using five active
lanes. Unsupported mode hints and versions are rejected.

Sound channels are sliced at their chart-time positions, including tempo changes
and stops. Continuations advance through the source recording; restarts begin
at its start. Multiple sound channels at one lane/time form one playable note
with layered sound. Chord keys share a monophonic slice voice; separate slices
can overlap. Gameplay, preview, replay audio export and streamed music
consume these slices. A single prepared slice shares source PCM storage; no
keysound files are generated on disk. Archive loading uses the existing indexed
resource batches and reads the selected chart's media. Preview opening audio
becomes available as its sources decode, without waiting for later sources.

Metadata, long notes, bar lines, tempo/stops, BGA layers/poor events and artwork
are supported. BGA layers retain black pixels and use authored alpha. The common
`ln_type`, note `t`/`up`, `scroll_events`, `key_channels`, and `mine_channels`
extensions are also read. Hidden notes and mines use their authored source
sounds; mine damage is retained, and mines overlapping notes or holds are
suppressed. Legacy `total` is absolute; 1.0 `total` scales the selected ruleset's
default gauge gain, including zero gain. Judge ranks 0–4 use the BMS rank table,
negative ranks retain normal rank, and other ranks use whole-percent extended
rank. Nonpositive modern resolution falls back to 240 pulses per quarter note.

Compatibility was checked against the local jbms-parser checkout at
`8108cd54a19275810e1f5d2c9586f008eb6db95e`, including lane assignments, hidden
and mine channels, judge ranks, and resolution defaults. BGA sequence and
conditional-layer extensions remain unsupported. Unknown extensions are ignored.

References:

- [bmson 1.0 specification](https://bmson-spec.readthedocs.io/en/master/doc/index.html)
- [jbms-parser](https://github.com/exch-bms2/jbms-parser)
- [BmsONE legacy conversion](https://github.com/excln/BmsONE/blob/master/src/bmson/Bmson100Convert.cpp)
