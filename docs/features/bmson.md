# bmson charts

The library imports and plays `.bmson` files from folders and supported archives,
including Android folder imports. Extension matching is case-insensitive.
Normal launch refresh discovers existing bmson files without forcing a library
rebuild. The additive database update preserves chart rows and archive caches.
Previously scanned archives reconcile newly recognized entries once using their
metadata indexes, reading only missing chart files; later refreshes use the cache.

Both bmson 1.0 and legacy 0.21/0.2.1 are parsed in `../bms-parser-cpp`; the app
uses that repository's generated amalgamation. Versionless files use the legacy
schema. Modern fields and legacy camelCase fields are interpreted separately.

Supported layouts match the existing BMS player: beat 5/7/10/14 keys and pop'n
9 keys. Unsupported mode hints and versions are rejected. Other game layouts
are not silently mapped onto beat-7k.

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
`ln_type`, note `t`/`up` and `scroll_events` extensions are also read. Other extensions are ignored. Legacy `total` is absolute; 1.0
`total` scales the selected ruleset's default gauge gain, including zero gain.
Judge width uses the existing whole-percent extended-rank setting.

References:

- [bmson 1.0 specification](https://bmson-spec.readthedocs.io/en/master/doc/index.html)
- [jbms-parser](https://github.com/exch-bms2/jbms-parser)
- [BmsONE legacy conversion](https://github.com/excln/BmsONE/blob/master/src/bmson/Bmson100Convert.cpp)
