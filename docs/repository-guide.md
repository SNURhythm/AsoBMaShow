# AsoBMaShow repository guide

This is the entry point for developers and coding agents. It describes the
current system rather than the sequence of changes that produced it.

## Start here

- [Feature documentation](features/README.md) maps product features to their
  code, persistent data, lifecycle boundaries, and tests.
- [Build, release, and verification](features/build-release-and-verification.md)
  covers local development and platform distribution workflows.
- [Replay contract matrix](replay/file-replay-contract-matrix.md) and the
  [skin compatibility references](skin-compat/) remain detailed external-format
  and acceptance sources.
- [Design specifications](superpowers/specs/) retain feature requirements,
  contracts, and architectural decisions. Their dated status and validation
  notes are historical; use feature pages and code for the current implementation.
- [Focused testing](testing/README.md) maps source areas to test groups.

## Repository map

| Area | Responsibility |
| --- | --- |
| `src/` | Portable C++ application code. `main.cpp` composes application services and owns the main loop. |
| `src/scene/` | Scene-level user flows, including library, settings, results, gameplay, and playfield presentation. |
| `src/repositories/` | Context-owned SQLite repositories for chart metadata, scores, replays, scan state, and playlists. |
| `src/replay/` | Beatoraja-compatible replay format, storage, validation, capture, playback, and lifecycle services. |
| `src/skin/` | Built-in and optional Beatoraja Lua gameplay-skin support, package operations, configuration, and rendering bridges. |
| `src/audio/`, `src/video/`, `src/input/` | Platform-neutral media, display, and input domains with native adapters behind explicit interfaces. |
| `src/bms_search/` | Find BMS source drivers and downloaded-archive staging. |
| `src/practice/`, `src/ir/` | Practice/analysis and Internet Ranking domain services. |
| `tests/` | Standalone C++ behavior tests, Python artifact/script verifiers, and test fixtures. |
| `android/`, `ios/`, `Info.plist` | Mobile applications and Apple bundle metadata. |
| `scripts/`, `.github/workflows/` | Local setup/distribution helpers and CI workflows. |
| `shader_src/` | Shader sources and the compiler driver; generated artifacts are consumed by renderers. |

Vendored dependencies live in directories such as `bgfx/`, `SDL/`, `SDL_ttf/`,
`yoga/`, and `stb/`. Treat them as external code unless a task explicitly
targets their integration.

## Runtime shape

`src/main.cpp` builds the `ApplicationContext`, initializes platform and media
services, and delegates navigation to `SceneManager`. Scenes compose views and
call focused domain services. Services own application rules; repositories own
durable SQLite access; platform bridges contain OS-only behavior. The main loop
is responsible for delivering queued native work on the application thread.

```text
native callbacks / OS events
          |
          v
platform bridge -> queued portable service work -> ApplicationContext
                                                   |
                                                   v
                      SceneManager <-> scenes <-> views
                                                   |
                                                   v
      repositories / replay files / profile files / media and render services
```

Keep those directions intact: views should not own database or native handles;
repositories should not own UI workflows; native callbacks should not mutate
scene state directly.

## Working conventions

- Follow the feature page before changing a subsystem. It identifies the
  relevant source roots, durable-data boundaries, and focused tests.
- Prefer behavior tests that link and exercise the relevant code. Python tests
  may verify executable scripts or produced artifacts; avoid text-shape tests
  that freeze internal implementation details.
- The BMS parser sources are amalgamated from the sibling `bms-parser-cpp`
  repository; follow the repository instructions before changing them.
- Build metadata and mobile delivery scripts are operational interfaces. Read
  `AGENTS.md` and the build/release feature page before running a distribution
  command.

## Documentation policy

Feature pages document stable intent and current structure. Update them when a
user-visible feature, an ownership boundary, a persistent format, or an
operational workflow changes. Do not add chronological implementation plans,
temporary audit reports, or source-shape assertions as permanent architecture
records.

Keep useful specifications even after implementation. Remove completed task
plans and execution checklists once their lasting requirements are covered by
feature pages or specs; Git history retains the implementation record. Update
references when pruning documents so tools and guides point to retained sources.
