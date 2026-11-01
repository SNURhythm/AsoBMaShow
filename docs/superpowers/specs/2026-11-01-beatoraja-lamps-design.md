# Complete beatoraja clear lamps

## Intent and scope

Support every beatoraja clear lamp as a native result, including Perfect and
Max. The reference is the local `../beatoraja` checkout, specifically
`ClearType.java`, `play/BMSPlayer.java`, and `result/MusicResult.java`.
The user approved upgrading historical results when retained judgments prove
the stronger lamp and requested a pull request after implementation.

## Approach

Extend the existing native lamp ranks and centralize judgment-based promotion.
Perfect uses rank 700 and Max uses 800, both through `clearLampRank<N>()`.
Existing ranks remain unchanged; 150 remains the reserved Light Assist alias.
Beatoraja skin IDs remain separate and map Perfect to 9 and Max to 10.

A display-only extension would leave persisted best lamps and filters wrong.
Rewriting every historical result would invalidate authenticated facts.
The selected approach is native support with derived historical upgrades and
refreshable mutable summaries, preserving original authenticated records.

## Awarding and presentation

For eligible completed chart and course results, use the existing full-combo
qualification and assist/playback restrictions. A qualified full combo with
no GOOD judgments earns Perfect; with no GREAT judgments either it earns Max.
Course-stage presentation uses stage-local judgments; course completion uses
aggregate judgments and requires all stages completed. Preserve beatoraja's
failure, gauge qualification, and assist precedence.

Expose distinct labels and colors, skin IDs, filter choices, record lamps,
best-lamp ordering, and result comparisons. Audit all Full Combo thresholds:
inclusive achievement checks still include Perfect and Max, while exact
categories and mappings distinguish them. Color details are an implementation
choice consistent with the existing palette.

## Historical data and compatibility

Promote only when retained counts establish a completed full combo with a
positive, consistent note total and the relevant judgment distribution.
Missing or contradictory evidence must not promote a result. Never infer a
judgment distribution from zero-filled optional fields or combine judgments
from separate attempts. Imported aggregate scores require their own explicit
lamp or sufficient per-attempt evidence.

Keep authenticated result facts, snapshots, fingerprints, replay payloads,
and submission proofs unchanged. Derive effective lamps at semantic read
boundaries and refresh mutable score/cache projections with an idempotent,
transactional migration or repair marker. Recovery and reconciliation must
continue to compare original canonical facts and reproduce the upgraded
projection. Avoid loading replay media or rescanning chart archives.

New result validators and persistence paths accept both ranks. Provider
adapters preserve distinctions supported by their protocol and explicitly
map stronger lamps to Full Combo where that is the provider's ceiling.
Do not fabricate unsupported remote lamp values.

## Validation and delivery

Extend existing behavioral tests for chart/course awards, assist restrictions,
failed and incomplete plays, skin mappings, filters, and provider mappings.
Exercise historical upgrades with small synthetic databases, including
insufficient evidence, idempotence, cache rebuilding, and authenticated
fingerprint preservation. Any optional real-profile check uses temporary
copies only.

Run the affected test groups and desktop compile check, broadening to the
full suite if shared-contract changes warrant it. Work on the provided
`feature/more-lamps` branch, commit and push task changes, then open a PR.
No deployment or parser changes are part of this work.
