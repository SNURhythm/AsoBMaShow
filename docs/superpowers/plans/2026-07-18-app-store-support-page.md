# App Store Support Page Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a public, low-maintenance AsoBMaShow support document that supplies the contact information and support channels required for the App Store Support URL.

**Architecture:** Add one root-level Markdown document rendered by GitHub. The page uses email for direct, private, security, and privacy contact; GitHub Issues for public bug reports and feature requests; and stable repository links for releases and the privacy policy.

**Tech Stack:** Markdown, GitHub-rendered repository pages, shell verification with `test`, `rg`, `curl`, and Git.

## Global Constraints

- Create only the root-level `SUPPORT.md`; do not modify application code, App Store metadata, or hosting configuration.
- Use `VioletXF (Jaewoo Ahn)` as the developer name and `zudevxf@gmail.com` as the primary direct contact address.
- Link public bug reports and feature requests to `https://github.com/SNURhythm/AsoBMaShow/issues`.
- Send privacy, security, or other sensitive reports to email rather than the public issue tracker.
- Link releases to `https://github.com/SNURhythm/AsoBMaShow/releases/latest` and privacy information to the relative target `PRIVACY_POLICY.md`.
- Do not promise a response time, service-level agreement, or guaranteed fix.
- Do not document unreleased online score or ranking features.
- Use `[skip ci]` in the implementation commit message.

---

### Task 1: Add the App Store Support Page

**Files:**
- Create: `SUPPORT.md`
- Reference: `PRIVACY_POLICY.md`
- Reference: `docs/superpowers/specs/2026-07-18-app-store-support-page-design.md`

**Interfaces:**
- Consumes: the existing public GitHub repository, its Issues and Releases pages, the root privacy policy, and the confirmed developer contact details.
- Produces: `SUPPORT.md`, publicly addressable after push as `https://github.com/SNURhythm/AsoBMaShow/blob/develop/SUPPORT.md`.

- [ ] **Step 1: Verify the support document is absent**

Run:

```bash
test -f SUPPORT.md
```

Expected: exit status `1` because the support document has not been created.

- [ ] **Step 2: Create the exact support document**

Create `SUPPORT.md` with this content:

```markdown
# AsoBMaShow Support

AsoBMaShow is a cross-platform BMS player for iOS, iPadOS, Android, macOS, and Windows. It is developed by VioletXF (Jaewoo Ahn).

## Contact

For direct support, email [zudevxf@gmail.com](mailto:zudevxf@gmail.com).

For public bug reports and feature requests, open an issue in the [AsoBMaShow issue tracker](https://github.com/SNURhythm/AsoBMaShow/issues).

Email us instead of opening a public issue if your message concerns privacy or security, or contains information that should not be public. Do not post passwords, personal information, or other sensitive data in GitHub Issues.

## What to Include

To help us understand a problem, include as much of the following as you can:

- the AsoBMaShow version or build number;
- your device model and operating-system version;
- the steps that reproduce the problem;
- what you expected to happen and what happened instead;
- relevant app settings; and
- screenshots or logs, when available.

Do not send copyrighted charts, music, videos, or other media unless you are authorized to share them.

## Common Help

### Charts Do Not Appear

Confirm that the intended chart-library folder is selected and that AsoBMaShow still has permission to access it. If folder access changed, add the folder again and refresh the library.

### Audio Does Not Play

Check the device and app volume, the selected audio output when applicable, and any operating-system permissions needed by the feature you are using.

### Controls Do Not Respond

Reconnect the input device and review its bindings in AsoBMaShow. For keyboard, gamepad, MIDI, USB, touch, or motion-controller input, also confirm that the operating system recognizes the device and that any required permission is enabled.

## Chart and Media Content

AsoBMaShow does not include BMS charts, music, videos, or other third-party media. Use and share only content that you are authorized to access.

## Related Links

- [Latest releases](https://github.com/SNURhythm/AsoBMaShow/releases/latest)
- [Privacy Policy](PRIVACY_POLICY.md)
```

- [ ] **Step 3: Verify the required support information**

Run:

```bash
test -f SUPPORT.md
test -f PRIVACY_POLICY.md
rg -n '^# AsoBMaShow Support$' SUPPORT.md
rg -n 'VioletXF \(Jaewoo Ahn\)' SUPPORT.md
rg -n 'mailto:zudevxf@gmail\.com' SUPPORT.md
rg -n 'https://github\.com/SNURhythm/AsoBMaShow/issues' SUPPORT.md
rg -n 'https://github\.com/SNURhythm/AsoBMaShow/releases/latest' SUPPORT.md
rg -n '\[Privacy Policy\]\(PRIVACY_POLICY\.md\)' SUPPORT.md
```

Expected: every command exits `0`; `rg` prints the matching line for each required heading, contact detail, and link.

- [ ] **Step 4: Verify there are no unfinished placeholders or malformed whitespace**

Run:

```bash
if rg -n 'TBD|TODO|FIXME|PLACEHOLDER' SUPPORT.md; then
  exit 1
fi
git diff --check
```

Expected: exit status `0` with no placeholder matches and no whitespace errors.

- [ ] **Step 5: Verify the external GitHub destinations are reachable**

Run:

```bash
curl -LfsS -o /dev/null https://github.com/SNURhythm/AsoBMaShow/issues
curl -LfsS -o /dev/null https://github.com/SNURhythm/AsoBMaShow/releases/latest
```

Expected: both commands exit `0`. The Support URL itself will become reachable only after `SUPPORT.md` is pushed to the public `develop` branch.

- [ ] **Step 6: Review the complete document diff**

Run:

```bash
git diff -- SUPPORT.md
git status --short
```

Expected: the diff contains exactly the approved support-page copy, and `git status --short` lists only `SUPPORT.md` as untracked.

- [ ] **Step 7: Commit the support document**

Run:

```bash
git add SUPPORT.md
git diff --cached --check
git diff --cached --name-only
git commit -m "docs: add App Store support page [skip ci]"
```

Expected: the staged name list contains only `SUPPORT.md`, and the commit succeeds with one created file.

- [ ] **Step 8: Verify the committed result**

Run:

```bash
git show --stat --oneline --summary HEAD
git status --short
```

Expected: the newest commit is `docs: add App Store support page [skip ci]`, its stat lists only `SUPPORT.md`, and the working tree is clean.
