# Local Combo reminders

Opt-in, device-local notifications on iOS and Android, without a backend.
Daily reminders use a user-selected local time and omit today after its goal is
complete. Comeback reminders occur 2, 7 and 14 civil days after projected combo
loss, then stop. Returning to the app cancels the previous comeback series.
Freeze inventory and partial-play protection participate in loss forecasting.
Settings belong to the device, not exported profiles. Notification taps open
Combo when the music selector is ready; stale profile payloads never switch
profiles. Permission is requested only through an explicit enable action.

## Implementation

1. Add pure bounded calendar scheduling policy and device preference storage.
   Verify goal completion, freezes, partial play, lost combos and cutoff times.
2. Add native adapters: iOS UserNotifications and Android inexact AlarmManager.
   Own only Combo requests; handle permission, foreground suppression, launch
   intents, stale plans and Android reboot/time changes.
3. Connect cached application state/lifecycle and localized Combo settings.
   Recompute after score/profile/preferences/day changes without per-frame DB I/O.
4. Run focused native tests, desktop build and available mobile compile checks;
   review lifecycle edge cases, then commit and push the existing branch.

## Limits

The OS controls delivery timing and may delay/suppress alerts. Plans are bounded
below iOS's pending request limit and refreshed on use. No background server or
analytics are involved. No deployment is authorized.
