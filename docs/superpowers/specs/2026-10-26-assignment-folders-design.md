# Assignment folders

Add a virtual Assignments root to both PLAY selectors (built-in and skin-driven). Its contents combine one
subfolder per active daily assignment and a deduplicated union of playable
charts that can still contribute. Each subfolder shows that assignment's charts.
The user explicitly chose to hide charts already counted today.

Reuse the full daily eligibility decision: selected table levels and current
previous lamp, or persisted sticky eligibility. Targets are achievements to
work toward, not browsing filters. A chart counted in one assignment can still
appear in another. After the shared bonus budget is spent, folders remain but
contain no contributing charts. Missing/unavailable charts are omitted.

Use today's persisted assignment snapshot, not tomorrow's pending edits.
Refresh on selector resume, repository changes, LN-mode changes and local-day
rollover. Root creation reads no chart payloads; candidate metadata loads lazily
from selected table levels and indexed hashes for sticky charts. Reuse each
level query and deduplicate overlaps. No archive scans or extraction.

The built-in browser keeps its normal paginated SQL chart list. A bound JSON
hash allowlist scopes counts, search, filtering, sorting and random selection
to the same eligible candidates as the skin-driven folder projection.

Use the compact localized root label “Assignments”; keep user assignment names unchanged.
