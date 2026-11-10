# GingerRush downloads and the retired embedded browser

BMS downloads were suspended on 2026-11-08. On 2026-11-10, the developer reported
permission from GingerRush's operator and requested restoration of that source
only. Missing-chart actions in both selectors now look up a valid MD5 through
GingerRush, with confirmation, progress, cancellation, archive verification and
local import. SHA256-only or title-only entries cannot start a lookup.

GingerRush is the only packaged lookup driver. Failure, cancellation or access
rejection never falls through to another source. No embedded browser, HTML
search, browser-captured downloads or arbitrary-URL admission UI is restored.
The retained skin download-site event remains a no-op. Normal local folder and
archive import, difficulty tables, storage tools and Bokutachi IR remain available.

Mobile downloads use a fresh private `GingerRushDownloadsV1` job root and new
native session/job identifiers. Tasks shows their progress and recovery actions.
On upgrade, Android cancels unscoped legacy jobs and their notification channel;
iOS invalidates the old background session and cancels old continued-processing
requests. These paths never resume pre-retirement jobs or read their saved URLs.
Completed chart libraries, scores, profiles and legacy private staging are preserved.

## GingerRush authorization review — 2026-11-10

- **Operator/service:** catizard / GingerRush.
- **Permission source:** direct Discord authorization, reported by the
  AsoBMaShow developer in the development conversation on 2026-11-10. The
  developer reported that permission was granted after asking to integrate
  GingerRush into AsoBMaShow for finding/downloading BMS packages, then explicitly
  requested restoration of GingerRush only. This record is a developer-attested
  summary; it is not a quotation or public copy of the private DM.
- **Authorized scope:** AsoBMaShow's GingerRush package lookup/download. No
  authorization is recorded for Konmai, Wriggle, HorieYuuka, BMS Search, IPFS,
  Google Drive/Dropbox discovery, or embedded browsing.
- **Access method:** user-initiated HTTPS GET to
  `https://gingerrush.com/download/package/{md5}` for a validated chart MD5;
  consume its JSON package URL and filename, then download that package.
  PixelDrain viewer links returned by this API use the ordinary file/filesystem
  download endpoints already used by this integration. This is file transport
  for GingerRush's response, not another package lookup provider. No HTML
  scraping, browser cookies, CAPTCHA handling or alternative-provider fallback.
- **Identification/restrictions:** identify as `AsoBMaShow`; preserve HTTPS and
  normal redirect security. No additional quotas, authentication requirements,
  or redistribution permission were supplied in the developer's report. Do not
  infer unrestricted bulk access or redistribution rights. Keep user-initiated
  acquisition and the existing bounded transfer queue. Access rejection ends
  the attempt; do not change identity or access method to get around it.
- **Implementation decision:** restore only GingerRush and shared download,
  import and progress UI. Use fresh durable-job/session storage so pre-retirement
  jobs from other sources cannot resume. Keep the existing legacy cancellation
  paths and the public apology. Verify with synthetic responses and loopback
  transports; do not contact unapproved services during tests.

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

The `area_downloads` test group covers GingerRush JSON parsing and source isolation,
invalid MD5, rejection/cancellation, native loopback transport, durable job recovery,
archive validation/publication and the shared download modal. Selector, library,
settings and UI groups cover the restored entry points and local import behavior.
Tests use synthetic packages and loopback servers, without probing live providers.

`bms_download_removal_tests` checks that unapproved drivers and the embedded browser
remain absent, legacy jobs remain isolated, and all five apology assets include the
November 10 update. It also preserves the original Korean text's checksum. The
start-screen notice remains available offline in all five languages.
