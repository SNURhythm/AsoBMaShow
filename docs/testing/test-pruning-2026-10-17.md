# Test pruning — 2026-10-17

This pass removes three source-text-only CTest suites, two repeated fixture-compiler cases, and compile-time API/member shape assertions. No production code changes.

| Removed checks | Retained coverage and limits |
| --- | --- |
| Windows Lua filesystem CMake `advapi32` string search | The actual Windows target must link successfully. A string anywhere in a CMake block did not prove correct linkage. |
| Settings unbind callback regex | `input_capture_controller_tests` exercises successful, failed, missing and ambiguous binding removal. Exact UI callback wiring is no longer pinned; this is not equivalent end-to-end UI coverage. |
| TextView batch-call / transient-allocation string search | Native rendering and view suites remain. This intentionally drops the source-level prohibition; it does not claim to measure TextView transient-buffer use. |
| Compiler subprocess mocks in preview and image-batch suites | Both real compiled fixtures remain. GNU/MSVC/clang-cl command tests in `music_select_error_flow_contract_tests.py` and real CMake launcher integration in `fixture_compiler_environment_tests.py` remain. Per-fixture subprocess counts are no longer frozen. |
| Audio member types and method signatures; two ResultSkinData member-presence checks | Audio mix/lifecycle and view runtime cases remain. Calls compile as part of those tests and the app; exact private representation and member-pointer signatures are no longer frozen. |

Windows filesystem security guards, settings stale-pointer checks, release validation, and all executable production-behavior cases are retained. The October 1 audit remains a historical record of that earlier pass.

Verification: rebuilt `audio_mix_tests`, `audio_wrapper_lifecycle_tests`, and `view_layout_tests`; all 498 remaining CTest registrations passed with `ctest --test-dir cmake-build-debug --output-on-failure -j 6` (129.54 seconds). `git diff --check` passed. This is a maintenance cleanup, not a measured performance improvement.
