# Persistent Bokutachi Lookup Cache Design

## Goal

Reduce the cold native Bokutachi ranking request from three serial HTTP calls
to one whenever the chart mapping and authenticated user identity have already
been observed for the active profile.

## Chosen Design

Each player profile owns a `bokutachi-cache.json` file beside its settings and
databases. The cache contains only reusable native API identifiers:

- one numeric Bokutachi user ID per normalized server origin;
- native chart IDs keyed by normalized server origin, BMS game, and lowercase
  chart SHA-256.

The file uses a versioned JSON object and is loaded once into a mutex-protected
in-memory store when IR services activate a profile. Cache hits do not write to
disk. Successful identity or chart resolution writes only when the stored
value changes, using the existing atomic-file infrastructure. Chart mappings
retain insertion order and are bounded to 2,048 entries; server origins are
bounded to 16 and the JSON input is capped at 1 MiB.

`TachiDriver::fetchChartRanking` consults the store before making prerequisite
requests. Missing chart IDs still use the native chart-resolve route, and a
missing user ID still uses `/api/v1/status`. Successful values are remembered
before the native PB request. If a PB request returns chart-not-found while
using a cached chart ID, the driver evicts that mapping, resolves it once, and
retries the PB request once. Other failures retain their existing behavior.

The cache is profile-local so a user ID is never shared across profiles. A
successful Bokutachi API-key replacement or removal clears cached user IDs but
keeps chart mappings, which are not credential-specific. Profile archives do
not include the cache.

## Data Shape

```json
{
  "schemaVersion": 1,
  "origins": [
    {
      "serverOrigin": "https://boku.tachi.ac",
      "userID": 123,
      "charts": [
        {
          "game": "bms-7k",
          "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
          "chartID": "native-chart-id"
        }
      ]
    }
  ]
}
```

The cache never contains API keys, derived key fingerprints, submission
payloads, outbox state, or score data.

## Failure and Compatibility Handling

- Cache load, validation, or write failures never prevent ranking requests;
  the driver falls back to the existing network flow.
- Malformed or oversized disposable cache data is treated as empty and may be
  replaced by later successful lookups.
- A file with a newer schema version is left untouched and disables cache
  writes for that activation, preserving forward compatibility.
- Origins, game names, hashes, chart IDs, user IDs, collection sizes, and file
  size are validated before entering memory.
- Cache mutation is synchronized because ranking and profile-service callbacks
  may run on different threads.
- Ranking continuation tokens already carry the resolved chart and user IDs,
  so paginated requests remain a single HTTP call and do not access the cache.

## Scope

This change persists native lookup results only. Reusing HTTP connections or
platform URL sessions is a separate transport optimization.
