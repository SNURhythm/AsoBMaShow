# Feature documentation

Each page below is a current guide to one durable product capability. The pages
answer: why the capability exists, how a user reaches it, where its code lives,
which data/lifecycle rules matter, and where to start testing it.

| Feature | Main code |
| --- | --- |
| [Library and chart scanning](library-and-chart-scanning.md) | `src/repositories/`, chart-scanning services, library scenes |
| [Gameplay and scoring](gameplay-and-scoring.md) | `src/scene/play/`, gameplay model and renderer |
| [Courses](courses.md) | course identity, continuation, result and replay services |
| [Gameplay skins](gameplay-skins.md) | `src/skin/`, `src/skin/beatoraja/`, playfield presentation |
| [Replays and video export](replays-and-video-export.md) | `src/replay/`, replay exporters and repositories |
| [Practice and analysis](practice-and-analysis.md) | `src/practice/`, gameplay and result/practice scenes |
| [Results, records, and persistence](results-records-and-persistence.md) | result models, scenes, and repositories |
| [Internet Ranking](internet-ranking.md) | `src/ir/` |
| [Profiles and data transfer](profiles-and-data-transfer.md) | profile services, settings, and document handoff |
| [Input and controllers](input-and-controllers.md) | `src/input/` |
| [Audio, video, and display](audio-video-and-display.md) | `src/audio/`, `src/video/`, rendering |
| [Find BMS and downloads](find-bms-and-downloads.md) | `src/bms_search/` |
| [Settings and user interface](settings-and-user-interface.md) | settings scenes and `src/view/` |
| [Mobile and platform integration](mobile-and-platform-integration.md) | Android/iOS bridges and document handoff |
| [Build, release, and verification](build-release-and-verification.md) | CMake, scripts, CI, and release metadata |

For repository-wide orientation, begin with the [repository guide](../repository-guide.md).
