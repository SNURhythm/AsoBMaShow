# Beatoraja Type-5 Compatibility Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:executing-plans` to implement this plan task-by-task. Execute
> inline in the current checkout; the user prohibited worktrees and subagents.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Admit type-5 Lua skins and implement their exact source-derived
loading, song-list model, rendering, runtime bridge, and owning session.

**Architecture:** Extend the existing Lua/common renderer pipeline with a
music-select target and a specialized canonical `SkinSongListObject`. A pinned
source ledger defines completeness. Type-5 decoding follows Beatoraja's loader
boundaries and bypasses AsoBMaShow validations that Beatoraja does not perform.

**Tech Stack:** C++23, Lua 5.4, bgfx, Python 3 source extraction, CMake/CTest.

**Spec:**
`docs/superpowers/specs/2026-09-01-beatoraja-lua-music-select-design.md`

## Global Constraints

- Compatibility authority is exactly Beatoraja commit
  `c2ed5db1a46145ed10790c3872f717e95b59db9d` at
  `/Users/xf/workspace/SNURhythm/beatoraja`.
- Do not derive behavior from `/Users/xf/Downloads/Skins`.
- Add no type-5 range, size, shape, consistency, coercion, resource-preflight,
  or semantic validation absent from the pinned source.
- The newly exposed music-select catalog format is `.luaskin`; the bundled
  Beatoraja `select.json` is an oracle, not a newly exposed format.
- Every valid source-surface row must end with executable evidence or an exact
  source-defined no-op classification.
- Make each commit a coherent production slice with its focused tests and
  ledger rows; fold trivial corrections into that slice and split a slice
  before it becomes broad enough to obscure review.
- Do not modify parser amalgamation files or run a whole-file formatter.

---

### Task 1: Pinned music-select source surface and evidence gate

**Files:**
- Create: `scripts/extract_beatoraja_music_select_skin_surface.py`
- Create: `docs/skin-compat/beatoraja-music-select-source-surface-v1.json`
- Create: `docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json`
- Create: `tests/beatoraja_music_select_skin_ledger_tests.py`
- Create: `tests/beatoraja_music_select_skin_ledger_evidence_tests.py`
- Create: `tests/music_select_skin_ledger_evidence.h`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: pinned Beatoraja Java source and the gameplay extractor's parsing
  helpers.
- Produces: sorted source rows with `id`, `source.path`, and `source.symbol`,
  plus ledger rows with runnable native-test ownership.

- [ ] **Step 1: Write a failing extractor contract test**

```python
def test_type5_surface_contains_songlist_and_selector_runtime(self):
    surface = music_select.extract(BEATORAJA_ROOT)
    identifiers = {row["id"] for row in surface["features"]}
    for expected in {
        "lua.object-field.song-list-center",
        "lua.object-field.song-list-clickable",
        "select.skin-bar.bar-count",
        "select.property.boolean.music-selector",
        "select.property.integer.music-selector",
        "select.property.float.music-selector",
        "select.property.string.music-selector",
        "select.property.event.music-selector",
        "select.input.music-select-input-processor",
    }:
        self.assertIn(expected, identifiers)
```

The test invokes the extractor against the local checkout and asserts the full
commit hash before inspecting rows.

- [ ] **Step 2: Run the test and observe the missing extractor failure**

Run: `python3 -m unittest tests/beatoraja_music_select_skin_ledger_tests.py -v`

Expected: import/file failure for the absent music-select extractor.

- [ ] **Step 3: Implement source reachability from actual type-5 loaders**

Reuse `json_classes`, `top_level_public_fields`, and normalization helpers from
the gameplay extractor. Seed reachable JSON classes from
`JsonSelectSkinObjectLoader.java` plus `JsonSkinObjectLoader.java`; include Lua
exports from the same pinned loader/facades. Add source rows for:

```python
SELECT_SOURCES = (
    "select/MusicSelector.java",
    "select/MusicSelectSkin.java",
    "select/SkinBar.java",
    "select/SkinDistributionGraph.java",
    "select/BarRenderer.java",
    "select/MusicSelectInputProcessor.java",
    "select/MusicSelectKeyProperty.java",
)
PROPERTY_FACTORIES = (
    "BooleanPropertyFactory.java",
    "IntegerPropertyFactory.java",
    "FloatPropertyFactory.java",
    "StringPropertyFactory.java",
    "TimerPropertyFactory.java",
    "EventFactory.java",
    "FloatWriter.java",
)
```

