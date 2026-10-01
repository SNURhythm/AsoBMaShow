# Test usefulness audit — 2026-10-01

The review scored all **409 CTest registrations** from commit `41c635c8`, plus **9 standalone platform/acceptance suites**. It removed **14 CTest entries** (409 → 395), **one standalone documentation suite**, and **21 low-value Python methods from mixed suites**. It also removed two C++ source-text-only cases, a source-check tail, redundant arithmetic, type-shape assertions, and literal layout/build metadata pins. The changes remove **1,710 net lines of test/verification/build code**, excluding the scorecard and documentation. No production application code changed and no new tests were added.

The complete [418-row scorecard](test-usefulness-scores-2026-10-01.csv) includes component scores, the decision, rationale, assertion evidence, and surviving coverage. Scores describe the original runner/suite, not every assertion independently; a highly useful mixed suite can still contain cases worth pruning. Evidence paths/line numbers refer to **`41c635c8`**, before deleted files and shifted lines. These are review judgments from reading assertions and production boundaries, not mutation-testing results or measured coverage percentages.

## Scoring

| Component | Points | Meaning |
| --- | ---: | --- |
| Failure impact | 0–3 | From harness/style noise to data integrity, crashes, correctness or security |
| Behavioral evidence | 0–3 | From token/type inspection to actual production execution or independent comparison |
| Unique coverage | 0–2 | Whether it catches a distinct failure beyond retained checks |
| Refactor durability | 0–2 | Whether harmless implementation, wording or layout changes invalidate it |

**7–10:** retain useful behavior. **4–6:** review/narrow; retain when a meaningful coverage gap remains. **0–3:** prune when the evidence supports doing so. These are priorities, not an automatic deletion policy. Runtime does not determine usefulness: expensive archive/concurrency and independent-oracle tests remain.

| Original CTest score | Entries |
| --- | ---: |
| 7–10 | 363 |
| 4–6 | 32 |
| 0–3 | 14 |

## Entire entries removed

| Test | Score | Reason and surviving coverage |
| --- | ---: | --- |
| `builtin_playfield_presentation_tests` | 1/10 | Only compile-time member-pointer signatures, replay pointer type and nonabstract BMSRenderer; main always returns success. Retain builtin_renderer_characterization_tests.cpp:1059 and :834 for actual frame submission and lane-cover drag, gameplay_simulation_tests and normal application compilation. Member-pointer signatures and nonabstract type pins add no runtime coverage. |
| `image_view_decoder_ownership_tests` | 1/10 | Only searches ImageView source text for forbidden decoder spellings; enforces ownership syntax rather than decoding behavior. Retain image_file_decoder_tests (tests/image_file_decoder_tests.cpp:307), image_decode_coordinator_tests (:289), and image_view_fade_tests (:383). These exercise decoding, cancellation and image ownership; deliberate loss is only the source-level architectural spelling ban. |
| `chart_filter_sort_panel_view_tests` | 2/10 | Only static_assert inheritance/private-member/type shapes and two option-list literals; executable main performs no runtime work. Retain chart_record_filters_tests.cpp:196 and main_menu_library_tests.cpp:443 for real query/sort behavior plus application compilation. Private-member/type pins and two literal option-list checks are intentionally relinquished; no claim of equivalent rendered option-list coverage. |
| `gameplay_skin_integration_tests` | 2/10 | Asserts empty std::function/default aggregate and that an explicitly initialized Failed enum remains Failed; no acquisition integration executes. Retain gameplay_skin_session_factory_tests.cpp:331 onward for actual no-selection/selected-failure/cancellation decisions and gameplay_skin_lifecycle_tests.cpp:965 for real acquisition semantics. The removed explicit Failed aggregate assertion only rechecks assigned data. |
| `gameplay_skin_loading_benchmark_script_contract` | 2/10 | Checks shell source spellings and one diagnostic phrase, without running either baseline or candidate benchmark. Benchmark runner and actual gameplay_skin_loading_benchmark_tests remain; its exact loop/spelling is no longer pinned. |
| `ir_ranking_detail_flow_audit` | 2/10 | Mostly exact icon, heading, helper-count and member-name assertions; ranking models/recycler have executable behavioral tests. tests/ir_ranking_modal_tests.cpp:185, :245 and :358 preserve stale-request rejection, details formatting and bounded visible rows; exact header/icon arrangement is intentionally not frozen. |
| `judgement_indicator_range_flow_audit` | 2/10 | Presence of field/function names cannot prove range values flow correctly; duplicates stronger range and presentation tests. tests/judgement_indicator_range_tests.cpp:64 and :82 verify numeric/clipping behavior. Shared replay config tests remain; not claiming full Settings UI propagation coverage. |
| `profile_archive_invalid_shard` | 2/10 | Invalid selector returns before all product tests; registration checks only test-runner argument handling and WILL_FAIL status. foundation_profile_archive_portable, foundation_profile_archive_validation, foundation_profile_archive_transactions, foundation_profile_archive_faults retain all production cases. |
| `profile_manager_invalid_shard` | 2/10 | Invalid selector returns before all product tests; registration checks only test-runner argument handling and WILL_FAIL status. foundation_profile_manager_bootstrap, foundation_profile_manager_deletion, foundation_profile_manager_integrity retain all production cases. |
| `records_ir_marker_audit` | 2/10 | Searches source for repository, callback and badge members that actual records loader/list tests exercise. tests/result_records_loader_tests.cpp:249 and :282 test upload states; tests/result_record_list_view_tests.cpp:201 and :204 test stable-key event routing. |
| `records_result_recall_flow_audit` | 2/10 | Large string inventory duplicates executable record actions and owner lifecycle fixtures without proving their composition. tests/main_menu_records_lifecycle_tests.py:96 executes owner callbacks; chart_record_actions_tests, course_record_actions_tests and records_ir_actions_tests retain preparation/upload semantics. |
| `result_visual_layout_audit` | 2/10 | Pins includes, view names and helper spellings; does not execute layout or draw ordering. tests/result_layout_geometry_tests.cpp:35, tests/result_gauge_history_tests.cpp:121 and tests/result_presentation_model_tests.cpp:477 keep real geometry/data behavior; visual stacking still needs UI verification. |
| `saved_course_result_browse_audit` | 2/10 | Checks only five header/source tokens; does not perform saved-course navigation. tests/course_record_actions_tests.cpp:156 and :168, tests/modern_result_recall_tests.cpp:536, tests/result_presentation_model_tests.cpp:1130 preserve preparation/aggregate/presentation behavior. Full interactive stage browsing remains a UI coverage gap. |
| `checkbox_button_content_tests` | 3/10 | Standalone checkbox mainly asserts constructor/getter/glyph/font choices; parent list tests already exercise selected content and toggle/lock routing. Retain ir_upload_candidate_list_view_tests.cpp:177 (actual selected checkbox content), :191 (toggle callback), :271 (recycled identity), :276 (locked input), and button_enabled_tests. Exact FontAwesome path/square glyph literals are consciously dropped, not duplicated by parent assertions. |
| `ios_release_documentation_tests` (standalone) | 1/10 | Exact release prose/badge phrases are not executable release safeguards. Actual setup, artifact and workflow tests remain. |

