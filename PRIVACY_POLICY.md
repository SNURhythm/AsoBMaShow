# AsoBMaShow Privacy Policy

**Effective date:** July 29, 2026

VioletXF (Jaewoo Ahn) ("VioletXF," "we," "us," or "our") provides AsoBMaShow. This Privacy Policy explains how AsoBMaShow handles information on iOS, Android, macOS, and Windows.

## Summary

AsoBMaShow is a local-first application. It does not provide an AsoBMaShow account, advertising, cross-app tracking, developer-operated analytics, crash reporting, or a developer-operated application backend. VioletXF does not receive the profiles, chart libraries, replays, settings, or other information that remains on your device.

AsoBMaShow includes an optional Internet Ranking (IR) feature. IR is disabled by default. If you enable an IR provider, the app can connect directly to that independent provider to read rankings, authenticate your provider account, submit gameplay results manually or automatically, or download your provider score history. The default provider is the Bokutachi deployment of Tachi at [boku.tachi.ac](https://boku.tachi.ac). VioletXF does not operate that service or receive the data sent to it through an AsoBMaShow backend.

Information may also leave the app when you direct a feature to interact with another service, such as downloading chart content, importing a difficulty table, opening a website, exporting or sharing a file, saving replay media, using a device backup service, or contacting us by email.

## Information Handled on Your Device

Depending on the features you use, AsoBMaShow may create, read, and store:

- your player profile display name and a locally generated profile identifier;
- chart-library folders, file paths, chart hashes, and chart metadata;
- scores, play history, replays, processed gameplay inputs, practice statistics, and practice presets;
- favorites, playlists, now-playing state, settings, and input bindings;
- archives, charts, profile files, and other content that you import, download, or export; and
- if you use IR, provider settings, an API key, queued submissions, upload status and receipts, cached provider and chart identifiers, public rankings, and provider score history downloaded to the device.

This information is used to provide the app's library, playback, gameplay, practice, profile, customization, and optional IR features. It remains on your device unless you intentionally export or share it, enable an IR operation described below, or initiate another feature that connects to a service.

## Internet Ranking (IR)

IR is an optional connection between AsoBMaShow and an independent Tachi-compatible ranking service. Bokutachi is configured by default, but you can enter another compatible server origin. The operator of the origin you choose receives IR requests and governs its accounts, public rankings, server logs, data use, retention, security, and deletion practices under its own terms and notices.

### Ranking Requests and Account Authentication

When an IR provider is enabled and you open a chart ranking, AsoBMaShow sends the chart's cryptographic hash, game mode in the request path, and ordinary network request information to the configured provider. It then downloads the provider's chart identifier, player account names or identifiers, ranks, scores, clear statuses, judgement or timing details when available, combo and bad-point information, and achievement times for display in the app.

Public chart-resolution and leaderboard requests do not contain your API key. If you saved an API key, AsoBMaShow separately sends it to the configured provider to authenticate the provider account and obtain its numeric account identifier. The app uses that identifier to mark your row in the public ranking and may cache the identifier locally.

### Score Submission and History Reconciliation

If you manually submit a score, retry a submission, or enable automatic score submission, AsoBMaShow sends the current API key and gameplay data to the configured provider. A submission can include:

- the chart's SHA-256 or MD5 hash and the 7-key or 14-key play mode;
- the play time, EX score, and clear status;
- PGreat, Great, Good, Bad, and Poor judgement counts;
- fast/slow and early/late timing counts when available;
- maximum combo, bad points, final gauge, and sampled gauge history; and
- the name of AsoBMaShow as the submitting client.

The submission does not include your AsoBMaShow profile display name or local profile identifier, the chart or media files, a replay file, raw input events, contacts, advertising identifiers, or motion-sensor readings. The provider uses the API key to associate the submitted result with your provider account and may display the account name or identifier and gameplay result in public rankings.

If you choose to import or reconcile IR history, AsoBMaShow authenticates to the provider, downloads the account's supported score history, and stores a local projection of that history so the app can show remote records and avoid unnecessary duplicate work.

### IR Credentials, Local Queue, and Controls

The API key is stored in a separate file inside the local AsoBMaShow profile. The app masks it in the interface, excludes it from profile exports and duplication, and does not copy it into score-submission payloads, queue rows, diagnostics, or logs. AsoBMaShow uses file protections intended to restrict the credential file to the device user, but it does not store the key in Apple Keychain, Android Keystore, or another platform credential manager.

IR settings let you:

- keep the provider disabled;
- enable or disable automatic submission separately;
- submit or retry eligible results manually;
- pause provider activity by disabling the provider;
- remove the API key and locally cached provider-account evidence; and
- discard eligible pending or failed local queue entries.

Turning off automatic submission prevents new automatic submissions but does not cancel existing queued work. Disabling the provider pauses its requests but does not delete queued entries or data already accepted by the provider. Removing the API key prevents authenticated requests and clears locally imported account data and receipts where supported, but it does not delete scores already stored by the provider. Public rankings can remain available while the provider is enabled without an API key. To delete provider-held account or score data, use the provider's controls or contact its operator.

## Permissions and Device Capabilities

AsoBMaShow accesses device capabilities only to provide related features:

- **Files and folders:** The app accesses locations you select for chart libraries, imports, downloads, profile transfers, and exports. Google Play builds use Android's system document access. Some non-Play Android distributions may offer optional broad shared-storage access so that chart folders can be used directly.
- **Photos:** On iOS, Photos access is used only when you ask the app to save exported replay media.
- **Motion sensors:** Optional gyroscope and related motion readings let an iPhone, iPad, or supported Android device act as a turntable controller. Raw motion readings are processed on the device and are not sent to VioletXF or an IR provider.
- **Input devices:** MIDI, USB, keyboard, gamepad, and touch input are used for app and gameplay controls. Input bindings and processed replay events may be stored locally. Raw input events are not included in IR submissions.

You can decline or revoke optional permissions in your operating-system settings. The feature that needs the permission may then be unavailable.

## Other Network Downloads and Third-Party Services

Other network activity is initiated by features you choose to use. In particular:

- BMS search and download features may send chart hashes or title and artist search terms to third-party catalog, search, archive, or download hosts.
- Difficulty-table import sends the table URL you provide or select to the host serving that table and its related data.
- Opening an external page sends the requested address to your browser and the destination website.
- Exporting or sharing content sends the selected file to the app, person, or storage provider you choose.

Like ordinary internet services, IR providers and these other third parties may receive connection information such as your IP address, request time, requested URL, and the app's user-agent value. Their handling of that information is governed by their own privacy policies and practices. VioletXF does not control independent services or a Tachi-compatible origin that you configure. Use only providers and download sources you trust, avoid sending sensitive information in URLs, and do not configure an untrusted IR server.

## Device Backups

Your operating system or a backup provider may include local AsoBMaShow data, including local IR settings or operational data, in a device backup depending on your Apple, Google, device, and backup settings. IR API keys are excluded from AsoBMaShow profile exports, but operating-system backup behavior is controlled by the platform. You control these settings, and the provider's policies govern backup storage, retention, and deletion.

## Information You Send to Us

If you contact us by email, we receive the email address, message, and attachments you choose to send. We use them to respond, maintain necessary support records, protect our rights, resolve disputes, and comply with legal obligations. Our email provider processes this correspondence on our behalf under its own terms and privacy policy.

## Sharing and Sale of Information

We do not sell personal information. We do not use personal information for advertising, cross-app tracking, or profiling.

VioletXF does not receive your locally stored app data or IR traffic through an AsoBMaShow backend. When you enable or use IR, AsoBMaShow sends the data described above directly to the configured independent provider for authentication, rankings, score submission, submission status, or history reconciliation. The provider may make account names, identifiers, and gameplay results public as part of its ranking service. Information may otherwise go to another party only through an action you direct, through a platform or backup feature you enable, in connection with email support, or when disclosure of information in our possession is required by applicable law or necessary to protect legal rights and safety.

Service providers acting on VioletXF's behalf must protect information consistently with this policy and applicable requirements. Independent IR providers, user-configured server operators, content hosts, browsers, and user-selected sharing destinations are not service providers acting on our behalf; review their practices before sending data to them.

## Retention and Deletion

Local app data remains on your device until you delete it using available app or device controls, clear the app's data, or uninstall the app. Files you export remain where you placed or shared them until you or the recipient deletes them. Device backups may remain under the backup provider's retention rules.

Pending, deferred, blocked, and failed IR queue entries remain locally until they succeed, you discard them where that action is available, or you delete the profile or app data. Successfully completed queue rows are retained locally for status and diagnostics and are normally purged after seven days. Removing an API key clears the local credential and associated local provider-account evidence where supported. It does not delete data previously sent to the provider.

The configured IR provider determines how long it keeps API access records, submitted gameplay results, account identifiers, IP addresses, and other server records. AsoBMaShow cannot retrieve or delete provider-held data on your behalf. Use the provider's account or privacy controls or contact its operator to request access or deletion. If you configured another server, contact that server's operator.

We retain email correspondence only for as long as reasonably necessary for the purposes described above. You may request deletion of correspondence in our possession by emailing [zudevxf@gmail.com](mailto:zudevxf@gmail.com), subject to legal and legitimate recordkeeping needs.

## Security

We limit information access to what app features require and rely on operating-system and device protections for locally stored data. The default Bokutachi origin uses HTTPS, authenticated IR requests do not follow redirects, and AsoBMaShow is designed to keep API keys out of submission bodies, queue rows, exports, diagnostics, and logs.

The app permits an advanced user to configure another HTTP or HTTPS Tachi-compatible origin. The settings screen identifies an HTTP origin as insecure because an API key and gameplay data would not be protected in transit. Use HTTPS and only a server operator you trust. Independent services have their own security practices, and no storage or transmission method can be guaranteed to be completely secure.

## Children's Privacy

AsoBMaShow is intended for a general audience and is not directed specifically to children under 13. We do not knowingly collect personal information from children through an AsoBMaShow account or application backend. IR providers are separate services with their own account eligibility and privacy practices; a parent or guardian should review those practices before allowing a child to enable IR or submit a score. If you believe a child has sent personal information to us by email, contact us so we can review and, when appropriate, delete it.

## International Users

Your local app data stays on your device unless you use or enable a feature that sends it elsewhere as described in this policy. IR providers, email providers, backup providers, and third-party content services may process information in countries other than your own under their respective policies. You may contact us with questions or requests concerning information in VioletXF's possession. Contact the applicable independent provider about information it controls.

## Policy Changes

We may update this policy when the app, integrated services, or legal requirements change. We will post the revised policy with a new effective date and provide an in-app, release-note, or similarly appropriate notice when a change is material.

## Contact

For privacy questions or requests concerning AsoBMaShow or information held by VioletXF, contact:

VioletXF (Jaewoo Ahn)  
[zudevxf@gmail.com](mailto:zudevxf@gmail.com)