Parse every `select/bar/*.java` class and every factory branch whose source
tests `instanceof MusicSelector`. Record the owning method/enum constant as the
symbol. Record `SkinBar` public slot constants and every public behavior method
reached by `prepare`, `draw`, or `mousePressed`.

- [ ] **Step 4: Generate the initial surface and ledger**

Run:

```bash
python3 scripts/extract_beatoraja_music_select_skin_surface.py \
  --beatoraja-root /Users/xf/workspace/SNURhythm/beatoraja --write
```

The initial ledger maps already-shared generic rows to existing evidence and
maps type-5 rows to the exact later task in these music-select plans. It may use
`missing` only while this plan is executing; the final gate in the selector
runtime plan requires zero missing rows.

- [ ] **Step 5: Add executable evidence validation**

Mirror the existing gameplay evidence protocol: native runners accept
`--list-ledger-assertions` and emit sorted JSON IDs. The Python test rejects
duplicate IDs, absent implementation/test paths, wrong runners, placeholders,
commit drift, and source/ledger set differences.

- [ ] **Step 6: Run and commit the source contract**

Run:

```bash
python3 -m unittest tests/beatoraja_music_select_skin_ledger_tests.py tests/beatoraja_music_select_skin_ledger_evidence_tests.py -v
python3 scripts/extract_beatoraja_music_select_skin_surface.py \
  --beatoraja-root /Users/xf/workspace/SNURhythm/beatoraja --check
```

Expected: PASS.

```bash
git add CMakeLists.txt scripts/extract_beatoraja_music_select_skin_surface.py docs/skin-compat/beatoraja-music-select-source-surface-v1.json docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json tests/beatoraja_music_select_skin_ledger_tests.py tests/beatoraja_music_select_skin_ledger_evidence_tests.py tests/music_select_skin_ledger_evidence.h
git commit -m "test: pin Beatoraja music select surface"
```

### Task 2: Type-5 target, settings item, and acquisition

**Files:**
- Modify: `src/skin/SkinTargetTraits.h`
- Modify: `src/skin/SkinProfileSettings.h`
- Modify: `src/skin/SkinProfileSettings.cpp`
- Modify: `src/skin/package/SkinPackageStore.cpp`
- Modify: `src/scene/SettingsSceneSkins.cpp`
- Modify: `src/skin/GameplaySkinLifecycle.cpp`
- Modify: `tests/gameplay_skin_traits_tests.cpp`
- Modify: `tests/gameplay_skin_lifecycle_tests.cpp`
- Modify: `tests/gameplay_skin_settings_tests.cpp`
- Modify: `docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json`

**Interfaces:**
- Consumes: existing type-keyed `selectedSkinEntries`.
- Produces: `SkinTargetKind::MusicSelect`, trait `{5, MusicSelect, 0,
  "Music Select"}`, and `.luaskin`-only Music Select catalog rows.

- [ ] **Step 1: Add failing target and Built-in-default tests**

```cpp
void testMusicSelectTargetIsFirstClassAndDefaultsToBuiltIn() {
  const auto target = skin::skinTargetTraitForType(5);
  require(target && target->kind == skin::SkinTargetKind::MusicSelect &&
              target->label == "Music Select",
          "Beatoraja type 5 maps to Music Select");
  skin::SkinProfileSettings settings;
  settings.sanitize();
  require(!settings.selectedSkinEntries.contains(5),
          "missing type-5 selection means Built-in");
}
```

Add lifecycle cases proving no selection returns `BuiltIn`, a ready selected
type-5 activation returns `Ready`, and a selected activation failure returns
`Failed` with the original entry and diagnostic rather than `BuiltIn`.

- [ ] **Step 2: Run the focused targets and observe type-5 rejection**

Run: `cmake --build cmake-build-debug --target gameplay_skin_traits_tests gameplay_skin_lifecycle_tests gameplay_skin_settings_tests -j 6 && ./cmake-build-debug/gameplay_skin_traits_tests`

Expected: the type-5 trait assertion fails.

- [ ] **Step 3: Add the target without a gameplay key-mode gate**

