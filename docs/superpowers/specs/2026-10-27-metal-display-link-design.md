# Metal display-link pacing and drawable acquisition

The user approved implementing the candidates in the Metal HUD investigation,
then excluded measurement work. Implement worker-driven CAMetalDisplayLink and
later fallback drawable acquisition; leave telemetry unchanged and do not add a
new submission limiter. Keep three drawables and the existing iOS 15 minimum.

## Approach

Use a narrow native bridge on the existing iOS application/render worker. Its
run loop receives a CAMetalDisplayLink update before the scene loop samples
input and updates animation. Retain that update's drawable for this iteration
and lend it to bgfx through an optional provider hook in the generated Metal
source. No second nextDrawable call is allowed for a managed frame, including
when its drawable becomes invalid. bgfx still presents and commits normally.
The callback captures one drawable; it does not enqueue frames or move rendering
onto UIKit main. preferredFrameLatency is 1.0. No timed present API is used.

A callback-driven rewrite of the entire scene loop is unnecessary; a bounded
worker run-loop wait preserves the existing loop's ownership and cleanup.
Waiting only inside bgfx would sample input before the presentation wake and
would not achieve the intended input freshness. Neither approach is selected.

The native bridge is compiled on Apple platforms for local verification, but
only the iOS application opts into it. iOS 17+ supports the new path. iOS 15–16,
unsupported native setups and temporary callback failures use the existing
software pacing. Unlimited and caps at/above display refresh request the display
rate; exact divisors of that rate at 30 Hz or higher can request the lower rate.
Very low and nondivisor caps retain timer pacing to preserve their semantics.
The worker never adds the old post-submit software wait to a display-link frame.

## Ownership and interruption

- The worker owns link creation, run-loop registration, pending drawable and
  invalidation. UIKit layer mutations remain on main through existing helpers.
- Each loop iteration releases its lease on every exit, including idle, export,
  background and failed frame submission. An unused drawable is never presented.
- Ignore stale callbacks, and bound waits so inactivity/quit cannot strand the
  worker. A callback timeout invalidates the link and backs off before retrying.
- A lifecycle generation change, inactive state, mismatched layer, drawable
  size/format change, or a second claim rejects a managed drawable. No fallback
  acquisition can sneak into that managed frame; bgfx's existing nil-drawable
  path remains responsible for safely retiring encoded work.
- Pause the link before executing queued owner work (including replay export),
  when acquiring any export reservation (including inline photo export), on
  inactive/unsupported requests, and at destruction. Resume only from the
  next normal scene iteration. Preserve prepared-frame retirement semantics.

## View order

Order BGA base/layer and horizontal/vertical blur passes before the backbuffer
clear. Keep clear before final composition, gameplay and UI, with readback last.
This delays nextDrawable on the fallback path. A display-link frame receives its
drawable from the OS callback; view ordering cannot defer that delivery.
Preserve positive/zero blur, brightness/alpha, menu clear and export output.

## Verification and limits

Add focused behavioral coverage for rate selection, stale/duplicate leases,
interruption and resize. Exercise the backend provider boundary and offscreen
view ordering with real Metal where practical; retain generated-patch checks.
Run relevant rendering/platform tests, desktop and unsigned iOS builds, and an
available simulator smoke check. A fresh reviewer checks lifecycle/ownership.
No new performance telemetry, queue-depth measurement or physical latency claim.
Commit/push to the current PR branch; no worktree, merge or deployment.

Builder check resolution: a callback already past its deadline is rejected.
Once accepted, a frame that overruns its deadline may still present normally;
do not drop every frame under sustained overload. Lifecycle/geometry invalidation
still rejects it. This is an implementation choice to preserve forward progress,
not a claim that late frames meet their target presentation time.
