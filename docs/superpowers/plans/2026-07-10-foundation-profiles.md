# Player Profiles and Portable Data Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate the current single-player data into isolated, switchable profiles with versioned settings and safe ZIP export/import while keeping the chart library shared.

**Architecture:** A small bootstrap resolves the active profile UUID; each profile owns settings/input/score/replay files under a fixed directory layout. First-run migration and import use staged directories, SQLite backup/integrity checks, checksums, and atomic rename so the current data remains recoverable after any failure.

**Tech Stack:** C++23, nlohmann/json, SQLite backup API, libarchive ZIP writer/reader, bundled SHA-256 implementation, SDL platform bridges, CMake/CTest.

## Global Constraints

- Work only on task branches/worktrees from `feature/player-foundations`; never edit or commit on `develop`.
- The chart/library database, difficulty tables, chart roots, downloads, and chart assets remain shared.
- Exact profile layout is `profiles/<uuid>/{profile.json,settings.json,input.json,scores.db,replays.db}` plus root `active-profile.json`.
- Existing `settings.cfg`, `db/score.db`, and `db/replay.db` are retained after migration and never deleted by this milestone.
- Unknown future JSON/DB/archive versions fail closed without modifying working data.
- Profile operations are blocked during gameplay, chart scanning/import, replay export, profile archive work, or score/replay writes.
- Export contains exactly six allowlisted members and never contains chart assets, downloads, paths, credentials, or IR data.
- New executables use CTest names prefixed `foundation_profile_`; new `src` files must be in iOS membership exceptions.
- This plan consumes `AudioVideoSettings` from the audio/video plan, `InputProfileStore` from the input plan, and score/replay schemas from the provenance plan.

---

### Task 1: Atomic files and versioned application settings

**Files:**
- Create: `src/AtomicFile.h`
- Create: `src/AtomicFile.cpp`
- Create: `src/VersionedJson.h`
- Create: `src/VersionedJson.cpp`
- Create: `src/AppSettingsStore.h`
- Create: `src/AppSettingsStore.cpp`
- Modify: `src/AppSettings.h`
- Modify: `src/AppSettings.cpp`
- Create: `tests/app_settings_store_tests.cpp`
- Create: `tests/fixtures/settings/legacy-full.cfg`
- Create: `tests/fixtures/settings/settings-v0.json`
- Create: `tests/fixtures/settings/settings-v1.json`
- Create: `tests/fixtures/settings/settings-future.json`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: every current `AppSettings` field plus `audioVideo` from the audio/video contract.
- Produces: reusable atomic/JSON migration primitives and path-injected settings load/save.

- [ ] **Step 1: Write failing fixture and round-trip tests**

Tests load the complete legacy fixture, assert every current field, save JSON v1, reload it, and compare equality including audio/video intent. Add v0→v1 idempotency, invalid-value diagnostics/sanitization, malformed/root-array no-rewrite, future-version no-rewrite, and injected atomic replacement failure preserving the prior file and backup.

- [ ] **Step 2: Register and verify red**

Create `app_settings_store_tests`, register `foundation_profile_settings`, and confirm missing-header compilation failure.

- [ ] **Step 3: Implement atomic and versioned JSON primitives**

```cpp
namespace atomic_file {
struct Operations {
  std::function<bool(const std::filesystem::path &, std::span<const std::byte>, std::string &)> writeAndSync;
  std::function<bool(const std::filesystem::path &, const std::filesystem::path &, std::string &)> replace;
  std::function<void(const std::filesystem::path &)> remove;
};
bool writeWithBackup(const std::filesystem::path &, std::span<const std::byte>,
                     std::string &, const Operations *operations=nullptr);
}
namespace versioned_json {
enum class LoadStatus { Loaded, Missing, IoError, Malformed, InvalidRoot,
  FutureVersion, MigrationFailed };
using Migration = std::function<bool(nlohmann::json &, std::string &)>;
struct LoadResult { LoadStatus status=LoadStatus::Missing; nlohmann::json document;
  std::vector<std::string> diagnostics; };
LoadResult loadAndMigrate(const std::filesystem::path &, int currentVersion,
                          std::span<const Migration>);
bool saveAtomic(const std::filesystem::path &, const nlohmann::json &,
                std::string &, const atomic_file::Operations *operations=nullptr);
}
```