```cpp
enum class SkinTargetKind { Gameplay, MusicSelect, Result, CourseResult };

inline constexpr std::array<SkinTargetTrait, 10> kSkinTargetTraits = {{
    {0, SkinTargetKind::Gameplay, 7, "7K"},
    {1, SkinTargetKind::Gameplay, 5, "5K"},
    {2, SkinTargetKind::Gameplay, 14, "14K"},
    {3, SkinTargetKind::Gameplay, 10, "10K"},
    {4, SkinTargetKind::Gameplay, 9, "9K"},
    {5, SkinTargetKind::MusicSelect, 0, "Music Select"},
    {7, SkinTargetKind::Result, 0, "Result"},
    {15, SkinTargetKind::CourseResult, 0, "Course Result"},
    {16, SkinTargetKind::Gameplay, 24, "24K"},
    {17, SkinTargetKind::Gameplay, 48, "24K Double"},
}};
```

Update stale profile comments from gameplay-only to type-keyed screen targets.

- [ ] **Step 4: Filter only the type-5 settings list by Lua source format**

Keep package scanning/source metadata unchanged. In the Music Select trait's
settings choices, include Built-in and catalog entries whose metadata type is
5 and whose entry path resolves to `GameplaySkinSourceFormat::Lua`. Do not mark
type-5 JSON/LR2 files invalid and do not expose them as Music Select choices.

- [ ] **Step 5: Generalize acquisition diagnostics and run tests**

Change target-neutral lifecycle messages from "gameplay skin" to "selected
skin" where `acquireForSkinType` serves non-gameplay targets. Call
`acquireForSkinType(5, false)` in tests so acquiring a selector does not discard
the gameplay writer chain.

Run: `cmake --build cmake-build-debug --target gameplay_skin_traits_tests gameplay_skin_lifecycle_tests gameplay_skin_settings_tests -j 6 && ./cmake-build-debug/gameplay_skin_traits_tests && ./cmake-build-debug/gameplay_skin_lifecycle_tests && ./cmake-build-debug/gameplay_skin_settings_tests`

Expected: PASS.

- [ ] **Step 6: Commit target support**

```bash
git add docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json src/skin/SkinTargetTraits.h src/skin/SkinProfileSettings.h src/skin/SkinProfileSettings.cpp src/skin/package/SkinPackageStore.cpp src/scene/SettingsSceneSkins.cpp src/skin/GameplaySkinLifecycle.cpp tests/gameplay_skin_traits_tests.cpp tests/gameplay_skin_lifecycle_tests.cpp tests/gameplay_skin_settings_tests.cpp
git commit -m "feat: add music select skin target"
```

### Task 3: Exact type-5 Lua table decoder

**Files:**
- Modify: `src/skin/beatoraja/LuaSkinRuntime.h`
- Modify: `src/skin/beatoraja/LuaSkinRuntime.cpp`
- Modify: `src/skin/beatoraja/BeatorajaSkinModel.h`
- Modify: `src/skin/beatoraja/LuaSkinTableDecoder.h`
- Modify: `src/skin/beatoraja/LuaSkinTableDecoder.cpp`
- Modify: `src/skin/beatoraja/GameplaySkinDocumentLoader.h`
- Modify: `src/skin/beatoraja/GameplaySkinDocumentLoader.cpp`
- Create: `tests/lua_music_select_skin_decoder_tests.cpp`
- Create: `tests/fixtures/skin/music_select/songlist_contract.luaskin`
- Modify: `docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: a configured Lua return value with header type 5.
- Produces:

```cpp
struct LuaSkinMusicSelectDecodeContext {
  LuaSkinRuntime &runtime;
  SkinBuiltinBindingCatalogView builtins;
};

BeatorajaSkinModelDecodeResult decodeMusicSelect(
    const LuaValueHandle &, LuaSkinMusicSelectDecodeContext) const;

struct SkinSongListDestinationDefinition {
  std::string objectName;
  SkinDestinationBody destination;
  SkinSourceLocation source;
};

struct SkinSongListDefinition {
  std::string id;
  int center = 0;
  std::vector<int> clickable;
  std::vector<SkinSongListDestinationDefinition> listOff, listOn, text, level,
      lamp, playerLamp, rivalLamp, trophy, label;
  std::optional<SkinSongListDestinationDefinition> graph;
};

