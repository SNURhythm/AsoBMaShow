# AsoBMaShow Privacy Policy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish an accurate root-level Markdown privacy policy for the current iOS, Android, macOS, and Windows versions of AsoBMaShow.

**Architecture:** One public `PRIVACY_POLICY.md` document is the source of truth for the current app's privacy practices. It distinguishes local handling from off-device collection, explains user-directed permissions and third-party network requests, and establishes retention, deletion, security, children, contact, and future-change disclosures without describing the planned ranking server as active.

**Tech Stack:** Markdown, repository source/configuration review, Git whitespace and content checks

## Global Constraints

- Developer: VioletXF (Jaewoo Ahn)
- Privacy contact: zudevxf@gmail.com
- Effective date: July 18, 2026
- Platforms: iOS, Android, macOS, and Windows
- Audience: General audience; not directed specifically to children under 13
- Current app: No accounts, advertising, cross-app tracking, developer-operated analytics, crash reporting, or online score/ranking service
- Future service: Update the privacy policy and store disclosures before enabling online scores or rankings
- Do not claim that every network connection is encrypted or that security is guaranteed
- Do not describe Firebase build distribution as an in-app data collection service

---

### Task 1: Create and verify the public privacy policy

**Files:**
- Reference: `docs/superpowers/specs/2026-07-18-privacy-policy-design.md`
- Create: `PRIVACY_POLICY.md`

**Interfaces:**
- Consumes: Current data practices defined by the approved design and repository manifests/configuration/source
- Produces: A standalone public privacy policy suitable for a stable web URL and consistent store disclosures

- [ ] **Step 1: Confirm the output file does not already exist**

Run:

```bash
test ! -e PRIVACY_POLICY.md
```

Expected: exit code 0 with no output. If the file exists, inspect it and modify it with `apply_patch` instead of replacing unrelated user content.

- [ ] **Step 2: Create the policy with the approved disclosures**

Create `PRIVACY_POLICY.md` with this exact content:

```markdown
# AsoBMaShow Privacy Policy

**Effective date:** July 18, 2026

VioletXF (Jaewoo Ahn) ("VioletXF," "we," "us," or "our") provides AsoBMaShow. This Privacy Policy explains how AsoBMaShow handles information on iOS, Android, macOS, and Windows.

## Summary

AsoBMaShow is a local-first application. The current version does not provide user accounts, advertising, cross-app tracking, developer-operated analytics, crash reporting, or an online score or ranking service. VioletXF does not collect or receive the profiles, scores, replays, chart libraries, or settings that the app stores on your device.

Information leaves the app only when you direct a feature to interact with another service, such as downloading chart content, importing a difficulty table, opening a website, exporting or sharing a file, saving replay media, using a device backup service, or contacting us by email.

## Information Handled on Your Device

Depending on the features you use, AsoBMaShow may create, read, and store:

- your player profile display name and a locally generated profile identifier;
- chart-library folders, file paths, chart hashes, and chart metadata;
- scores, play history, replays, processed gameplay inputs, practice statistics, and practice presets;
- favorites, playlists, now-playing state, settings, and input bindings; and
- archives, charts, profile files, and other content that you import, download, or export.

This information is used to provide the app's library, playback, gameplay, practice, profile, and customization features. It remains on your device unless you intentionally export or share it or initiate a feature that connects to another service.

## Permissions and Device Capabilities

AsoBMaShow accesses device capabilities only to provide related features:

- **Files and folders:** The app accesses locations you select for chart libraries, imports, downloads, profile transfers, and exports. Google Play builds use Android's system document access. Some non-Play Android distributions may offer optional broad shared-storage access so that chart folders can be used directly.
- **Photos:** On iOS, Photos access is used only when you ask the app to save exported replay media.
- **Motion sensors:** Optional gyroscope and related motion readings let an iPhone, iPad, or supported Android device act as a turntable controller. Raw motion readings are processed on the device and are not sent to VioletXF.
- **Input devices:** MIDI, USB, keyboard, gamepad, and touch input are used for app and gameplay controls. Input bindings and processed replay events may be stored locally.

You can decline or revoke optional permissions in your operating-system settings. The feature that needs the permission may then be unavailable.

## Network Downloads and Third-Party Services

Network activity is initiated by features you choose to use. In particular:

- BMS search and download features may send chart hashes or title and artist search terms to third-party catalog, search, archive, or download hosts.
- Difficulty-table import sends the table URL you provide or select to the host serving that table and its related data.
- Opening an external page sends the requested address to your browser and the destination website.
- Exporting or sharing content sends the selected file to the app, person, or storage provider you choose.

Like ordinary internet services, these third parties may receive connection information such as your IP address, request time, requested URL, and user agent. Their handling of that information is governed by their own privacy policies and practices. VioletXF does not control these independent services. Avoid using untrusted download sources or sending sensitive information in URLs.

## Device Backups

Your operating system or a backup provider may include local AsoBMaShow data in a device backup, depending on your Apple, Google, device, and backup settings. You control these settings, and the provider's policies govern backup storage, retention, and deletion.

## Information You Send to Us

If you contact us by email, we receive the email address, message, and attachments you choose to send. We use them to respond, maintain necessary support records, protect our rights, resolve disputes, and comply with legal obligations. Our email provider processes this correspondence on our behalf under its own terms and privacy policy.

## Sharing and Sale of Information

We do not sell personal information. We do not use personal information for advertising, cross-app tracking, or profiling.

We do not receive or share your locally stored app data. Information may go to another party only through an action you direct, through a platform or backup feature you enable, in connection with email support, or when disclosure of information in our possession is required by applicable law or necessary to protect legal rights and safety.

## Retention and Deletion

Local app data remains on your device until you delete it using available app or device controls, clear the app's data, or uninstall the app. Files you export remain where you placed or shared them until you or the recipient deletes them. Device backups may remain under the backup provider's retention rules.

Because the current version has no developer-operated account or application backend, VioletXF has no remote AsoBMaShow account data for you to retrieve or delete. We retain email correspondence only for as long as reasonably necessary for the purposes described above. You may request deletion of correspondence in our possession by emailing [zudevxf@gmail.com](mailto:zudevxf@gmail.com), subject to legal and legitimate recordkeeping needs.

## Security

We limit information access to what app features require and rely on operating-system and device protections for locally stored data. Built-in network services use secure connections where supported. However, user-supplied and third-party sources have their own security practices, and no storage or transmission method can be guaranteed to be completely secure.

## Children's Privacy

AsoBMaShow is intended for a general audience and is not directed specifically to children under 13. The current version does not knowingly collect personal information from children through an account or application backend. If you believe a child has sent personal information to us by email, contact us so we can review and, when appropriate, delete it.

## International Users

Your local app data stays on your device unless you use a feature that sends it elsewhere as described in this policy. Email providers, backup providers, and third-party content services may process information in countries other than your own under their respective policies. You may contact us with questions or requests concerning your information.

## Future Online Features and Policy Changes

This policy describes the current version of AsoBMaShow. Before enabling a future online score or ranking service, we will update this policy and the applicable store privacy disclosures to explain the service's actual data practices.

We may also update this policy when the app or legal requirements change. We will post the revised policy with a new effective date and provide an in-app, release-note, or similarly appropriate notice when a change is material.

## Contact

For privacy questions or requests, contact:

VioletXF (Jaewoo Ahn)  
[zudevxf@gmail.com](mailto:zudevxf@gmail.com)
```