Real writes create a sibling `.tmp`, flush and fsync/`FlushFileBuffers`, rotate one valid `.bak`, and atomically replace. Failure injection is used only by tests.

- [ ] **Step 4: Implement `AppSettingsStore` and remove fixed paths**

```cpp
enum class AppSettingsLoadStatus { Loaded, Missing, Invalid, FutureVersion };
struct AppSettingsLoadResult { AppSettings settings; AppSettingsLoadStatus status;
  std::vector<std::string> diagnostics; };
class AppSettingsStore {
public:
  static constexpr int kCurrentSchemaVersion = 1;
  static AppSettingsLoadResult Load(const std::filesystem::path &settingsJson);
  static AppSettingsLoadResult LoadLegacyCfg(const std::filesystem::path &settingsCfg);
  static bool Save(const std::filesystem::path &settingsJson,
                   const AppSettings &, std::string &errorMessage);
};
```

JSON v1 contains every current field and nested `audio`/`video` objects. A missing version is v0 and migrates sequentially. Keep the existing fixed-path `AppSettings::load/save/configPath` wrappers temporarily so this commit still builds before profile bootstrapping lands; move reusable parsing/sanitization into the store without changing those wrappers' legacy behavior. Task 3 removes the wrappers after every caller is profile-aware.

- [ ] **Step 5: Verify and commit**

```bash
cmake --build cmake-build-debug --target app_settings_store_tests -j 6
ctest --test-dir cmake-build-debug -R '^foundation_profile_settings$' --output-on-failure
git add src/AtomicFile.* src/VersionedJson.* src/AppSettings* tests/fixtures/settings tests/app_settings_store_tests.cpp src/CMakeLists.txt CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat(profile): add versioned atomic settings"
```

---

### Task 2: Profile model and lossless first-run migration

**Files:**
- Create: `src/PlayerProfile.h`
- Create: `src/PlayerProfileManager.h`
- Create: `src/PlayerProfileManager.cpp`
- Create: `src/ProfileDatabaseTools.h`
- Create: `src/ProfileDatabaseTools.cpp`
- Create: `tests/player_profile_manager_tests.cpp`
- Create: `tests/fixtures/profiles/legacy-settings.cfg`
- Modify: `src/AppDatabaseInitializer.h`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: Task 1 settings, input profile defaults/store, provenance DB `EnsureSchema()` APIs.
- Produces: profile CRUD, stable paths, staged migration, and active bootstrap.

- [ ] **Step 1: Write profile/migration tests first**

Create real temporary legacy settings/SQLite files. Assert exact settings conversion; score/course/replay/event row counts; WAL-backed writes included by backup; source files retained; second initialization idempotent; orphan finalized-profile recovery; every injected migration-phase failure cleaning staging and leaving bootstrap/source untouched; and create/rename/duplicate/delete constraints.

- [ ] **Step 2: Define model and manager API**