// Added as a field on BeatorajaSkinModel by this task.
std::optional<SkinSongListDefinition> songListDefinition;
```

`LuaRuntimePurpose::MusicSelect` is a live/render-capable purpose and uses the
same pinned Lua host surface as the current live gameplay purpose.

- [ ] **Step 1: Write a failing two-pass decoder test**

The fixture returns type 5 and changes its title according to exported
`skin_config`. Its songlist includes every field, an authored center of
`100000`, clickable values outside the 60 rendered slots, and arrays longer
than the fixed SkinBar setter slots. Assert all authored values survive decode;
these values catch accidental AsoBMaShow clamps or array-size rejection.

```cpp
const auto decoded = decoder.decodeMusicSelect(result.value(),
    {.runtime = *runtime, .builtins = runtime->bindingCatalog()});
require(decoded.model && decoded.model->header.type == 5,
        "configured type-5 Lua table decodes");
require(decoded.model->songListDefinition &&
            decoded.model->songListDefinition->center == 100000,
        "type-5 center is not clamped by AsoBMaShow");
```

- [ ] **Step 2: Run the target and observe missing decodeMusicSelect**

Run: `cmake --build cmake-build-debug --target lua_music_select_skin_decoder_tests -j 6`

Expected: compilation fails because the type-5 decode API/model is absent.

- [ ] **Step 3: Add a live MusicSelect runtime purpose**

Every runtime branch currently meaning "configured live presentation" must
accept both `Gameplay` and `MusicSelect`. Catalog and Validation behavior stays
unchanged. Add tests that type-5 render callbacks, event executor binding,
`main_state`, timer utilities, and the legacy facade are available in
MusicSelect purpose.

- [ ] **Step 4: Decode the pinned `JsonSkin.SongList` schema without new guards**

Use the existing binding/reference machinery for generic objects. Add a
target-specific songlist pass with fields exactly:

```cpp
constexpr std::array<std::string_view, 12> kSongListFields = {
    "id", "center", "clickable", "listoff", "liston", "text",
    "level", "lamp", "playerlamp", "rivallamp", "trophy", "label"};
// `graph` is the nullable thirteenth field.
```

Missing arrays become empty and missing graph remains absent, matching field
initializers in `JsonSkin.SongList`. Do not use
`LuaSkinTableDecoderPolicy::maxDecodedObjects`,
`maxGameplayDimension`, `maxGameplayOffsets`, or the gameplay text budget for
type-5 admission. Runtime allocation failures remain runtime diagnostics.

- [ ] **Step 5: Dispatch the document loader by target type**

After header/configuration reconciliation, type 5 calls
`decodeMusicSelect`; gameplay/result targets retain their current decoder.
Require only the header/type conditions performed by the pinned path. Do not
run `GameplaySkinValidator`'s key-mode or gameplay-only object rejection over
the type-5 model.

- [ ] **Step 6: Run decoder/runtime tests and commit**

Run: `cmake --build cmake-build-debug --target lua_music_select_skin_decoder_tests lua_skin_runtime_tests lua_skin_table_decoder_tests -j 6 && ./cmake-build-debug/lua_music_select_skin_decoder_tests && ./cmake-build-debug/lua_skin_runtime_tests && ./cmake-build-debug/lua_skin_table_decoder_tests`

Expected: PASS.

```bash
git add CMakeLists.txt docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json src/skin/beatoraja/BeatorajaSkinModel.h src/skin/beatoraja/LuaSkinRuntime.h src/skin/beatoraja/LuaSkinRuntime.cpp src/skin/beatoraja/LuaSkinTableDecoder.h src/skin/beatoraja/LuaSkinTableDecoder.cpp src/skin/beatoraja/GameplaySkinDocumentLoader.h src/skin/beatoraja/GameplaySkinDocumentLoader.cpp tests/lua_music_select_skin_decoder_tests.cpp tests/fixtures/skin/music_select/songlist_contract.luaskin
git commit -m "feat: decode Beatoraja music select Lua tables"
```

### Task 4: Canonical song-list object and exact resolution

**Files:**
- Modify: `src/skin/beatoraja/BeatorajaSkinModel.h`
- Create: `src/skin/beatoraja/MusicSelectSkinModelResolver.h`
- Create: `src/skin/beatoraja/MusicSelectSkinModelResolver.cpp`
- Modify: `src/skin/beatoraja/LuaSkinTableDecoder.cpp`
- Create: `tests/music_select_skin_model_tests.cpp`
- Modify: `docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: decoded generic definitions and authored SongList destinations.
- Produces:

```cpp
struct SkinSongListPresentation {
  SkinObjectId object = 0;
  SkinDestinationBody destination;
  SkinSourceLocation source;
};

struct SkinSongListObject {
  int center = 0;
  std::vector<int> clickable;
  std::vector<SkinSongListPresentation> listOff;
  std::vector<SkinSongListPresentation> listOn;
  std::vector<SkinSongListPresentation> text;
  std::vector<SkinSongListPresentation> level;
  std::vector<SkinSongListPresentation> lamp;
  std::vector<SkinSongListPresentation> playerLamp;
  std::vector<SkinSongListPresentation> rivalLamp;
  std::vector<SkinSongListPresentation> trophy;
  std::vector<SkinSongListPresentation> label;
  std::optional<SkinSongListPresentation> graph;
};
```

- [ ] **Step 1: Add failing resolution tests from literal source behavior**

Cover liston imageset lookup with matching listoff destination, first matching
image/text/value/graph definition, missing texture/object behavior, negative
graph type `-1` versus every other negative type, and out-of-slot destinations
remaining decoded but not installed in a fixed renderer slot.

- [ ] **Step 2: Run and observe the absent song-list payload failure**

Run: `cmake --build cmake-build-debug --target music_select_skin_model_tests -j 6`

Expected: compilation fails because `SkinSongListObject` is absent.

- [ ] **Step 3: Add the song-list payload to the common model**

Add `SkinSongListObject` to `SkinObjectPayload`. Resolve from the raw
`songListDefinition` added in Task 3 and retain every destination and source
location in authored order. Center/clickable remain the authored values on the
resolved object; do not introduce a second metadata copy.

- [ ] **Step 4: Resolve only as `JsonSelectSkinObjectLoader` does**

For list on/off, resolve only `ImageSet` definitions and build their ordered
image states using the first available timer/cycle rule. Resolve lamp/player
lamp/rival lamp/trophy/label from Image, text from Text, level from Value, and
graph from a matching negative Graph. Apply fixed-slot installation later;
the resolver must not reject extra entries.

- [ ] **Step 5: Keep type-5 model admission separate from gameplay validation**

The resolver performs reference construction and the same nested object's
own `validate()` outcome as pinned Beatoraja. It does not apply safe-coordinate
bounds, object-count caps, required-destination checks, or gameplay-only
invalid-object rejection.

- [ ] **Step 6: Run model/decoder regressions and commit**

Run: `cmake --build cmake-build-debug --target music_select_skin_model_tests lua_music_select_skin_decoder_tests beatoraja_skin_model_tests -j 6 && ./cmake-build-debug/music_select_skin_model_tests && ./cmake-build-debug/lua_music_select_skin_decoder_tests && ./cmake-build-debug/beatoraja_skin_model_tests`

Expected: PASS.

```bash
git add CMakeLists.txt docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json src/skin/CMakeLists.txt src/skin/beatoraja/BeatorajaSkinModel.h src/skin/beatoraja/MusicSelectSkinModelResolver.h src/skin/beatoraja/MusicSelectSkinModelResolver.cpp src/skin/beatoraja/LuaSkinTableDecoder.cpp tests/music_select_skin_model_tests.cpp
git commit -m "feat: model Beatoraja song lists"
```

### Task 5: SkinBar rendering and pointer behavior

**Files:**
- Create: `src/skin/beatoraja/MusicSelectBarRenderer.h`
- Create: `src/skin/beatoraja/MusicSelectBarRenderer.cpp`
- Create: `src/music_select/MusicSelectTypes.h`
- Modify: `src/skin/beatoraja/Skin2DRenderer.h`
- Modify: `src/skin/beatoraja/Skin2DRenderer.cpp`
- Create: `tests/music_select_bar_renderer_tests.cpp`
- Modify: `docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `SkinSongListObject`, prepared resources, frame time, and
  `MusicSelectBarFrame` rows.
- Produces:

```cpp
enum class MusicSelectBarKind {
  Song, Folder, Table, Hash, Executable, Grade, RandomCourse,
  Command, Container, SearchWord, SameFolder
};

struct MusicSelectBarFrame {
  MusicSelectBarKind kind = MusicSelectBarKind::Song;
  std::string title;
  bool exists = false;
  std::int64_t addDateSeconds = 0;
  int lamp = 0;
  int rivalLamp = 0;
  int difficulty = 0;
  int level = 0;
  int featureFlags = 0;
  std::string trophyName;
  std::array<int, 11> folderLampCounts{};
};

