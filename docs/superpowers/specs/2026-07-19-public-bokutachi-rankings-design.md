# Public Bokutachi Rankings Design

## Goal

Allow enabled Bokutachi profiles to read chart rankings without an API key while retaining optional authenticated identity highlighting and keeping every score-submission operation authenticated.

## Request contract

- Chart resolution (`POST /api/v1/games/:game/charts/resolve`) and PB pages (`GET /api/v1/games/:game/charts/:chartID/pbs`) are public requests. They never carry an `Authorization` header, even when a profile has an API key.
- When the profile has no API key, ranking fetches skip `/api/v1/status`; rows are returned without a `currentUser` marker.
- When the profile has a valid API key, only `/api/v1/status` receives it. The resolved user ID marks matching rows as the current user.
- A non-empty malformed credential is rejected before HTTP. A server rejection from the authenticated identity request is reported as `AuthenticationRequired`; it is not silently downgraded to anonymous access.
- Submission and submission polling retain their existing mandatory API-key validation and authenticated requests.

## Parsing and pagination

The native ranking parser accepts an optional authenticated user ID. PB validation and all score fields are identical in authenticated and anonymous mode; only `currentUser` differs.

Ranking continuation tokens retain the public chart ID and pagination counters and encode `userID` as either a positive integer or JSON `null`. Existing authenticated tokens remain valid. Anonymous continuation requests remain public and do not need a credential lookup.

The persistent `bokutachi-cache` continues to store resolved chart IDs. Cached user IDs are consulted only when the current fetch has a valid credential, so an anonymous fetch never highlights a previously authenticated account.

## Service and UI boundary

`IrRankingService` always calls the ranking-capable driver, passing an empty credential when none exists. Provider enablement and chart compatibility remain the UI availability conditions; credential presence is not one.

## Security and failure behavior

- Public requests cannot leak API keys through headers, bodies, page tokens, diagnostics, or cache rows.
- Identity requests and diagnostics retain credential redaction.
- Cancellation, stale chart recovery, response limits, and malformed-page validation remain unchanged.

## Verification

Add focused tests for anonymous parsing, anonymous first-page and continuation fetching, public header absence in authenticated mode, malformed configured credentials, optional service credentials, and unchanged authenticated identity highlighting. Run the focused test targets and the desktop compile target.
