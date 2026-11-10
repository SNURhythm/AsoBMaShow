# GingerRush-only download restoration

## Intent and authorization

Restore the removed download feature at the developer's explicit request after
catizard granted GingerRush use. The permission record and access method are in
`docs/features/find-bms-and-downloads.md#gingerrush-authorization-review--2026-11-10`.

## Scope

Restore existing missing-chart download actions, confirmation/progress/cancel,
archive verification/import, settings and mobile durable download task UI.
Only a valid MD5 can start a GingerRush lookup. The resulting package URL is the
only download candidate. No other source lookup, failover, search webpage,
embedded browser or manual arbitrary-URL download is restored. Keep local
imports and legacy-transfer retirement. Preserve the original apology text and
append the developer's November 10 GingerRush authorization/restoration update
in Korean and all four translations.

## Approach

Selectively restore the existing UI, archive and transfer infrastructure, with
one GingerRush driver and fresh mobile job/session storage. Reverting the whole
removal commit would restore unauthorized providers and remove the notice.
Rewriting the full transport/import system would discard existing recovery and
archive safeguards without benefiting this request.

## Boundaries

Missing/invalid MD5, not-found, malformed response, cancellation and rejection
must terminate without consulting another provider. Use ordinary GingerRush
package transport, with AsoBMaShow identification. Do not reattach legacy jobs
or import their saved URLs. Preserve crash recovery for newly authorized jobs.
No deployment, real-service probe, worktree, or changes to the user's existing
Xcode project edits are required for this task.
