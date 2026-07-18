# AsoBMaShow Privacy Policy Design

**Date:** July 18, 2026  
**Status:** Approved for implementation

## Objective

Create a public, store-ready privacy policy for the version of AsoBMaShow
currently being submitted for Apple review. The policy will accurately describe
the submitted binary rather than preemptively describing the planned online
score and ranking service.

The policy will be written in English as a root-level Markdown file named
`PRIVACY_POLICY.md`. It will cover the iOS, Android, macOS, and Windows versions
of AsoBMaShow.

## Publisher and Contact

- Developer: VioletXF (Jaewoo Ahn)
- Privacy contact: zudevxf@gmail.com
- Effective date: July 18, 2026
- Audience: General audience; the app is not directed specifically to children
  under 13.

## Current Product Boundary

The reviewed version has no user accounts, advertising, cross-app tracking,
developer-operated analytics, crash reporting, or online score/ranking
service. Firebase is used for build distribution outside the running app, not
as an in-app data collection service.

The policy must not describe the planned score/ranking server as an active
feature. Before that service is enabled, its exact data fields, purposes,
identity model, recipients, safeguards, retention, deletion process, and user
controls must be designed and disclosed in an updated policy.

## Policy Structure

The policy will use concise sections in this order:

1. Scope and summary
2. Information the app handles
3. How information is used
4. Local storage, exports, and device backups
5. Network downloads and third-party services
6. Permissions and device capabilities
7. Data sharing and sale
8. Retention and deletion
9. Security
10. Children's privacy
11. Future online features and policy changes
12. International users and privacy requests
13. Contact

## Information Handled Locally

The policy will identify the following on-device information:

- player profile display name and locally generated identifier;
- chart-library folders, file paths, hashes, and chart metadata;
- scores, play history, replays, gameplay inputs, practice statistics, and
  presets;
- favorites, playlists, now-playing state, settings, and input bindings; and
- imported archives, downloaded charts, and user-created exports.

This information is processed and retained locally unless the user explicitly
exports it, asks the app to save media, or initiates an online lookup or
download. Gameplay replays may preserve processed control events, but raw
motion readings are not sent to VioletXF.

## Permissions and Device Capabilities

The policy will explain feature-specific access:

- Motion sensors support the optional gyroscope turntable controller and are
  processed on-device.
- MIDI, USB, keyboard, gamepad, and touch input support gameplay controls.
- iOS Photos access is used only when the user requests that exported replay
  media be saved.
- File and folder access supports chart libraries, imports, downloads, profile
  transfer, and exports.
- The Google Play Android flavor uses system document access. A non-Play
  Android distribution may request broad shared-storage access so the app can
  use chart folders directly.

Users can decline or revoke optional access through operating-system settings,
although the related feature may then be unavailable.

## Network and Third-Party Data Flow

Network activity is feature-driven and initiated by the user. BMS lookup and
download may send chart hashes or title/artist search terms to BMS catalog and
archive hosts. Difficulty-table import sends the requested table URL to its
host. Archive and metadata downloads send the requested URL to the relevant
host.

Those independent hosts necessarily receive ordinary request information such
as IP address and user agent and may apply their own logging, retention, and
privacy practices. The policy will make clear that VioletXF does not control
those services. It will not claim that every possible user-provided or
third-party URL has a specific transport or privacy guarantee.

If the user opens an external web page, the browser and destination site govern
that interaction. If the user exports or shares a file to another app or cloud
provider, the user-selected recipient governs its subsequent handling.

## Backups and Direct Contact

Local app data may be included in operating-system or device backups according
to the user's Apple, Google, or device settings. Backup retention and deletion
are controlled by the applicable platform provider and the user.

When a user emails the privacy contact, VioletXF receives the sender's email
address, message, and attachments. These are used only to answer the request,
maintain necessary support records, resolve disputes, or comply with legal
obligations.

## Sharing, Retention, and Deletion

AsoBMaShow does not sell personal data and does not use personal data for
advertising, cross-app tracking, or profiling. The user controls intentional
exports, shares, and connections to third-party content hosts.

Local data remains until the user deletes it through available app or device
controls, removes exported files, clears app data, or uninstalls the app.
Platform backups may persist under the provider's retention rules. Email
correspondence is retained only as reasonably necessary for the purposes above.
Users may request deletion of correspondence by emailing the privacy contact,
subject to legitimate legal or recordkeeping needs.

Because the current app has no developer-operated account or backend, there is
no remote AsoBMaShow account data to retrieve or delete.

## Security

The policy will describe proportionate safeguards without making guarantees
the implementation cannot support:

- local data benefits from operating-system and device protections;
- access is limited to what app features require;
- built-in network services use secure connections where supported; and
- third-party or user-supplied sources have their own security practices.

The policy will avoid absolute claims such as guaranteeing security or stating
that all network traffic is encrypted.

## Changes and Future Ranking Service

The policy will use a new effective date when practices materially change and
will provide an in-app, release-note, or similarly appropriate notice when
warranted. The online score/ranking service must not be enabled until the
privacy policy and relevant store privacy disclosures have been updated to
match its actual implementation.

## Store-Review Alignment

The completed policy must be suitable for publication at a stable public web
URL and accessible from the app or its store listing. Its disclosures must stay
consistent with the App Store privacy responses and Google Play Data safety
form.

The design accounts for the current store requirements that a policy identify
data handling, purposes, third parties, retention/deletion, safeguards, and a
privacy contact:

- [Apple App Review privacy guidance](https://developer.apple.com/app-store/review/)
- [Apple App Privacy Details](https://developer.apple.com/app-store/app-privacy-details/)
- [Google Play User Data policy](https://support.google.com/googleplay/android-developer/answer/10144311)

This design and the resulting policy are an implementation-oriented disclosure
based on the repository and approved product facts, not legal advice.

## Verification

Before completion:

1. Confirm the policy contains no placeholders or hypothetical active features.
2. Compare every data and permission statement with the current manifests,
   platform configuration, dependencies, and relevant source paths.
3. Confirm developer name, contact, effective date, and platform scope.
4. Check that collection, sharing, retention, deletion, security, children, and
   change-notice statements are internally consistent.
5. Render or inspect the Markdown structure for readable headings, links, and
   lists.
6. Review the final Git diff and commit only the intended documentation.
