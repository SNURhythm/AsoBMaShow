# App Store Support Page Design

## Context

AsoBMaShow needs a public Support URL for its App Store listing. Apple requires the Support URL to lead users to accurate contact information for app issues, feedback, and feature requests. The repository already publishes a root-level `PRIVACY_POLICY.md`, and the public GitHub repository is an appropriate low-maintenance host for a matching support document.

## Goal

Create a concise root-level `SUPPORT.md` that gives users a direct way to contact VioletXF, provides a structured public bug-reporting channel, and offers enough guidance to produce actionable support requests.

After the file is pushed to the public `develop` branch, the App Store Support URL will be:

`https://github.com/SNURhythm/AsoBMaShow/blob/develop/SUPPORT.md`

## Non-Goals

- Building or configuring a standalone support website or GitHub Pages site.
- Creating a comprehensive user manual or troubleshooting knowledge base.
- Promising a response time, service-level agreement, or guaranteed fix.
- Accepting privacy or security reports through a public issue tracker.
- Documenting unreleased online score or ranking features.

## Audience and Tone

The page is for AsoBMaShow users on iOS, iPadOS, Android, macOS, and Windows. It will use direct, friendly English and short sections that render clearly on GitHub and mobile browsers.

## Page Structure

The root `SUPPORT.md` will contain the following sections:

1. **Title and introduction**
   - Identify the page as AsoBMaShow Support.
   - Name VioletXF (Jaewoo Ahn) as the developer.
   - State that the page covers support for the app's supported platforms.

2. **Contact options**
   - Display `zudevxf@gmail.com` as a clickable email address and the primary direct contact method.
   - Link to `https://github.com/SNURhythm/AsoBMaShow/issues` for public bug reports and feature requests.
   - Direct users to email privacy, security, account-sensitive, or otherwise non-public information instead of posting it to GitHub Issues.

3. **Information to include in a support request**
   - App version or build number.
   - Device model and operating-system version.
   - Clear reproduction steps.
   - Expected and actual behavior.
   - Relevant settings, screenshots, and logs when available.
   - A warning not to include passwords, personal information, or copyrighted chart and media files they are not authorized to share.

4. **Common help**
   - Briefly point users toward checking selected chart-library folders and file permissions when charts do not appear.
   - Suggest checking the selected audio device, volume, and app permissions for audio problems.
   - Suggest reconnecting and reconfiguring keyboard, gamepad, MIDI, USB, touch, or motion-controller input when controls do not respond.
   - Keep this section general so it remains accurate across platforms and releases.

5. **Chart and media content**
   - Explain that AsoBMaShow does not include BMS charts, music, or other third-party media.
   - Ask users to use and share only content they are authorized to access.

6. **Related links**
   - Link to the repository's latest releases at `https://github.com/SNURhythm/AsoBMaShow/releases/latest`.
   - Link to the root privacy policy with the relative Markdown target `PRIVACY_POLICY.md`.

## Maintenance

The support page will avoid version-specific instructions that are likely to become stale. Contact details and links should be reviewed before each App Store submission and updated whenever the developer's support channels change.

## Verification

Before completion:

- confirm `SUPPORT.md` exists at the repository root;
- confirm the developer name, email address, GitHub Issues URL, releases URL, and privacy-policy link are exact;
- scan for placeholders such as `TBD` and `TODO`;
- run `git diff --check`; and
- inspect the rendered Markdown structure for clear headings and readable links.

## Files in Scope

- Add `SUPPORT.md` at the repository root.
- Do not modify application code, store metadata, or hosting configuration.