## Mixed suites narrowed

| Cases or assertions removed | Case usefulness | What remains |
| --- | ---: | --- |
| Remote-result source inventories and exporter source-check tail | 2/10 | Real recall/action/timing, filename sanitation, exported artifacts, failure behavior, native result presentation and extracted result-skin handoffs |
| Dropdown member/type assertions | 1/10 | Overlay placement at edges, normalized anchors and disabled/unavailable option behavior |
| Result layout's exact padding/gap/height constants | 2/10 | Compact/regular modes, content fitting, photo order and canvas sizing |
| Replay export's hand-written `4 + 530 - 150 == 384` arithmetic | 1/10 | Production metadata aggregation assertions immediately above it, plus result calculation suites |
| Required-artifact existence and frozen acceptance `pending` snapshots | 2/10 | Reference capture/trace and archive validation; actual acceptance validator mutation tests in the standalone suite |
| Superpowers plan path and `Task ` label requirements | 0/10 | Ledger status/provenance, completeness and current executed assertion ownership |
| Exact music arrangement choices and rendering every lead event separately | 2/10 | Relative onset math, synthesis/codec output, repeatability, release wrapping, seam rejection and atomic replacement |
| Literal Apply/scratch/player labels, old help copy and exact width/helper/TextView expressions | 1–3/10 | Catalog reference/placeholder integrity, real localization/UI behavior, and unique source guards for stale pointers, transactional unbind and initialization |
| TextView explicit link-list and texture-creation expression checks | 2/10 | Actual linker/font lifetime/deferred text geometry tests and a narrow transient-buffer prohibition |
| Windows Lua comment/source spellings | 2/10 | Real Lua filesystem/runtime/host behavior and the Windows security API link guard |
| AGENTS.md prose, bot identity, exact commit text/action versions and self-registration checks | 0–2/10 | Real setup/clean execution, artifact verification, branch restrictions, cancellation/serialization and release routing |

The iOS verification script now selects `builtin_renderer_characterization_tests` and `gameplay_skin_session_factory_tests` in place of the two removed type/aggregate-only executables. The release workflow tests verify the updated selection; distribution behavior is unchanged.

### Removed Python methods in retained suites

These exact cases were individually reviewed and scored 0–3/10 as summarized above. Source positions refer to the original revision.

