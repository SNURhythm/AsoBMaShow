# Internet Ranking Privacy Policy Update Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Update AsoBMaShow's public privacy policy so it accurately describes the shipped Bokutachi/Tachi Internet Ranking feature and supports matching App Store privacy disclosures.

**Architecture:** Keep `PRIVACY_POLICY.md` as the single public policy for all supported platforms. Distinguish local-first app data from optional IR data sent to an independent provider, and describe provider enablement, automatic/manual submission, public ranking reads, credentials, local queue/cache retention, remote retention/deletion, and user controls.

**Tech Stack:** Markdown, repository source/configuration audit, Apple App Privacy taxonomy, Git verification

## Global Constraints

- Developer: VioletXF (Jaewoo Ahn)
- Privacy contact: zudevxf@gmail.com
- Effective date: July 29, 2026
- Platforms: iOS, Android, macOS, and Windows
- IR is disabled by default and uses Bokutachi at `https://boku.tachi.ac` by default.
- Users may configure another Tachi-compatible origin, including an insecure HTTP origin; the policy must not imply that every origin is controlled by VioletXF or protected by HTTPS.
- VioletXF does not operate the default IR service and does not receive IR credentials, submissions, or ranking requests through an AsoBMaShow backend.
- The policy must not claim that provider-side retention or deletion is controlled by AsoBMaShow.
- Do not describe Firebase build distribution as in-app collection.

---

### Task 1: Reconcile the policy with the shipped IR data flow

**Files:**
- Modify: `PRIVACY_POLICY.md`
- Reference: `src/ir/IrProfileSettings.h`
- Reference: `src/ir/IrCredentialStore.cpp`
- Reference: `src/ir/tachi/TachiBatchManual.cpp`
- Reference: `src/ir/tachi/TachiDriver.cpp`
- Reference: `src/scene/SettingsSceneIr.cpp`

**Interfaces:**
- Consumes: The implemented provider defaults, credential handling, ranking requests, score payload, queue retention, and user controls.
- Produces: A public policy that no longer says online scores/rankings are absent and that distinguishes local data, IR provider data, and VioletXF-held support correspondence.

- [x] **Step 1: Update the current-state summary and effective date**

Change the effective date to July 29, 2026. State that there are no AsoBMaShow accounts, ads, tracking, analytics, crash reporting, or developer-operated app backend, while identifying optional Internet Ranking as the exception to local-only score handling.

- [x] **Step 2: Add the exact Internet Ranking disclosures**

Identify Bokutachi/Tachi as the default independent provider; explain that enabling rankings sends chart hashes and request metadata, an API key authenticates the provider account, submissions send the chart hash, play time, score, clear status, judgements, timing, combo, bad-point, and gauge data, and the provider may associate and publicly display the score with its account name or ID. Explain that history reconciliation downloads provider-held scores to local storage.

- [x] **Step 3: Document IR storage, recipients, retention, deletion, security, and controls**

Explain local credential storage, exclusion from exports, queued-submission retention and discard controls, seven-day successful-row cleanup, provider-controlled remote retention/deletion, disabling/removing-key behavior, default HTTPS, the insecure custom-HTTP warning, and ordinary connection metadata such as IP address and user agent.

- [x] **Step 4: Remove obsolete future-service claims and reconcile every affected section**

Update the network, sharing, retention, security, children, and international-transfer sections. Replace the future-online-feature section with a general policy-changes section.

- [x] **Step 5: Verify policy content and repository hygiene**

Run:

```bash
rg -n "July 29, 2026|Internet Ranking|Bokutachi|Tachi|API key|chart hash|gameplay|seven days|disable|Remove Key|independent" PRIVACY_POLICY.md
! rg -n "does not provide.*online score|future online score|no remote AsoBMaShow account data|TBD|TODO|FIXME|PLACEHOLDER" PRIVACY_POLICY.md
git diff --check
git diff -- PRIVACY_POLICY.md
git status --short
```

Expected: the active IR service is fully disclosed, obsolete claims and placeholders are absent, the Markdown diff has no whitespace errors, and only the plan plus intended policy file are modified.

- [x] **Step 6: Review against current Apple requirements**

Confirm that the handoff instructs the App Store Connect owner to declare `User ID` and `Gameplay Content`, both used for `App Functionality`, linked to the user, and not used for tracking; to update and publish the privacy responses; and to use the public policy URL after the branch reaches the public repository.
