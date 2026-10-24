# Lua skin additions from beatoraja 0.8.9

These additions follow upstream `83574048` and extend AsoBMaShow's existing
Lua skin host. Historical oracle fixtures remain pinned to their original
versions. Functions in `main_state` become available during configured loading.

## HTTP

`main_state.http_get(url, timeout_ms, max_bytes)` returns normalized text and
`true`, or `nil, error`. `http_get_lines` returns an array of lines instead.
Timeout defaults to 1000 ms and is clamped to 1–5000 ms. Capacity defaults to
1 MiB and must be an integer from 1 through 8 MiB. The limit counts received
body bytes, including newline and UTF-8 bytes. There is no separate line-count
limit. These synchronous functions remain unavailable in frame callbacks.

For frame callbacks, start a skin-owned asynchronous request:

```lua
local id, error = main_state.http_get_async(url, 1000, 1048576)
-- In a later callback:
local response = main_state.http_poll(id)
if response.status == "completed" then
    -- response.lines is a 1-based array; response.status_code is the HTTP status.
elseif response.status == "failed" then
    -- response.error describes the failure.
end
```

`http_poll` always returns `status`, `status_code`, `lines`, and `error`.
A pending result has status `pending`. Reading a terminal result consumes it;
subsequent polls fail. `http_cancel(id)` returns whether it found and cancelled
the request. Disposing a skin cancels its retained requests without waiting on
the rendering thread.

Async requests use two shared download workers, a 32-job queue, at most 16
retained requests/results per skin, and a 16 MiB per-skin capacity budget.
The timeout includes queue time. Async destinations must be public HTTP(S),
with no URL credentials, no HTTPS downgrade, and at most three redirects.
The existing synchronous APIs retain their separate destination policy.

The limited `java.net.URL` connection facade also accepts
`connection:setResponseLimitBytes(max_bytes)`, including after connecting and
before reading the body.
Its reader supports `reader:close()`; reading a closed reader fails.

## Browser navigation

`main_state.open_url(url)` returns a boolean. The URL must be absolute HTTP(S),
contain no credentials, and be at most 2048 bytes long. Navigation requires a
current Enter, Space, or pointer press edge and is limited to one attempted
navigation per second per skin. Holding a key does not repeatedly authorize
navigation. Unsupported platform actions return `false`.

Catalog loading, validation, settings previews, and image/video export disable
browser navigation, new async requests, and file deletion. Live gameplay,
selection, and result skins receive their own native services.

## Input facade

The limited `com.badlogic.gdx.Gdx.input` facade supports:

- `isKeyPressed(key)` and `isKeyJustPressed(key)`.
- `isButtonPressed(button)` and `justTouched()`.
- `getX()`, `getY()`, `getDeltaX()`, and `getDeltaY()`.

Positions use drawable pixels with a top-left origin. Deltas and press edges
expire at the next input frame; multiple reads within a frame agree.
`com.badlogic.gdx.Input.Buttons` defines `LEFT`, `RIGHT`, `MIDDLE`, `BACK`,
and `FORWARD`. `Input.Keys` accepts case-insensitive constant names and existing
display labels; unknown names return `nil`. `ANY_KEY` applies to held-key reads,
not just-pressed reads. These additions do not expose unrestricted Java APIs.

## File facade

`luajava.new(File, path)` and `luajava.newInstance("java.io.File", path)` create
the limited File facade. It supports `mkdir`, `listFiles`, `isFile`,
`isDirectory`, `getName`, `getPath`, and `tostring(file)`.

`listFiles()` returns File objects, includes directories, skips symbolic links,
and returns `nil` if the accessible result exceeds 4096 entries. Ordering is
filesystem-defined. `main_state.file_list` retains its string-list contract.
Direct file helpers reject linked path components and revalidate facade paths
when used. Existing legacy `io` and resource-resolution policies are separate.

`main_state.file_delete(path)` returns `true` only when it deletes an authorized
regular file. It does not delete directories or follow symbolic links.