| Original location | Method |
| --- | --- |
| `tests/beatoraja_gameplay_oracle_tests.py:110` | `test_required_oracle_artifacts_are_committed` |
| `tests/beatoraja_skin_reference_tests.py:654` | `test_acceptance_schema_freezes_device_protocol_and_completion_evidence` |
| `tests/beatoraja_skin_reference_tests.py:894` | `test_ordinary_runtime_io_contract_has_no_denial_policy` |
| `tests/beatoraja_skin_reference_tests.py:223` | `test_required_contract_artifacts_are_committed` |
| `tests/beatoraja_skin_reference_tests.py:526` | `test_runtime_io_contract_has_no_separate_aso_phase_policy` |
| `tests/generate_select_sounds_tests.py:51` | `test_lead_attacks_land_on_their_scored_subdivisions` |
| `tests/generate_select_sounds_tests.py:72` | `test_lead_uses_straight_eighths_against_an_exact_backbeat` |
| `tests/generate_select_sounds_tests.py:62` | `test_main_hook_preserves_syncopation_and_separates_note_attacks` |
| `tests/ios_build_setup_tests.py:849` | `test_agent_guidance_describes_automatic_ios_sources` |
| `tests/localization_catalog_tests.py:57` | `test_equal_english_labels_have_independent_contextual_ids` |
| `tests/lua_skin_file_system_windows_contract_tests.py:37` | `test_render_transition_does_not_invent_a_file_io_freeze` |
| `tests/lua_skin_file_system_windows_contract_tests.py:16` | `test_runtime_keeps_beatoraja_selected_directory_boundary` |
| `tests/lua_skin_file_system_windows_contract_tests.py:25` | `test_runtime_uses_ordinary_live_file_io` |
| `tests/settings_gameplay_skin_initialization_contract_tests.py:77` | `test_configuration_actions_load_the_latest_snapshot` |
| `tests/settings_gameplay_skin_initialization_contract_tests.py:92` | `test_diagnostic_history_keeps_one_text_view_per_record` |
| `tests/settings_input_binding_ui_contract_tests.py:13` | `test_settings_exposes_only_directional_scratch_actions` |
| `tests/settings_virtual_controller_ui_contract_tests.py:47` | `test_hispeed_auto_adjust_button_measures_its_longest_label` |
| `tests/settings_virtual_controller_ui_contract_tests.py:28` | `test_settings_card_keeps_controller_controls_compact` |
| `tests/settings_virtual_controller_ui_contract_tests.py:39` | `test_settings_card_offers_the_one_or_two_player_side` |
| `tests/text_view_transient_buffer_contract_tests.py:9` | `test_every_text_view_executable_links_the_sdl_ttf_lifetime_runtime` |
| `tests/text_view_transient_buffer_contract_tests.py:46` | `test_text_texture_materializes_only_when_the_view_is_rendered` |

## Deliberately retained coverage and limits

- Windows no-follow/reparse/ACL and UIKit synchronization source guards remain: the default macOS run cannot exercise those platform APIs. Their scores are modest; that is a reason for review, not blind deletion.
- Scene adapter checks with no identified complete behavioral replacement remain, including persistence recovery, Find BMS handoff, MainMenu ranking/cache lifetime and replay exporter batching/cancellation. The persistence audit lost only build/self-registration duplication.
- Parser/replay/SQLite integrity, cancellation, ownership, real compiled scene fixtures, render/device goldens, 513 MiB RAR5 coverage, audio quota stress, all regex oracle inputs, and both regex backends remain. A fixture using fake dependencies can still test real production control flow and is not automatically useless.
- Exact private field types, icon glyph/font choices, diagnostic TextView structure and musical arrangement choices intentionally cease to be frozen by tests. Retained model/unit checks do not establish end-to-end visual stacking, every Settings propagation path, or interactive saved-course stage browsing.
- This audit does not claim that every remaining assertion is optimal. Medium-scoring tiny helper suites remain where their distinct boundary coverage is uncertain or consolidation would require unrelated refactoring.

## Verification

- Desktop application and all configured targets built successfully after target removal and mixed-suite edits.
- The full parallel suite passed **395/395 in 179.42 seconds**. This pruning is a maintenance/coverage-quality cleanup; it does not demonstrate an additional runtime speedup over the earlier optimization work.
- After the final redundant replay arithmetic removal, its target rebuilt and a second full run passed **395/395 in 74.89 seconds** with warm binaries/caches.
- **49 iOS build-setup**, **20 iOS workflow**, and **26 acceptance-validator** checks passed. The acceptance scan initially rejected the unstaged deleted files still returned by `git ls-files`; staging the reviewed deletions fixed the input state without modifying the validator.
- Inventory comparison confirms exactly the 14 listed removals, no newly registered tests, and both native regex backends still present. Scorecard totals and all 21 removed Python methods were independently checked against the diff.
- Independent code/scorecard review found no blockers; `git diff --check` passed. No distribution command was run.

Future additions should name the regression they catch and first consider extending an existing behavioral suite. Test count, a new feature, a particular skill workflow, or a source spelling alone is not a reason to create another test.