struct MusicSelectSongListFrame {
  std::span<const MusicSelectBarFrame> bars;
  std::size_t selectedIndex = 0;
  std::int64_t wallClockSeconds = 0;
  bool rivalSelected = false;
  int movementDirection = 0;
  std::int64_t movementEndMillis = 0;
};
```

- [ ] **Step 1: Write failing draw-command tests for all slot families**

Use literal rows for each bar kind and assert draw order: bar images, folder
graphs, titles, trophies, lamps, levels, then labels. Assert the exact 11 text
fallback slots, 3 trophies, 7 level slots, 5 labels, 11 lamps, and 60 bar rows.
Test newness at exactly `addDate + 24h` and label precedence for undefined LN,
LN/CN/HCN, mines, and random. Assert the source `value == -1` no-draw result
for `SameFolderBar`, which has no explicit `BarRenderer` type branch.

- [ ] **Step 2: Run the target and observe missing renderer symbols**

Run: `cmake --build cmake-build-debug --target music_select_bar_renderer_tests -j 6`

Expected: compilation fails because the specialized renderer is absent.

- [ ] **Step 3: Port `SkinBar.prepare` and `BarRenderer.prepare/render` ordering**

Use common destination evaluation and prepared resources for each nested
object. Keep the source's movement interpolation and row wrapping. Fixed-slot
access returns no object for an out-of-range authored entry; it does not reject
the model.

- [ ] **Step 4: Implement exact clickable hit testing**

```cpp
struct MusicSelectBarPointerResult {
  bool consumed = false;
  std::optional<std::size_t> selectIndex;
  bool closeDirectory = false;
};
```

Loop authored `clickable` values in order, derive wrapped row index relative to
`center`, use the evaluated on/off image destination as the hit rectangle, and
return select for pointer button 0 or close-directory for every other button.

- [ ] **Step 5: Run renderer and common draw regressions**

Run: `cmake --build cmake-build-debug --target music_select_bar_renderer_tests skin_draw_command_tests -j 6 && ./cmake-build-debug/music_select_bar_renderer_tests && ./cmake-build-debug/skin_draw_command_tests`

Expected: PASS.

- [ ] **Step 6: Commit rendering**

```bash
git add CMakeLists.txt docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json src/music_select/MusicSelectTypes.h src/skin/CMakeLists.txt src/skin/beatoraja/MusicSelectBarRenderer.h src/skin/beatoraja/MusicSelectBarRenderer.cpp src/skin/beatoraja/Skin2DRenderer.h src/skin/beatoraja/Skin2DRenderer.cpp tests/music_select_bar_renderer_tests.cpp
git commit -m "feat: render Beatoraja music select bars"
```

### Task 6: Music-select state bridge and owning session

**Files:**
- Create: `src/skin/beatoraja/MusicSelectSkinStateBridge.h`
- Create: `src/skin/beatoraja/MusicSelectSkinStateBridge.cpp`
- Create: `src/skin/beatoraja/MusicSelectSkinSession.h`
- Create: `src/skin/beatoraja/MusicSelectSkinSession.cpp`
- Create: `tests/music_select_skin_state_bridge_tests.cpp`
- Create: `tests/music_select_skin_session_tests.cpp`
- Modify: `docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: an immutable selector frame plus an action sink supplied by the
  controller plan.
- Produces:

```cpp
struct MusicSelectPropertyValues {
  std::map<int, bool> booleans;
  std::map<int, std::int64_t> integers;
  std::map<int, double> floats;
  std::map<int, std::string> strings;
  std::map<int, std::int64_t> timers;
};

struct MusicSelectSkinFrame {
  std::uint64_t serial = 0;
  std::int64_t elapsedMillis = 0;
  MusicSelectPropertyValues properties;
  MusicSelectSongListFrame songList;
};

struct MusicSelectSkinActionSink {
  std::function<void(int, std::span<const int>)> event;
  std::function<void(int, double)> floatWriter;
  std::function<void(int, std::string_view)> stringWriter;
};
```

`MusicSelectSkinSession::create(ValidatedSkinActivation,
MusicSelectSkinSessionContext)` owns the revision lease, configured Lua
runtime, model, resources, movies, bridge, renderer, and bar renderer.