```cpp
inline constexpr int kPlayerProfileSchemaVersion=1;
inline constexpr int kActiveProfileSchemaVersion=1;
struct PlayerProfile { int schemaVersion=kPlayerProfileSchemaVersion; std::string id;
  std::string displayName; std::string createdAt; std::string lastUsedAt; };
struct PlayerProfilePaths { std::filesystem::path root, profileJson, settingsJson,
  inputJson, scoresDb, replaysDb; };
enum class ProfileError { None, InvalidName, NotFound, ActiveProfileDeletion,
  LastProfileDeletion, FutureVersion, IoFailure, MigrationFailure,
  IntegrityFailure, SwitchBlocked };
struct ProfileResult { ProfileError error=ProfileError::None; std::string message;
  std::optional<PlayerProfile> profile; bool ok() const; };
struct PlayerProfileManagerDependencies {
  std::function<std::string()> generateUuid;
  std::function<std::string()> utcNow;
  std::function<bool(const std::filesystem::path &, const std::filesystem::path &,
                     std::string &)> snapshotDatabase;
};
class PlayerProfileManager {
public:
  explicit PlayerProfileManager(std::filesystem::path applicationDataRoot,
      PlayerProfileManagerDependencies dependencies={});
  ProfileResult Initialize();
  const PlayerProfile &activeProfile() const;
  PlayerProfilePaths activePaths() const;
  PlayerProfilePaths pathsFor(std::string_view id) const;
  std::vector<PlayerProfile> listProfiles() const;
  ProfileResult createProfile(std::string displayName);
  ProfileResult duplicateProfile(std::string_view sourceId, std::string displayName);
  ProfileResult renameProfile(std::string_view id, std::string displayName);
  ProfileResult deleteProfile(std::string_view id);
  ProfileResult validateProfile(std::string_view id) const;
  ProfileResult commitActiveProfile(std::string_view id);
};
```

- [ ] **Step 3: Implement SQLite validation tools**

```cpp
bool snapshotSqliteDatabase(const std::filesystem::path &source,
                            const std::filesystem::path &destination,
                            std::string &errorMessage);
bool sqliteIntegrityCheck(const std::filesystem::path &, std::string &);
std::optional<std::int64_t> sqliteTableRowCount(
    const std::filesystem::path &, std::string_view table, std::string &);
```

Use `sqlite3_backup`, not file copy, so WAL data is included.

- [ ] **Step 4: Implement the exact staged migration**

Read/recover bootstrap, remove only `.staging-*`, create staging UUID, convert legacy settings, write default input, snapshot or initialize both DBs, migrate schemas, run integrity checks, compare all score/course/replay/event row counts, write profile metadata, rename staging to final, then atomically write bootstrap last. On failure return legacy fallback and remove only staging.

- [ ] **Step 5: Verify and commit**

```bash
cmake --build cmake-build-debug --target player_profile_manager_tests -j 6
ctest --test-dir cmake-build-debug -R '^foundation_profile_manager$' --output-on-failure
git add src/PlayerProfile* src/ProfileDatabaseTools* src/AppDatabaseInitializer.h tests/player_profile_manager_tests.cpp tests/fixtures/profiles CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat(profile): migrate legacy data into profiles"
```

---

### Task 3: Profile-scoped database binding and transactional switching

**Files:**
- Create: `src/ProfileSessionCoordinator.h`
- Create: `src/ProfileSessionCoordinator.cpp`
- Create: `tests/profile_switch_tests.cpp`
- Modify: `src/context.h`
- Modify: `src/main.cpp`
- Modify: `src/AppSettings.h`
- Modify: `src/AppSettings.cpp`
- Modify: `src/ScoreDBHelper.h`
- Modify: `src/ScoreDBHelper.cpp`
- Modify: `src/ReplayDBHelper.h`
- Modify: `src/ReplayDBHelper.cpp`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `src/scene/SettingsSceneControls.cpp`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/ChartViewerScene.cpp`
- Modify: `src/scene/MusicPlayerScene.cpp`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: manager/profile paths, path-bindable score/replay helpers, settings/input stores.
- Produces: atomic active-profile switches and context-owned save/switch APIs.

- [ ] **Step 1: Write switch transaction tests first**

Use two temporary profiles with distinct settings/scores/replays. Assert helper paths/query results change, ChartDB remains shared, score revision/cache callback changes, each declared blocker rejects, and settings/DB/bootstrap failures restore previous helper paths/settings/input and refresh caches again.

- [ ] **Step 2: Implement coordinator**

```cpp
struct ProfileSwitchResult { ProfileError error=ProfileError::None;
  std::string message; bool ok() const; };