- [ ] **Step 3: Verify identity, scope, current-state, and future-feature statements**

Run:

```bash
rg -n "VioletXF \(Jaewoo Ahn\)|zudevxf@gmail\.com|July 18, 2026|iOS, Android, macOS, and Windows|no developer-operated account|future online score or ranking service" PRIVACY_POLICY.md
```

Expected: matches for publisher/contact, effective date, platform scope, current no-backend deletion statement, and future ranking disclosure.

- [ ] **Step 4: Verify required policy topics and reject placeholders**

Run:

```bash
rg -n '^## (Summary|Information Handled on Your Device|Permissions and Device Capabilities|Network Downloads and Third-Party Services|Device Backups|Information You Send to Us|Sharing and Sale of Information|Retention and Deletion|Security|Children.s Privacy|International Users|Future Online Features and Policy Changes|Contact)$' PRIVACY_POLICY.md
! rg -n 'TBD|TODO|FIXME|PLACEHOLDER|example\.com' PRIVACY_POLICY.md
```

Expected: all 13 section headings match and the placeholder scan produces no output.

- [ ] **Step 5: Reconcile disclosures with the current app configuration**

Run:

```bash
rg -n "NSMotionUsageDescription|NSPhotoLibrary(Add)?UsageDescription" ios/Xcode/AsoBMaShow/AsoBMaShow/Info.plist
rg -n "INTERNET|MANAGE_EXTERNAL_STORAGE|sensor\.gyroscope|software\.midi|usb\.host" android/app/src/main/AndroidManifest.xml android/app/src/firebase/AndroidManifest.xml
rg -n "IS_ANALYTICS_ENABLED|IS_ADS_ENABLED" ios/Xcode/AsoBMaShow/AsoBMaShow/GoogleService-Info.plist
rg -n "CURLOPT_URL|HttpURLConnection|lookupByMd5|searchQueries|ImportFromUrl" src android/app/src/main/java --glob '!**/bms_parser.*'
```

Expected: evidence for Photos, motion, internet, optional all-files access, gyro/MIDI/USB capabilities, disabled Firebase analytics/ads flags, and user-directed network features. Review the matches and confirm no statement in the policy exceeds that evidence.

- [ ] **Step 6: Inspect Markdown and Git hygiene**

Run:

```bash
sed -n '1,320p' PRIVACY_POLICY.md
git diff --check
git diff -- PRIVACY_POLICY.md
git status --short
```

Expected: readable Markdown, no whitespace errors, only the intended policy content in its diff, and no unrelated new modification caused by this task.

- [ ] **Step 7: Commit the policy**

Run:

```bash
git add PRIVACY_POLICY.md
git commit -m "docs: add privacy policy [skip ci]"
```

Expected: one commit containing only `PRIVACY_POLICY.md`.

- [ ] **Step 8: Verify the committed result**

Run:

```bash
git show --stat --oneline --summary HEAD
git status --short
```

Expected: the latest commit is `docs: add privacy policy [skip ci]`, its stat lists only `PRIVACY_POLICY.md`, and the worktree has no changes introduced by this task.
