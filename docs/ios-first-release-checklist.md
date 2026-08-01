# iOS-first release checklist — 0.0.1

This checklist is the final human gate for the first iOS/iPadOS release. The
shipping contract is marketing version `0.0.1`, minimum iOS/iPadOS 14.0, iPhone
and iPad support, and `NSAllowsArbitraryLoads = true` so user-selected HTTP
difficulty tables continue to load. It does not authorize a deployment.

## Automated candidate gate

- [x] Run `scripts/ios_release_verify.sh` without any distribution action.
- [x] Confirm release-critical native tests and iOS release-contract tests pass.
- [x] Build the unsigned device app and run `scripts/ios_artifact_audit.sh`.
- [x] Confirm the unsigned artifact reports version 0.0.1, iOS 14.0, arm64,
  iPhone/iPad families, compiled icons, permission strings, retained ATS
  behavior, and resolved embedded frameworks.
- [ ] Run `scripts/ios_artifact_audit.sh --require-signature` on the exact signed
  archive/export selected for App Store Connect.

Privacy manifests are intentionally omitted from this repository release gate,
as accepted for 0.0.1. Do not add or synthesize one merely to satisfy this
checklist. Re-evaluate that decision if App Store validation, a bundled SDK's
declared requirements, or Apple's submission requirements report a concrete
manifest obligation.

## App Store Connect metadata

- [ ] Complete Privacy labels from the behavior documented in
  `PRIVACY_POLICY.md`, including optional IR data sent directly to the selected
  independent provider. Confirm there is no tracking, advertising, or
  developer-operated analytics claim.
- [ ] Complete the Age rating questionnaire for music/rhythm gameplay and any
  user-imported content that can be displayed.
- [ ] Verify the Support URL opens the published support page and shows the
  current contact address.
- [ ] Verify the Privacy policy URL opens the published August 1, 2026 policy.
- [ ] Answer Export compliance for the exact binary and its networking/
  cryptography dependencies; retain the App Store Connect answer or supporting
  determination with the release record.
- [ ] Confirm Content rights for the app, bundled sample assets, fonts,
  libraries, screenshots, and any music/chart material shown in store media.
- [ ] Write Review notes that explain document-based chart import, optional IR,
  Photos export, motion-controller permission, and why arbitrary network loads
  remain enabled for user-selected difficulty tables.
- [ ] Confirm category, copyright, contact details, release notes, and version
  text all identify release 0.0.1.

## Store media

- [ ] Upload current iPhone screenshots from the release candidate, covering
  first launch/library, chart selection, gameplay, and results.
- [ ] Upload current iPad screenshots captured from the iPad layout, not scaled
  iPhone images.
- [ ] Check every screenshot for private chart paths, profile names, API keys,
  test accounts, debug overlays, and third-party content without release rights.

## Signed archive and upload readiness

- [ ] Archive from the reviewed commit only after the verification job passes.
- [ ] Confirm the signed archive uses bundle ID `com.snurhythm.AsoBMaShow`,
  marketing version 0.0.1, the intended unique build number, App Store profile,
  distribution certificate, and Release configuration.
- [ ] Validate the archive in Xcode/App Store Connect and save the validation
  result with the commit and build number.
- [ ] Run the artifact audit with signature enforcement against the exported
  `.app` or `.ipa`; resolve every failure before upload.
- [ ] Confirm the serialized TestFlight job is the only upload path selected.

## Physical-device smoke

- [ ] On a physical iPhone running a supported OS, test clean install, upgrade,
  first launch, profile readiness, chart-folder selection, heavy library
  scrolling, chart launch, gameplay audio/input, BGA playback and seek,
  background/foreground, memory warning recovery where practical, Photos
  export, anonymous HTTP ranking behavior, authenticated HTTPS IR, and rejected
  authenticated HTTP IR.
- [ ] Repeat the core flow on a physical iPad, including landscape layout,
  document picker, touch controls, BGA rendering, background/foreground, and
  replay export.
- [ ] Confirm no crash, main-thread checker violation, fatal log, sustained
  audio underrun, unbounded memory growth, or credential disclosure appears.

## Release record

- [ ] Record commit SHA, build number, Xcode/SDK version, archive checksum,
  automated gate output, artifact-audit output, App Store validation result,
  physical iPhone/iPad devices and OS versions, and any accepted limitations.
- [ ] Obtain the final human go/no-go decision before selecting the TestFlight
  or App Store submission action.
