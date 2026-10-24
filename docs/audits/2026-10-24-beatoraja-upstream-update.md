# Beatoraja upstream update — 2026-10-24

## Tracked checkout

- Repository: <https://github.com/exch-bms2/beatoraja>
- Local checkout: `~/workspace/SNURhythm/beatoraja`, branch `master`, tracking `origin/master`.
- Before pull: `ad42f56c4658e968f93b24bf23440fe51cb9878e` (2026-09-29), `RivalScoreAccessor : serialize rival score synchronization in background executor`.
- After `git pull --ff-only`: `83574048ba489082eb46df7fe9806b464bf1f526` (2026-10-19), `IRScoreData : add playtime`.
- Delta: 31 commits, including four merge commits; 57 files, 2,118 insertions and 386 deletions. The pull also fetched tag `0.8.9`, pointing to the new HEAD.
- AsoBMaShow comparison baseline: `9ea2ad492a7da2c05fe5c3510a6dbb8c09a326e1` on `develop`.
- Both checkouts were clean before this task. The beatoraja update was a fast-forward with no local changes to reconcile.

[Upstream comparison](https://github.com/exch-bms2/beatoraja/compare/ad42f56c4658e968f93b24bf23440fe51cb9878e...83574048ba489082eb46df7fe9806b464bf1f526).
Use the after-pull SHA as the starting point of the next upstream review.
Historical skin oracle pins remain unchanged: those record behavior at their
original commits, not a claim of compatibility with this new HEAD.

This is a source comparison and adoption recommendation. No game behavior was
changed, and neither game was built or runtime-tested for this report.

## Filtering the 0.8.9 release notes against our previous pin

The user-provided release notes summarize the release interval, which is much
larger than our update interval. **New for this review means absent at
`ad42f56c` and introduced by `83574048`.** The recommendations below already use
that commit delta; the broad release-note wording should not expand their scope.

### Remove from the new-work list: already present at `ad42f56c`

- **Random-selection folder expansion:** `1e269906` and earlier work.
- **ZIP/RAR playback, including without temporary extraction:** `d9ab59be`,
  `721856fb`. Only archive-aware Explorer handling changed in our interval.
- **Per-player tags/favorites in `songreview.db`:** `a44948c3`.
- **Song/table update progress indicators:** `e4150d25`. Keep only the later
  updater exception/cleanup fix as new.
- **Difficulty, scratch, long-note and speed-change filters:** `9c8a9987`,
  `0db8ac8e`.
- **Named properties and named/negated `op`/`draw`:** `6018645f`, `56d11123`,
  `532cc93b`.
- **Editable skin text and search input:** `8d847213`, `de7ec990`.
- **Skin previews and `skin.skinpreview`:** `b211fb72`, `187b78d5`, `8e815d5e`.
- **Practice skin definitions, properties and events:** `1a624e75`. The new
  practice distribution-graph wiring is a later fix to this existing feature.
- **Clipping:** `4d7141b1`.
- **Unicode, emoji and general fallback-font support:** `94f6b227`, `76251867`,
  `0dde1aa1`. Keep only the Lua fallback-entry decoding fix as new.
- **Numeric-display spacing at different resolutions:** `060815d4`.
- **WebP/animated WebP:** `389aa5ea` and its preceding image support. Later
  changes to the pixmap pool concern ownership/diagnostics, not format support.
- **Existing Lua property/timer/event/file/HTTP/audio APIs:** `e6d39ad4`,
  `39351940` and earlier API work. The new subset is listed separately below.
- **macOS CoreAudio and Linux ALSA through PortAudio:** `983d5577`, `396df302`.
- **PortAudio device-selection fallback:** `b823f46d` already improved fallback
  and shutdown. Further lifecycle/ASIO work is new, not initial fallback support.
- **8-bit PCM and lower-allocation PCM conversion:** `05cb9663`, `4f09014a`.
- **Guide SE long-note fix:** predates the pin; the key-sound processor is
  unchanged across our interval, and BMSPlayer's interval diff adds playtime
  measurement rather than Guide SE behavior.
- **Pitch-preserving practice time stretching:** `7ea055bb`, `97c55cc3`,
  `e8885d01`.
- **Video stride/right-edge and seek-buffer fixes, looping, first-frame and
  audio-track compatibility:** `7593a9f5`, `3a227a30`, `a9720c03`, `2ca5803e`,
  `35df11bd`, `5f2579ec`. Keep the later shutdown/resource cleanup as new.
- **GDX AudioDevice output:** `a64dceb5`, `13964aa2`.
- **Custom Judge `longnoteMargin`:** `84b65ebb` and earlier judge support.
- **`#SPEED` and its Constant-mode behavior:** `5856331e`, merged by `3b39f2e4`.
- **Score-log fixes and `avgjudge`/`oldavgjudge`:** `968b731a`.
- **Last-play timestamp, ranking/player-score and username skin properties:**
  already in the pin's `IntegerPropertyFactory`/`StringPropertyFactory`, which
  are unchanged in our interval. Last-play timestamp is different from the new
  actual performance-duration field.
- **Sequential background rival-score retrieval:** `ad42f56c` itself, with
  incremental synchronization in `0da9509d`.
- **General rendering/font/audio/database memory and GC improvements:** much
  of this predates the pin, including `8d501c49`, `5739cf4e`, `4f09014a`,
  `bd8b341e`, and shared FreeType generators in `ef715a2f`. Keep the specific
  later leaks/lifecycle fixes below; do not count the whole optimization effort.
- **Restricted Lua Java/file/OS access and limited luajava facade:** present in
  the pin's `SkinLuaAccessor`, `e6d39ad4`, `6cee0663`. The facade is expanded
  after the pin; its initial introduction/restriction is not new.
- **IR plugin isolation/removal of direct application access:** `91cba5dd`.
- **Backup advice:** operational advice, not an incoming implementation change.

### Retain from those notes: genuinely new parts of mixed or new bullets

- **Updater failure recovery:** `8320241d`.
- **Lua font-fallback decoding:** `69f2c6b3`.
- **Lua additions in `0cb03d9d`:** async HTTP/poll/cancel, byte-based HTTP
  capacity, input-gated browser opening, file deletion, richer File/input
  facades, and direct file-helper symlink rejection. Existing property/timer/
  event/audio APIs are not newly introduced by this commit.
- **Further PortAudio/ASIO lifecycle fixes and diagnostics:** `43b36b3c`,
  `d6ac510b`, `047dac73`, `a8daecc7`, `9cedde7f`, `db211724`.
- **Font texture, unused skin resource, shared pool and video shutdown cleanup:**
  `523fcdd2`, `51f76ca0`, `ff1f1d5d`; memory diagnostics in `fca09e56`.
- **Specific malformed-input fixes:** course/practice JSON, JSON skin headers,
  broken `.brd`, null song distribution and invalid sort ID in `e11b20a7`,
  `2ed69380`, `741790ee`, `9996715f`, `76a59aed`, `3bed7bc2`. Validation existed
  before the pin; these are additional cases.
- **No-follow skin/system-sound discovery:** `2d380751`.

### Additional incoming changes not clearly named in the supplied notes

- Archive-aware Explorer action (`adcde8f3`).
- Practice distribution graph preparation (`60c64638`).
- Gauge-tip alpha restoration (`4ca20052`).
- Missing-chart ordering across sort modes (`7d01f715`).
- Configurable sorter cycling, marked WIP (`939f6c66`).
- Shared settings-bound constants (`4d593d3d`).
- Actual performance duration in gameplay/course/IR records (`83574048`).

Being present in the old beatoraja pin does **not** prove that AsoBMaShow has
implemented that feature. It means it belongs to an existing parity backlog,
not to this newly pulled change set. The adoption priorities below remain:
font decoding, missing-chart ordering and practice graphs first; the new Lua
contract next; measured duration and diagnostics as follow-ups.

## What changed upstream

1. **Malformed local data handling.** Course loading filters actual JSON files,
   handles case-insensitive extensions and null entries, and closes readers.
   Practice settings reset their defaults and tolerate invalid/null JSON.
   Broken compressed replays, null song distributions, invalid sort IDs, and
   invalid JSON skin headers fail more gracefully. The launcher database updater
   dismisses its progress UI on failure and honors the configured song-info path.
2. **Filesystem traversal and archives.** Skin/system-sound discovery skips
   symbolic links. Explorer actions resolve ZIP/RAR chart paths to the archive's
   containing directory, including missing-chart lookup fallbacks.
3. **Lua font fallbacks.** Font fallback entries now accept path strings as well
   as `{path, type}` objects, enabling the intended TTF/OTF fallback declarations.
4. **Resource lifetime.** Bitmap-font caches own their texture pages. Skin
   loading retains unused resources and cleans up partial failures; disposal
   continues after an individual resource fails. Resource pools deduplicate
   concurrent loads and serialize eviction against registration. Video shutdown
   no longer depends on available command-queue space and releases retained
   movie/frame buffers reliably.
5. **Memory diagnostics.** Optional low-frequency state-transition summaries
   report JVM/native memory and estimates for fonts, images, BGA/video and
   archive resources.
6. **Rendering fixes.** Skin-authored note-distribution graphs receive practice
   start/end/speed. Gauge flicker restores base RGBA so the tip's alpha does not
   leak into subsequent nodes.
7. **PortAudio/ASIO stability.** Initialization and stream ownership are tracked,
   shutdown aborts writes before release, startup waits are bounded, and steady
   output uses native blocking writes. Added device/latency/write diagnostics and
   queries against the already-loaded ASIO library. Requested buffers below the
   ASIO minimum are raised to that minimum.
8. **Selection and settings.** All sorters put missing charts last and compare
   two missing charts by title. Configurable `enabledBarSorters` controls cycling
   for events 12 and 312 while preserving legacy numeric sort IDs; the upstream
   commit calls this WIP. Settings bounds are shared constants used by validation
   and launcher controls.
9. **Expanded Lua host contract.** Commit `0cb03d9d` adds bounded asynchronous
   HTTP (`http_get_async`, `http_poll`, `http_cancel`), input-gated `open_url`,
   `file_delete`, richer File/input facades, and API documentation. HTTP switches
   from line/character caps to raw-byte limits: 1 MiB default, 8 MiB maximum per
   request, with an optional capacity argument and legacy setter. Async requests
   have cancellation, deadlines, queue limits, disposal cleanup and a 16 MiB
   per-skin reservation/result budget. New file operations reject symlink
   components. The async transport's destination restrictions do not apply to
   the existing synchronous APIs; upstream documents that distinction.
10. **Actual performance duration.** `ScoreData` and `IRScoreData` gain `playtime`
    in milliseconds, with `-1` for unknown. Gameplay tracks real elapsed time
    separately from the speed-adjusted chart clock, excludes ending margins,
    handles early termination, and sums course stages. The IR worker protocol
    carries the field. This does not itself show that a particular IR service
    consumes it.

The new [upstream Lua API reference](https://github.com/exch-bms2/beatoraja/blob/83574048ba489082eb46df7fe9806b464bf1f526/docs/lua-main-state-api.md)
documents the larger compatibility change in detail.

## Recommended changes to AsoBMaShow

### Apply first: small, confirmed compatibility gaps

1. **Preserve Lua string-form font fallbacks** (`69f2c6b3`). In
   [LuaSkinTableDecoder.cpp](../../src/skin/beatoraja/LuaSkinTableDecoder.cpp),
   `decodeRawFontFallback` copies a string into a local `ignored` variable,
   leaving `output.path` empty. A declaration such as
   `fallback = {"fonts/fallback.ttf"}` therefore loses the requested face.
   Preserve that path and check its use through the existing scalable-font
   resource path. Extend the existing decoder coverage with a missing-glyph
   fallback check; do not treat this as a request to rewrite bitmap fonts.
2. **Apply missing-chart ordering to every sorter** (`7d01f715`).
   [MusicSelectBarManager.cpp](../../src/music_select/MusicSelectBarManager.cpp)
   applies missing-chart handling only to ARTIST/BPM/LENGTH/LEVEL, after the
   TITLE early return, and treats two missing songs as tied. Move the missing
   song comparison ahead of the individual sort rules and use the upstream
   title tie-break. Check table rows with and without scores, mixed folder/song
   rows, and the paged/SQL routes before claiming all selector paths agree.
3. **Wire practice state into authored distribution graphs** (`60c64638`).
   The `noteDistribution` branch in
   [Skin2DRenderer.cpp](../../src/skin/beatoraja/Skin2DRenderer.cpp) supplies the
   gameplay graph and play cursor but not practice start/end or speed. Its
   [render request](../../src/skin/beatoraja/SkinNoteDistributionGraphRenderer.h)
   already has optional range fields, and
   [PlaySkinStateBridge.cpp](../../src/skin/beatoraja/PlaySkinStateBridge.cpp)
   exposes practice range/frequency. Connect these for the active practice menu
   and reproduce upstream cursor scaling. Verify a skin-authored graph at a
   non-100% practice speed; the built-in practice presentation is a separate path.

### Apply next: the updated Lua compatibility surface

4. **Expand the limited File and input facades** (`0cb03d9d`).
   [LuaSkinHostModules.cpp](../../src/skin/beatoraja/LuaSkinHostModules.cpp)
   currently exposes File `mkdir`/`listFiles` and Gdx input `isKeyPressed`.
   Add the upstream File constructor alias, `isFile`, `isDirectory`, `getName`,
   `getPath`, `tostring`, and File-valued `listFiles`; add key-edge/button/pointer
   input methods and `Input.Buttons`. Match the new key-name resolution too.
   Returning File facades instead of strings changes observable behavior, so
   update the versioned contract and tests deliberately. Keep the limited
   facade model; arbitrary Java access is unnecessary.
5. **Implement the HTTP/browser API with ownership and bounds** (`0cb03d9d`).
   [LuaSkinHttpClient.h](../../src/skin/beatoraja/LuaSkinHttpClient.h) still sets
   1,024 lines / 65,536 characters; there are no async/poll/cancel/open-url exports
   in the host module. Update synchronous and legacy byte-limit semantics in
   both native transports, then add skin-owned async jobs and browser actions.
   Match terminal-result consumption, cancellation/disposal, queue/deadline and
   aggregate-budget behavior. Transport workers must not invoke Lua. Exercise
   UTF-8 byte accounting, absent/misleading Content-Length, cancellation during
   reads and skin teardown, and preview/export capability restrictions.
6. **Align direct file helpers with new symlink rules and deletion** (`0cb03d9d`).
   [LuaSkinFileSystem.cpp](../../src/skin/beatoraja/LuaSkinFileSystem.cpp)'s
   `rejectAliasedPath` explicitly skips no-follow checks for pinned Lua path
   semantics, citing the old upstream lexical-only resolver. Revisit that
   exception for the updated `main_state.file_*` and File facades, and implement
   regular-file-only `file_delete`. Preserve the distinctions between these
   helpers, legacy Lua `io`/module loading, resource resolution and private
   overlay storage; upstream did not make all path APIs identical. Check
   linked ancestors, dangling links, directories and preview denial.

### Useful follow-ups, with narrower justification

7. **Add real attempt duration when needed for IR/history** (`83574048`).
   AsoBMaShow already stores `play_duration_seconds`, but
   [ScoreProvenance.cpp](../../src/ScoreProvenance.cpp)'s
   `makeScoreProvenance` derives it from chart `PlayLength`, rather than measuring
   the attempt. That is not equivalent to upstream's millisecond duration for
   failures, speed changes or unknown historical records. Introduce a distinct
   measured value and unknown state, then carry it through result/course
   persistence and supported IR payloads. Preserve existing provenance/hash and
   historical-statistics semantics; do not rename the old field blindly.
8. **Consider configurable sorter cycling** (`939f6c66`). This is a product
   choice plus skin-event compatibility work, not the same fix as missing-chart
   ordering. If adopted, sanitize empty/duplicate choices, preserve numeric IDs,
   and make native controls and skin events agree. Upstream marks it WIP.
9. **Borrow lifecycle diagnostics and regression scenarios**, rather than Java
   classes (`fca09e56`, `523fcdd2`, `51f76ca0`, `ff1f1d5d`). Check repeated skin
   switches, optional unused assets, partial load failures, concurrent resource
   preparation, and video stop during decode. AsoBMaShow's
   [SkinResourceCatalog.cpp](../../src/skin/beatoraja/SkinResourceCatalog.cpp),
   [VideoPlayer.cpp](../../src/video/VideoPlayer.cpp) and
   [VideoMemoryBudget.h](../../src/video/VideoMemoryBudget.h) have their own
   ownership/budget structures. This review does not establish an equivalent
   native leak. Any added diagnostics should use retained counters/metadata,
   not scan or reread archive payloads.

## Already covered or unsuitable for direct porting

- **Gauge alpha restoration:** the local gauge `emitRole` copies the evaluated
  geometry for each node and multiplies alpha on that copy. The shared mutable
  color alias fixed by `4ca20052` is absent from this path.
- **Archive Explorer resolution:**
  [MusicSelectExternalActions.cpp](../../src/music_select/MusicSelectExternalActions.cpp)
  already resolves archive members and archive folders to the containing
  directory, including fallback lookups. `MusicSelectScene` supplies the archive
  splitter to skin event 212. Native file reveal is also archive-aware. Review
  target-existence/error behavior if needed; no new archive-resolution feature
  is required by `adcde8f3`.
- **Java PortAudio blocking-write implementation:**
  [AudioBackend.cpp](../../src/audio/AudioBackend.cpp) uses callback-driven
  PortAudio, factory-owned initialization, native stream ownership, effective
  latency information and callback/underflow counters. Its ASIO device-buffer
  negotiation may merit a Windows hardware check, but copying startup polling,
  JNA DLL lookup or blocking-mixer machinery would be inappropriate.
- **JavaFX settings/updater code and Java stream/LibGDX disposal fixes:** port
  behavior only where an equivalent native issue is demonstrated. The replay
  codec already catches JSON parse errors and validates structure; practice
  presets already validate object structure and report JSON exceptions. The
  upstream malformed-data examples are useful targeted cases, not evidence
  that our implementations currently crash.
- **Null `songinfo.db` distribution handling:** AsoBMaShow has its own chart and
  graph model; no direct Java bean/SQL mapper patch applies. Check any future
  beatoraja database importer at that boundary.

## Validation and future baseline

The report's commit range, file counts and commit subjects were read from Git;
recommendations above were checked against the cited local source paths.
Documentation validation consists of a diff/whitespace review, local-link
checks, and verifying beatoraja HEAD equals `origin/master`. No runtime bug
reproduction or new compatibility implementation is claimed.

For subsequent implementation, use the relevant groups in
[the testing guide](../testing/README.md): `skin_runtime`/`skin_assets` for Lua
and fonts, `music_select` for sorting, `practice`/`rendering` for graph wiring,
and `results`/`ir` for measured duration. Keep historical oracle fixtures pinned;
new expectations should explicitly identify this updated upstream commit.

## Complete incoming commit list

Chronological Git traversal of `ad42f56c..83574048`; dates are author dates.

```text
e11b20a7 2026-09-30 CourseDataAccessor : fix json file processing
2ed69380 2026-09-30 PracticeConfiguration : fix json file processing
2d380751 2026-09-30 Skin/SystemSound : disable symbolic link
741790ee 2026-09-30 JSONSkinLoader : modify loadHeader exception handling
adcde8f3 2026-09-30 EventFactory : fix open_with_explorer with zip/rar song
9996715f 2026-09-30 PlayDataAccessor : fix broken .brd handling
76a59aed 2026-09-30 SongInformationAccessor : fix NULL distribution handling
3bed7bc2 2026-09-30 PlayerConfig : fix illegal sortid handling
69f2c6b3 2026-10-03 enable ttf/otf fallback for lua skin
fca09e56 2026-10-05 add MemoryDiagnostics
523fcdd2 2026-10-06 BitmapFontCache : fix font texture disposing
2e479f62 2026-10-06 Merge pull request #885 from Nekokan/lua-ttf-otf-fallback
51f76ca0 2026-10-06 Skin : fix unused Texture/Pixmap disposing
ff1f1d5d 2026-10-06 ResourcePool : fix resource disposing
d22ce10b 2026-10-06 Merge remote-tracking branch 'origin/master'
60c64638 2026-10-07 SkinNoteDistributionGraph : fix prepare in practice mode
8320241d 2026-10-07 PlayConfigurationView : fix songdata.db updater exception handling
4ca20052 2026-10-16 SkinGauge: restore base RGBA after tip flicker
43b36b3c 2026-10-16 PortAudioDriver : fix ASIO duplicate initialization
67f6a8b1 2026-10-16 Merge pull request #886 from 010m010/codex/fix-gauge-tip-flicker
d6ac510b 2026-10-17 PortAudioDriver : add logInfo
d9f3dde2 2026-10-17 Merge remote-tracking branch 'origin/master'
047dac73 2026-10-17 add PortAudioAsioDiagnostics
a8daecc7 2026-10-17 PortAudioDriver : add WriteStatistics
9cedde7f 2026-10-17 PortAudioDriver : add STARTUP/NATIVE_BLOCKING mode
7d01f715 2026-10-17 BarSorter : fix missing chart handling
939f6c66 2026-10-17 PlayerConfig : add enabledBarSorters (WIP)
db211724 2026-10-17 PortAudioDriver : clamp audio buffer size
4d593d3d 2026-10-17 Config : add min/max static value
0cb03d9d 2026-10-18 LuaSkinLoader : add luajava facede methods
83574048 2026-10-19 IRScoreData : add playtime
```
