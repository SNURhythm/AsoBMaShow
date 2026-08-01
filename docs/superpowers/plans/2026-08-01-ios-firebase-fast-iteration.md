# iOS Firebase Fast Iteration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let PR-to-`develop` iOS Firebase distribution run without scheduling or waiting for the full release verifier while preserving the verifier as a required TestFlight dependency.

**Architecture:** Keep the existing three explicit iOS jobs. Give `ios-verify` an event guard that excludes the PR-to-`develop` Firebase event, remove only the `ios-firebase` dependency edge, and retain `ios-testflight -> ios-verify` unchanged. Protect the job graph with the existing workflow contract suite.

**Tech Stack:** GitHub Actions YAML, Python `unittest`, shell syntax validation

## Global Constraints

- Marketing version remains `0.0.1`.
- The app deployment target remains iOS/iPadOS 14.
- `NSAllowsArbitraryLoads = true` remains enabled.
- No `PrivacyInfo.xcprivacy` manifest is added.
- Do not invoke Firebase or TestFlight distribution while implementing or verifying this change.
- Do not change Fastlane lane routing or TestFlight serialization.

---

### Task 1: Separate the Firebase iteration graph from the TestFlight release gate

**Files:**

- Modify: `tests/ios_release_workflow_tests.py`
- Modify: `.github/workflows/mobile-beta-deploy.yml`
- Modify: `docs/release-readiness-audit-2026-08-01.md`

**Interfaces:**

- Consumes: GitHub event fields `github.event_name` and `github.base_ref`; existing jobs `ios-verify`, `ios-firebase`, and `ios-testflight`.
- Produces: a PR-to-`develop` graph with only the Firebase distribution job eligible, plus a TestFlight graph that still requires successful `ios-verify` completion.

- [x] **Step 1: Write the failing workflow graph test**

Replace the test that requires both distribution jobs to depend on verification with:

```python
def test_firebase_pr_bypasses_release_verification_but_testflight_does_not(self):
    verify = self.workflow.split("  ios-verify:", 1)[1].split(
        "  ios-firebase:", 1
    )[0]
    firebase = self.workflow.split("  ios-firebase:", 1)[1].split(
        "  ios-testflight:", 1
    )[0]
    testflight = self.workflow.split("  ios-testflight:", 1)[1].split(
        "  android-firebase:", 1
    )[0]

    self.assertIn("./scripts/ios_release_verify.sh", verify)
    self.assertIn(
        "github.event_name != 'pull_request' || github.base_ref != 'develop'",
        verify,
    )
    self.assertNotIn("needs: ios-verify", firebase)
    self.assertIn("needs: ios-verify", testflight)
```

- [x] **Step 2: Run the focused test and verify RED**

Run: `python3 tests/ios_release_workflow_tests.py`

Expected: FAIL because `ios-verify` has no Firebase-event exclusion and `ios-firebase` still contains `needs: ios-verify`.

- [x] **Step 3: Implement the minimal workflow graph**

Add this job-level condition to `ios-verify`:

```yaml
if: ${{ github.event_name != 'pull_request' || github.base_ref != 'develop' }}
```

Remove only this line from `ios-firebase`:

```yaml
needs: ios-verify
```

Keep `ios-testflight` and its `needs: ios-verify` dependency unchanged.

- [x] **Step 4: Reconcile the release audit**

Update the release-engineering remediation and remaining-risk language so it states that Firebase PR distribution intentionally bypasses the release verifier for iteration, while TestFlight remains gated. Do not change the recorded test counts or artifact results.

- [x] **Step 5: Run focused verification and verify GREEN**

Run:

```bash
python3 tests/ios_release_workflow_tests.py
bash -n scripts/ios_release_verify.sh scripts/ios_firebase_deploy.sh
scripts/ios_release_verify.sh --dry-run
```

Expected: all eight workflow tests pass, shell syntax passes, and the dry-run lists build/audit commands without invoking a distribution lane.

- [x] **Step 6: Review and commit**

Run:

```bash
git diff --check
git status -sb
```

Confirm the diff changes only the planned workflow contract, workflow graph, plan/design documentation, and release-audit wording. Commit with:

```bash
git add .github/workflows/mobile-beta-deploy.yml tests/ios_release_workflow_tests.py docs/release-readiness-audit-2026-08-01.md docs/superpowers/plans/2026-08-01-ios-firebase-fast-iteration.md
git commit -m "ci(ios): speed up Firebase iteration"
```

- [x] **Step 7: Publish the PR update**

Push the existing branch and verify PR #89 points at the new commit. Do not wait for or trigger a distribution run manually.