- [ ] **Step 1: Add failing bridge lookup and action-order tests**

Use hand-written literal maps. Assert numeric and named built-in selectors,
negative boolean selectors, missing Beatoraja sentinel values, custom Lua
bindings, custom timers/events, and ordered event/writer delivery to the sink.

- [ ] **Step 2: Run and observe missing bridge/session failures**

Run: `cmake --build cmake-build-debug --target music_select_skin_state_bridge_tests music_select_skin_session_tests -j 6`

Expected: compilation fails because both types are absent.

- [ ] **Step 3: Implement the bridge over one immutable frame**

The bridge implements `ISkinFrameState`. It resolves the source selector IDs
from frame maps and delegates custom Lua bindings through the retained runtime.
It stages actions while evaluating and publishes them only after successful
frame submission. It returns Beatoraja's per-property absent value; it does not
replace missing rows with a common zero.

- [ ] **Step 4: Implement exact two-pass session creation**

Create the resource and write-capable Lua filesystems from the activation
revision. Run configured Lua with `LuaRuntimePurpose::MusicSelect` and an
initial frame bridge. Require configured header type 5. Plan/upload common
resources, prepare movies, enter render phase, and retain the activation lease
until session destruction.

- [ ] **Step 5: Integrate generic and song-list rendering transactionally**

`render(RenderContext &, const MusicSelectSkinFrame &)` updates custom
objects, evaluates generic destinations, evaluates/draws the song list at its
authored object position, submits the frame, and only then releases staged
actions through `MusicSelectSkinActionSink`. Return structured diagnostics on
failure and retain no fallback renderer.

- [ ] **Step 6: Run focused session regressions and commit**

Run: `cmake --build cmake-build-debug --target music_select_skin_state_bridge_tests music_select_skin_session_tests play_skin_session_tests -j 6 && ./cmake-build-debug/music_select_skin_state_bridge_tests && ./cmake-build-debug/music_select_skin_session_tests && ./cmake-build-debug/play_skin_session_tests`

Expected: PASS.

```bash
git add CMakeLists.txt docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json src/skin/CMakeLists.txt src/skin/beatoraja/MusicSelectSkinStateBridge.h src/skin/beatoraja/MusicSelectSkinStateBridge.cpp src/skin/beatoraja/MusicSelectSkinSession.h src/skin/beatoraja/MusicSelectSkinSession.cpp tests/music_select_skin_state_bridge_tests.cpp tests/music_select_skin_session_tests.cpp
git commit -m "feat: add music select skin session"
```

### Task 7: Compatibility-core verification

**Files:**
- Modify only source/test/ledger files from Tasks 1-6 when a focused failure proves a
  compatibility-core defect.

**Interfaces:**
- Consumes: completed type-5 loader/model/renderer/session.
- Produces: all core ledger rows bound to executable evidence; controller rows
  remain assigned to the next ordered plan.

- [ ] **Step 1: Refresh the ledger from pinned source**

Run:

```bash
python3 scripts/extract_beatoraja_music_select_skin_surface.py \
  --beatoraja-root /Users/xf/workspace/SNURhythm/beatoraja --write
```

Classify decoder/model/renderer/session rows as implemented only when their
named native runner emits the row ID. Keep selector-controller rows assigned
to their exact task in
`docs/superpowers/plans/2026-09-01-beatoraja-music-select-runtime.md`.

- [ ] **Step 2: Run the source and evidence gates**

Run:

```bash
python3 scripts/extract_beatoraja_music_select_skin_surface.py \
  --beatoraja-root /Users/xf/workspace/SNURhythm/beatoraja --check
python3 -m unittest tests/beatoraja_music_select_skin_ledger_tests.py tests/beatoraja_music_select_skin_ledger_evidence_tests.py -v
```

Expected: PASS with no unclassified row and no wrongly claimed core evidence.

- [ ] **Step 3: Run all focused type-5 tests**

Run: `ctest --test-dir cmake-build-debug --output-on-failure -R 'music_select|lua_skin_runtime|beatoraja_skin_model' -j 6`

Expected: all matching tests pass.

- [ ] **Step 4: Check ledger closure and diff hygiene for this phase**

Run: `git diff --check`

Expected: no output.

If a row is wrong, return to its owning task's evidence test and update that
task's production/test/ledger slice together. Do not create a ledger-only
cleanup commit.
