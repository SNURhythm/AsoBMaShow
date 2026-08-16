# Skin Safety Level Design

## Goal

Let the owner choose how much protection the Lua gameplay-skin host applies
without silently changing Beatoraja-compatible behavior. The default remains
safe; the most permissive level can lift every protection only after an
explicit warning acknowledgement.

## Levels and UI

`SkinSafetyLevel` has three persisted values:

1. `Standard` is the default and enforces every host safeguard.
2. `BeatorajaCompatibility` relaxes safeguards classified as protective while
   retaining catastrophe safeguards.
3. `Unrestricted` relaxes both protective and catastrophe safeguards.

The Gameplay Skins panel presents a labelled three-value dropdown. Selecting
`Unrestricted` opens a blocking modal that explains a skin can consume
storage, memory, CPU, and file descriptors; change process-wide state; and
read or write outside its package. Cancel keeps the prior level. The explicit
enable action persists `Unrestricted`; loading an already acknowledged value
does not show the modal. No level change triggers a rescan.

The existing `gameplayCompatibilityEnabled` setting remains a legacy selected
skin enablement alias and is not reused for safety level.

## Policy and scope

One `SkinSafetyPolicy` derives effective behavior from `SkinSafetyLevel`.
Every security-related resource or containment decision takes that policy
instead of adding its own compatibility boolean. Standard paths and diagnostics
remain byte-for-byte behaviorally identical. Unrestricted follows Beatoraja's
direct, live-file behavior at existing protection branches. Malformed input
and normal I/O failures still remain errors.

The policy covers package/folder/archive import and catalog traversal, Lua
virtual-file resolution and writes, Lua allocation/execution budgets, Lua
table/binding decoding, and image/font/resource allocation. It introduces no
automatic rescan and no semantic Lua/model validation.

## Persistence and verification

The profile setting defaults to Standard for old settings. Package operations
use the active profile's policy, and that same policy flows into catalog and
runtime work. Tests cover migration, modal cancellation/confirmation, exact
Standard diagnostics, and each relaxed policy path.