class ProfileSessionCoordinator {
public:
  using Blocker=std::function<std::optional<std::string>()>;
  using ApplyInput=std::function<bool(const std::filesystem::path &, std::string &)>;
  using RestoreInput=std::function<void(const std::filesystem::path &)>;
  using RefreshCaches=std::function<void()>;
  ProfileSessionCoordinator(PlayerProfileManager &, ScoreDBHelper &, ReplayDBHelper &,
      Blocker, ApplyInput, RestoreInput, RefreshCaches);
  ProfileSwitchResult switchTo(std::string_view profileId, AppSettings &currentSettings);
};
```

Validate target metadata/JSON/DB versions and integrity before rebinding. Load temporary settings/input, save old state, bind/apply/refresh, write bootstrap last, and restore all old state plus caches on failure.

- [ ] **Step 3: Integrate application context and saves**

Add profile manager/coordinator to `ApplicationContext`, plus `saveSettings(std::string*)` and `switchProfile(...)`. Initialize the manager before application databases, then bind score/replay paths. Replace every direct `context.settings.save()` in Settings, Gameplay, ChartViewer, MainMenu, and MusicPlayer with `context.saveSettings()`. Replace startup `AppSettings::load()` with `AppSettingsStore::Load(activePaths.settingsJson)` and then remove the fixed-path `AppSettings::load/save/configPath` wrappers.

- [ ] **Step 4: Verify and commit**

```bash
cmake --build cmake-build-debug --target profile_switch_tests main -j 6
ctest --test-dir cmake-build-debug -R '^foundation_profile_switch$' --output-on-failure
git add src/ProfileSessionCoordinator.* src/context.h src/main.cpp src/ScoreDBHelper.* src/ReplayDBHelper.* src/scene tests/profile_switch_tests.cpp CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat(profile): isolate and switch active profile state"
```

---

### Task 4: Checksummed ZIP export and import

**Files:**
- Modify: `src/ArchiveRAII.h`
- Create: `src/FileChecksum.h`
- Create: `src/FileChecksum.cpp`
- Create: `src/ProfileArchive.h`
- Create: `src/ProfileArchive.cpp`
- Create: `tests/profile_archive_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: manager validation/paths, SQLite snapshots, settings/input validators.
- Produces: format-v1 portable archive with atomic create/import/overwrite.

- [ ] **Step 1: Write archive security and round-trip tests first**

Cover exact settings/input/score/replay/provenance round trip, UUID collision generating a new ID, explicit overwrite, injected replacement rollback, traversal, duplicate member, absolute path, symlink/hardlink, checksum, future component version, oversized metadata, invalid UTF-8, corrupt SQLite, and export failure leaving no destination/temp.

- [ ] **Step 2: Implement public archive types**

```cpp
struct ProfileArchiveManifest {
  static constexpr int kFormatVersion=1;
  int formatVersion=kFormatVersion; std::string sourceApplicationVersion;
  std::string profileUuid, profileDisplayName, createdAt;
  int profileSchemaVersion=1, settingsSchemaVersion=1, inputSchemaVersion=1;
  int scoreSchemaVersion=5, replaySchemaVersion=3;
};
enum class ProfileImportMode { CreateWithNewId, Overwrite };
struct ProfileImportOptions { ProfileImportMode mode=ProfileImportMode::CreateWithNewId;
  std::optional<std::string> overwriteProfileId; };
struct ProfileArchiveResult { ProfileError error=ProfileError::None; std::string message;
  std::optional<PlayerProfile> profile; bool ok() const; };
class ProfileArchiveService {
public:
  explicit ProfileArchiveService(PlayerProfileManager &);
  ProfileArchiveResult Export(std::string_view, const std::filesystem::path &);
  ProfileArchiveResult Import(const std::filesystem::path &,
                              const ProfileImportOptions &options={});
};
```

- [ ] **Step 3: Implement deterministic export**

