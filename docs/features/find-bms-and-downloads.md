# Retired BMS downloads and embedded browser

BMS package search/download and the embedded browser were removed on 2026-11-08
while the permitted use of external services is reviewed. Neither selector,
Tasks, Settings, nor the newcomer tutorial offers these actions. The retained
skin download-site event is a no-op. Provider clients, durable download jobs,
native browser hosts and mobile transfer services are no longer packaged.

On upgrade, Android cancels jobs belonging to the retired download service and
its notification channel. iOS invalidates the legacy background URL session and
cancels pending background tasks in the downloads namespace. These cleanup
paths never recreate a transfer or resume saved jobs. Completed chart libraries,
scores and profiles are preserved; private download staging is left untouched.

Local folder/archive import, library scanning, difficulty-table import, storage
tools and Bokutachi IR remain available. Tasks shows local library progress.
Users can acquire files independently and import them through the existing
local import workflow.

## Public apology

The start screen has a persistent **Developer’s apology** action. It opens the
complete document offline, in the app's English, Korean, Japanese, Simplified
Chinese or Traditional Chinese language. Korean is the developer's original
text without editorial changes. Translations provide a button to read that
original and return to the translation. The text retains the original date and
planned-action wording; it is not a live implementation-status report.

Mouse wheel, touch gestures and arrow/Page Up/Page Down/Home/End keys scroll the
document. Escape/Back closes it. Tab selects the original/close action and Enter
activates it. Gamepad Up/Down scrolls, East closes, and West switches between the
translation and original. Opening the notice suspends the start-screen input
adapter so queued gameplay input cannot start a game behind it.

Assets live in `assets/notices/`; `PublicApologyModal` uses the existing
virtualized document reader. No web view or external page is used.

## External-service review before any new integration

Before adding, restoring or expanding an external integration, record:

- The operator, service and exact endpoints/methods, including any HTML
  collection, embedded browser, automatic retry or download behavior.
- The official permission, terms or direct operator authorization covering
  AsoBMaShow, with a source, the date checked and any restrictions.
- Authentication, identification, rate/concurrency limits, redistribution
  conditions, and how the implementation satisfies them.
- Remaining uncertainty and the decision to keep the integration disabled
  until the operator's permission is clear.

Another client's open-source implementation is not evidence that its service
access is permitted for AsoBMaShow. A public webpage is not permission to collect
its HTML, and permission to use an API does not establish permission for other
access methods.

If an operator blocks or restricts an existing integration, stop it immediately.
Confirm the operator's current position before deciding whether to resume.
Do not change User Agent identification, browser behavior, endpoints, or access
methods to continue using a restricted service while permission is unresolved.

## Verification

`bms_download_removal_tests` checks retired packaging and all five notice assets,
including the exact Korean checksum. Intro navigation tests cover the notice
entry; text-input view tests cover its language switch, scrolling, close actions
and responsive viewport. Local library/archive and selector tests continue to
exercise file import, indexing, navigation and task progress independently of
network package acquisition.