Snapshot DBs, stream SHA-256 for `manifest.json`, `settings.json`, `input.json`, `scores.db`, and `replays.db`, write deterministic `checksums.sha256`, create a sibling temporary ZIP with libarchive, reopen/verify it, then atomically rename it.

- [ ] **Step 4: Implement allowlisted staged import**

Accept only the six exact members. Reject unsafe types/names, metadata above 1 MiB, either DB above 2 GiB, total expansion above 4 GiB, mismatched checksum, future version, invalid JSON/input, or failed DB integrity before touching profiles. Collision defaults to a new UUID. Overwrite stages, backs up the old inactive profile, installs, and restores on failure.

- [ ] **Step 5: Verify and commit**

```bash
cmake --build cmake-build-debug --target profile_archive_tests -j 6
ctest --test-dir cmake-build-debug -R '^foundation_profile_archive$' --output-on-failure
git add src/ArchiveRAII.h src/FileChecksum.* src/ProfileArchive.* tests/profile_archive_tests.cpp src/CMakeLists.txt CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat(profile): add portable profile archives"
```

---

### Task 5: Profile settings UI and platform file handoff

**Files:**
- Create: `src/scene/ProfileSettingsController.h`
- Create: `src/scene/ProfileSettingsController.cpp`
- Create: `tests/profile_settings_controller_tests.cpp`
- Modify: `src/scene/SettingsScene.h`
- Modify: `src/scene/SettingsScene.cpp`
- Modify: `src/scene/SettingsSceneLayout.cpp`
- Modify: `src/scene/SettingsSceneControls.cpp`
- Modify: `src/AndroidNatives.h`
- Modify: `src/AndroidNatives.cpp`
- Modify: `android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowActivity.java`
- Modify: `src/iOSNatives.hpp`
- Modify: `src/iOSNatives.mm`
- Modify: `src/scene/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: manager, coordinator, archive service, and platform document pickers.
- Produces: Profile tab with safe CRUD/switch/export/import.

- [ ] **Step 1: Write controller state tests first**

Assert each service result maps to stable UI state, active/last-profile deletion stays disabled with a reason, successful activation refreshes profiles/settings, and failed switch/import/overwrite retains the previous selection and error text.

- [ ] **Step 2: Implement controller API**

```cpp
class ProfileSettingsController {
public:
  explicit ProfileSettingsController(ApplicationContext &);
  std::vector<PlayerProfile> profiles() const;
  ProfileResult create(std::string name);
  ProfileResult rename(std::string_view id, std::string name);
  ProfileResult duplicate(std::string_view id, std::string name);
  ProfileResult remove(std::string_view id);
  ProfileSwitchResult activate(std::string_view id);
  ProfileArchiveResult exportProfile(std::string_view id,
                                     const std::filesystem::path &destination);
  ProfileArchiveResult importProfile(const std::filesystem::path &,
                                     const ProfileImportOptions &);
};
```

- [ ] **Step 3: Add the Profile settings tab**

Place Profile first. Show active marker, display name, last-used time, and Create/Rename/Duplicate/Delete/Export/Import. Explain disabled delete. Run archive work on `std::jthread`, disable actions while active, and retain status/error. Successful switch rebuilds settings and reapplies theme/audio/runtime values.

- [ ] **Step 4: Add platform document handoff**

Desktop uses existing tinyfiledialogs. iOS uses `UIDocumentPickerViewController` and document interaction. Android uses `ACTION_OPEN_DOCUMENT` and `ACTION_CREATE_DOCUMENT`, copying through granted content URIs. All platform callbacks return to the main-thread state machine.

- [ ] **Step 5: Verify and commit**

```bash
cmake --build cmake-build-debug --target profile_settings_controller_tests view_layout_tests main -j 6
ctest --test-dir cmake-build-debug -R '^(foundation_profile_|view_layout_tests)' --output-on-failure
scripts/ios_firebase_deploy.sh --build-only
git add src/scene src/AndroidNatives.* src/iOSNatives.* android tests/profile_settings_controller_tests.cpp CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat(settings): manage portable player profiles"
```
