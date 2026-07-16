# iOS Project Structure and Incremental Build Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the iOS app target discover supported `src/` files automatically and make local/Firebase PR builds reuse stable, checkout-scoped Xcode intermediates.

**Architecture:** Register the existing synchronized `src/` folder directly on the Xcode app target and retain only the Android source exclusion. Replace the tracked Pods symlink with a timestamp-preserving restore into a normal ignored directory, keep one relative workspace reference, and route Fastlane plus build-only through one checkout-hashed DerivedData resolver.

**Tech Stack:** Xcode 26 project format, Bash, Python 3 `unittest`, CocoaPods, Fastlane 2.229.1, Git.

## Global Constraints

- New supported source files under `src/` must join the iOS app target without editing `project.pbxproj`.
- `AndroidNatives.cpp` must remain outside the iOS target.
- `audio/AudioWrapper.cpp` must retain its explicit Objective-C++ file type.
- Existing frameworks, libraries, resources, build settings, signing, and export behavior must remain unchanged.
- TestFlight builds must continue to use `clean: true`.
- Firebase PR archives and `scripts/ios_firebase_deploy.sh --build-only` must retain a stable DerivedData directory and reuse `ArchiveIntermediates` or normal build intermediates within the same checkout.
- Separate checkouts and Git worktrees must not share DerivedData by default.
- `IOS_DERIVED_DATA_PATH` must remain an explicit override.
- CocoaPods installation must remain lockfile-controlled and must not place a tracked machine-specific path in the repository.
- Verification must not upload a build.

---

### Task 1: Adopt native synchronized source membership and a portable workspace

**Files:**
- Create: `tests/ios_build_setup_tests.py`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj:709-918,1207-1226`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcworkspace/contents.xcworkspacedata:1-11`
- Delete: `ios/Xcode/AsoBMaShow/Pods`
- Modify: `scripts/check_application_startup_gate.sh:102-110`
- Modify: `scripts/check_result_persistence_flow.py:135-140,583-589`

**Interfaces:**
- Consumes: Xcode object IDs `B76AAF3F2DA4A1C400E8327C` for the synchronized `src/` group and `B70027002BF7A8D8000DB8EC` for the app target.
- Produces: Target-wide synchronized source discovery, one `AndroidNatives.cpp` exception, one relative Pods workspace reference, and `IOSBuildSetupTests` repository guards.

- [ ] **Step 1: Write failing project-structure tests**

Create `tests/ios_build_setup_tests.py`:

```python
#!/usr/bin/env python3
import subprocess
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / "ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj"
WORKSPACE = ROOT / "ios/Xcode/AsoBMaShow/AsoBMaShow.xcworkspace/contents.xcworkspacedata"
SRC_GROUP_ID = "B76AAF3F2DA4A1C400E8327C"
EXCEPTION_SET_ID = "B76AAF692DA4A1C400E8327C"
TARGET_ID = "B70027002BF7A8D8000DB8EC"


def object_block(project: str, object_id: str, next_section: str) -> str:
    start = project.index(f"\t\t{object_id}")
    end = project.index(next_section, start)
    return project[start:end]


class IOSBuildSetupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.project = PROJECT.read_text(encoding="utf-8")

    def test_app_target_owns_synchronized_src_group(self):
        target = object_block(
            self.project, TARGET_ID, "/* End PBXNativeTarget section */"
        )
        self.assertIn("fileSystemSynchronizedGroups = (", target)
        self.assertIn(SRC_GROUP_ID, target)

    def test_android_native_is_only_source_membership_exception(self):
        exceptions = object_block(
            self.project,
            EXCEPTION_SET_ID,
            "/* End PBXFileSystemSynchronizedBuildFileExceptionSet section */",
        )
        start = exceptions.index("membershipExceptions = (")
        end = exceptions.index("\n\t\t\t);", start)
        paths = [
            line.strip().removesuffix(",")
            for line in exceptions[start:end].splitlines()[1:]
            if line.strip()
        ]
        self.assertEqual(["AndroidNatives.cpp"], paths)
        self.assertIn(f"target = {TARGET_ID}", exceptions)

    def test_audio_wrapper_keeps_objective_cpp_override(self):
        group = object_block(
            self.project,
            SRC_GROUP_ID,
            "/* End PBXFileSystemSynchronizedRootGroup section */",
        )
        self.assertIn(
            "audio/AudioWrapper.cpp = sourcecode.cpp.objcpp;", group
        )

    def test_workspace_has_one_relative_pods_project(self):
        tree = ET.parse(WORKSPACE)
        locations = [node.attrib["location"] for node in tree.findall("FileRef")]
        pods_locations = [value for value in locations if "Pods.xcodeproj" in value]
        self.assertEqual(["group:Pods/Pods.xcodeproj"], pods_locations)

    def test_pods_path_is_not_tracked(self):
        result = subprocess.run(
            ["git", "ls-files", "--", "ios/Xcode/AsoBMaShow/Pods"],
            cwd=ROOT,
            check=True,
            text=True,
            capture_output=True,
        )
        self.assertEqual("", result.stdout.strip())


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the tests and verify the current structure fails**

Run:

```bash
python3 tests/ios_build_setup_tests.py IOSBuildSetupTests -v
```

Expected: FAIL because the target lacks `fileSystemSynchronizedGroups`, the exception list contains 188 paths, the workspace contains two Pods references, and `Pods` is tracked.

- [ ] **Step 3: Convert the Xcode target to synchronized membership**

In `project.pbxproj`, replace the contents of the existing `membershipExceptions` list with:

```text
			membershipExceptions = (
				AndroidNatives.cpp,
			);
```

Add this field to the `AsoBMaShow` `PBXNativeTarget` after `dependencies`:

```text
			fileSystemSynchronizedGroups = (
				B76AAF3F2DA4A1C400E8327C /* ../../../../src */,
			);
```

Do not change the root group's `explicitFileTypes` map.

- [ ] **Step 4: Remove machine-specific CocoaPods project entries**

Delete the tracked `ios/Xcode/AsoBMaShow/Pods` symlink. Replace the workspace file with:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<Workspace
   version = "1.0">
   <FileRef
      location = "group:AsoBMaShow.xcodeproj">
   </FileRef>
   <FileRef
      location = "group:Pods/Pods.xcodeproj">
   </FileRef>
</Workspace>
```

- [ ] **Step 5: Update feature audits to rely on synchronized membership**

In `scripts/check_application_startup_gate.sh`, replace the two per-source iOS membership assertions with:

```bash
require_count ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj \
  'fileSystemSynchronizedGroups = \(' 1 \
  "iOS app target synchronized source membership"
```

In `scripts/check_result_persistence_flow.py`, replace the final per-file iOS assertion with:

```python
require(
    ios_project.count("fileSystemSynchronizedGroups = (") == 1
    and "B76AAF3F2DA4A1C400E8327C /* ../../../../src */" in ios_project,
    "iOS target must compile the synchronized src folder",
)
```

- [ ] **Step 6: Run the focused and existing audits**

Run:

```bash
python3 tests/ios_build_setup_tests.py IOSBuildSetupTests -v
scripts/check_application_startup_gate.sh
scripts/check_result_persistence_flow.sh
```

Expected: all tests and audits PASS.

- [ ] **Step 7: Commit the portable Xcode structure**

```bash
git add tests/ios_build_setup_tests.py \
  ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj \
  ios/Xcode/AsoBMaShow/AsoBMaShow.xcworkspace/contents.xcworkspacedata \
  scripts/check_application_startup_gate.sh \
  scripts/check_result_persistence_flow.py
git rm ios/Xcode/AsoBMaShow/Pods
git commit -m "build(ios): adopt synchronized source folder"
```

### Task 2: Isolate and centralize reusable DerivedData

**Files:**
- Create: `scripts/ios_derived_data_path.sh`
- Modify: `tests/ios_build_setup_tests.py`
- Modify: `scripts/ios_firebase_deploy.sh:178-189`
- Modify: `ios/Xcode/AsoBMaShow/fastlane/Fastfile:1-2,61-66`
- Modify: `scripts/ios_firebase_deploy.env.example:41-42`

**Interfaces:**
- Consumes: `IOS_DERIVED_DATA_PATH`, `HOME`, and the canonical repository root.
- Produces: Executable `scripts/ios_derived_data_path.sh [--root PATH]`, printing exactly one DerivedData path; both Fastlane Firebase archives and build-only consume it.

- [ ] **Step 1: Add failing DerivedData resolver tests**

Add these imports to `tests/ios_build_setup_tests.py`:

```python
import os
import tempfile
```

Add these constants below the existing constants:

```python
DERIVED_DATA_HELPER = ROOT / "scripts/ios_derived_data_path.sh"
DEPLOY_SCRIPT = ROOT / "scripts/ios_firebase_deploy.sh"
FASTFILE = ROOT / "ios/Xcode/AsoBMaShow/fastlane/Fastfile"
```

Add this class before the `if __name__` block:

```python
class DerivedDataPathTests(unittest.TestCase):
    def resolve(self, root: Path, **environment: str) -> str:
        env = os.environ.copy()
        env.update(environment)
        return subprocess.run(
            [str(DERIVED_DATA_HELPER), "--root", str(root)],
            cwd=ROOT,
            env=env,
            check=True,
            text=True,
            capture_output=True,
        ).stdout.strip()

    def test_explicit_override_is_returned_exactly(self):
        self.assertEqual(
            "/tmp/custom-ios-derived-data",
            self.resolve(ROOT, IOS_DERIVED_DATA_PATH="/tmp/custom-ios-derived-data"),
        )

    def test_checkout_path_is_stable_and_isolated(self):
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp)
            first = parent / "checkout-a"
            second = parent / "checkout-b"
            first.mkdir()
            second.mkdir()
            home = parent / "home"
            home.mkdir()
            first_path = self.resolve(first, HOME=str(home), IOS_DERIVED_DATA_PATH="")
            self.assertEqual(
                first_path,
                self.resolve(first, HOME=str(home), IOS_DERIVED_DATA_PATH=""),
            )
            self.assertNotEqual(
                first_path,
                self.resolve(second, HOME=str(home), IOS_DERIVED_DATA_PATH=""),
            )
            self.assertTrue(
                first_path.startswith(
                    str(home / "Library/Developer/Xcode/DerivedData/AsoBMaShow-FirebaseCI-")
                )
            )

    def test_fastlane_and_build_only_share_resolver(self):
        self.assertIn("ios_derived_data_path.sh", DEPLOY_SCRIPT.read_text())
        fastfile = FASTFILE.read_text()
        self.assertIn("ios_derived_data_path.sh", fastfile)
        self.assertIn("clean: !distribute_to_firebase", fastfile)
```

- [ ] **Step 2: Run the tests and verify the resolver is missing**

Run:

```bash
python3 tests/ios_build_setup_tests.py DerivedDataPathTests -v
```

Expected: ERROR for the missing helper and FAIL because the current callers contain independent defaults.

- [ ] **Step 3: Implement the shared path resolver**

Create executable `scripts/ios_derived_data_path.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"

if [ "$#" -gt 0 ]; then
  if [ "$#" -ne 2 ] || [ "$1" != "--root" ]; then
    echo "Usage: scripts/ios_derived_data_path.sh [--root PATH]" >&2
    exit 2
  fi
  ROOT_DIR="$2"
fi

if [ -n "${IOS_DERIVED_DATA_PATH:-}" ]; then
  printf '%s\n' "${IOS_DERIVED_DATA_PATH}"
  exit 0
fi

CANONICAL_ROOT="$(cd "${ROOT_DIR}" && pwd -P)"
ROOT_HASH="$(printf '%s' "${CANONICAL_ROOT}" | shasum -a 256 | awk '{ print substr($1, 1, 12) }')"
[ -n "${ROOT_HASH}" ] || { echo "Unable to hash repository root" >&2; exit 1; }

printf '%s\n' "${HOME}/Library/Developer/Xcode/DerivedData/AsoBMaShow-FirebaseCI-${ROOT_HASH}"
```

Run `chmod +x scripts/ios_derived_data_path.sh`.

- [ ] **Step 4: Route build-only and Fastlane through the helper**

In `scripts/ios_firebase_deploy.sh`, replace the local default in `run_build_only` with:

```bash
  local derived_data_path
  derived_data_path="$("${ROOT_DIR}/scripts/ios_derived_data_path.sh")"
```

At the top of `fastlane/Fastfile`, add:

```ruby
require "open3"
```

Replace `firebase_derived_data_path` with:

```ruby
def firebase_derived_data_path
  repository_root = File.expand_path("../../../..", __dir__)
  helper = File.join(repository_root, "scripts", "ios_derived_data_path.sh")
  output, error, status = Open3.capture3(helper)
  UI.user_error!("Unable to resolve iOS DerivedData path: #{error.strip}") unless status.success?

  path = output.strip
  UI.user_error!("iOS DerivedData path resolver returned an empty path") if path.empty?
  path
end
```

In `scripts/ios_firebase_deploy.env.example`, replace the assigned global default with an optional override example:

```bash
# Optional. The default is stable per checkout/worktree.
# IOS_DERIVED_DATA_PATH="${HOME}/Library/Developer/Xcode/DerivedData/custom-asobmashow"
```

- [ ] **Step 5: Run resolver and Ruby syntax tests**

Run:

```bash
python3 tests/ios_build_setup_tests.py DerivedDataPathTests -v
bash -n scripts/ios_derived_data_path.sh scripts/ios_firebase_deploy.sh
ruby_version="$(tr -d '[:space:]' < ios/Xcode/AsoBMaShow/.ruby-version)"
(
  cd ios/Xcode/AsoBMaShow
  PATH="${HOME}/.asdf/installs/ruby/${ruby_version}/bin:${PATH}" \
    bundle exec ruby -c fastlane/Fastfile
)
```

Expected: all tests PASS and Ruby reports `Syntax OK`.

- [ ] **Step 6: Commit DerivedData isolation**

```bash
git add scripts/ios_derived_data_path.sh \
  scripts/ios_firebase_deploy.sh \
  scripts/ios_firebase_deploy.env.example \
  ios/Xcode/AsoBMaShow/fastlane/Fastfile \
  tests/ios_build_setup_tests.py
git commit -m "build(ios): isolate incremental DerivedData"
```

### Task 3: Restore CocoaPods cache into a normal local directory

**Files:**
- Create: `scripts/ios_pods_cache.sh`
- Modify: `tests/ios_build_setup_tests.py`
- Modify: `scripts/ios_init.sh:1-81`

**Interfaces:**
- Consumes: A lock-hashed external Pods cache, local `Podfile.lock`, and a generated local `Pods/` directory.
- Produces: `ios_pods_cache_valid PODS_DIR LOCKFILE`, `ios_pods_cache_restore CACHE_DIR PODS_DIR LOCKFILE`, and `ios_pods_cache_store PODS_DIR CACHE_DIR LOCKFILE`; `ios_init.sh` uses these without symlinking Pods.

- [ ] **Step 1: Add failing real-filesystem cache tests**

Add this constant to `tests/ios_build_setup_tests.py`:

```python
PODS_CACHE_HELPER = ROOT / "scripts/ios_pods_cache.sh"
IOS_INIT = ROOT / "scripts/ios_init.sh"
```

Add this test class before the `if __name__` block:

```python
class PodsCacheTests(unittest.TestCase):
    def bash(self, command: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["bash", "-c", f'source "{PODS_CACHE_HELPER}"; {command}'],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )

    @staticmethod
    def make_valid_pods(directory: Path, lock: Path, marker: str) -> None:
        (directory / "Pods.xcodeproj").mkdir(parents=True)
        (directory / "Pods.xcodeproj/project.pbxproj").write_text(
            "// generated pods project\n", encoding="utf-8"
        )
        (directory / "Manifest.lock").write_bytes(lock.read_bytes())
        (directory / "marker.txt").write_text(marker, encoding="utf-8")

    def test_restore_creates_real_local_directory_and_preserves_timestamp(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            lock = root / "Podfile.lock"
            lock.write_text("PODS:\n", encoding="utf-8")
            cache = root / "cache"
            local = root / "Pods"
            self.make_valid_pods(cache, lock, "cached")
            marker_time = 1_700_000_000
            os.utime(cache / "marker.txt", (marker_time, marker_time))

            result = self.bash(
                f'ios_pods_cache_restore "{cache}" "{local}" "{lock}"'
            )
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertTrue(local.is_dir())
            self.assertFalse(local.is_symlink())
            self.assertEqual("cached", (local / "marker.txt").read_text())
            self.assertEqual(marker_time, int((local / "marker.txt").stat().st_mtime))

    def test_invalid_source_does_not_replace_existing_cache(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            lock = root / "Podfile.lock"
            lock.write_text("PODS:\n", encoding="utf-8")
            source = root / "Pods"
            source.mkdir()
            cache = root / "cache"
            self.make_valid_pods(cache, lock, "preserved")

            result = self.bash(
                f'ios_pods_cache_store "{source}" "{cache}" "{lock}"'
            )
            self.assertNotEqual(0, result.returncode)
            self.assertEqual("preserved", (cache / "marker.txt").read_text())

    def test_ios_init_uses_copy_cache_instead_of_pods_symlink(self):
        script = IOS_INIT.read_text(encoding="utf-8")
        self.assertIn("ios_pods_cache_restore", script)
        self.assertIn("ios_pods_cache_store", script)
        self.assertNotIn('link_cache_dir "${IOS_DIR}/Pods"', script)
```

- [ ] **Step 2: Run the tests and verify the cache helper is missing**

Run:

```bash
python3 tests/ios_build_setup_tests.py PodsCacheTests -v
```

Expected: FAIL because `ios_pods_cache.sh` does not exist and `ios_init.sh` still symlinks `Pods`.

- [ ] **Step 3: Implement atomic, timestamp-preserving cache restore/store**

Create executable `scripts/ios_pods_cache.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

ios_pods_cache_valid() {
  [ "$#" -eq 2 ] || return 2
  local pods_dir="$1"
  local lock_file="$2"

  [ -d "${pods_dir}" ] &&
    [ ! -L "${pods_dir}" ] &&
    [ -f "${pods_dir}/Manifest.lock" ] &&
    [ -f "${pods_dir}/Pods.xcodeproj/project.pbxproj" ] &&
    cmp -s "${lock_file}" "${pods_dir}/Manifest.lock"
}

ios_pods_cache_restore() {
  [ "$#" -eq 3 ] || return 2
  local cache_dir="$1"
  local pods_dir="$2"
  local lock_file="$3"
  local staging="${pods_dir}.restore.$$"

  ios_pods_cache_valid "${cache_dir}" "${lock_file}" || return 1
  rm -rf "${staging}"
  mkdir -p "${staging}"
  if ! rsync -a --delete "${cache_dir}/" "${staging}/" ||
     ! ios_pods_cache_valid "${staging}" "${lock_file}"; then
    rm -rf "${staging}"
    return 1
  fi

  rm -rf "${pods_dir}"
  mv "${staging}" "${pods_dir}"
}

ios_pods_cache_store() {
  [ "$#" -eq 3 ] || return 2
  local pods_dir="$1"
  local cache_dir="$2"
  local lock_file="$3"
  local staging="${cache_dir}.tmp.$$"
  local previous="${cache_dir}.previous.$$"

  ios_pods_cache_valid "${pods_dir}" "${lock_file}" || return 1
  mkdir -p "$(dirname "${cache_dir}")"
  rm -rf "${staging}" "${previous}"
  mkdir -p "${staging}"
  if ! rsync -a --delete "${pods_dir}/" "${staging}/" ||
     ! ios_pods_cache_valid "${staging}" "${lock_file}"; then
    rm -rf "${staging}"
    return 1
  fi

  if [ -e "${cache_dir}" ] || [ -L "${cache_dir}" ]; then
    mv "${cache_dir}" "${previous}"
  fi
  if mv "${staging}" "${cache_dir}"; then
    rm -rf "${previous}"
    return 0
  fi

  rm -rf "${staging}"
  if [ -e "${previous}" ] || [ -L "${previous}" ]; then
    mv "${previous}" "${cache_dir}"
  fi
  return 1
}
```

Run `chmod +x scripts/ios_pods_cache.sh`.

- [ ] **Step 4: Integrate the copy cache into `ios_init.sh`**

After `CACHE_ROOT`, source the helper:

```bash
# shellcheck source=scripts/ios_pods_cache.sh
source "${ROOT_DIR}/scripts/ios_pods_cache.sh"
```

Replace `install_pods` with:

```bash
install_pods() {
  local pod_lock_hash
  local gem_lock_hash
  local pods_cache

  pod_lock_hash="$(shasum "${IOS_DIR}/Podfile.lock" | awk '{ print $1 }')"
  gem_lock_hash="$(shasum "${IOS_DIR}/Gemfile.lock" | awk '{ print $1 }')"
  pods_cache="${CACHE_ROOT}/pods/${pod_lock_hash}-${gem_lock_hash}"

  if ios_pods_cache_restore "${pods_cache}" "${IOS_DIR}/Pods" \
      "${IOS_DIR}/Podfile.lock"; then
    echo "Using cached CocoaPods install: ${pods_cache}"
    return 0
  fi

  rm -rf "${IOS_DIR}/Pods"
  cd "${IOS_DIR}"
  bundle exec pod install --deployment
  if ! ios_pods_cache_valid "${IOS_DIR}/Pods" "${IOS_DIR}/Podfile.lock"; then
    echo "CocoaPods installation is incomplete" >&2
    return 1
  fi
  if ! ios_pods_cache_store "${IOS_DIR}/Pods" "${pods_cache}" \
      "${IOS_DIR}/Podfile.lock"; then
    echo "Unable to refresh CocoaPods cache: ${pods_cache}" >&2
    return 1
  fi
}
```

Keep `link_cache_dir` for `vendor/bundle`; remove only its use for `Pods`.

- [ ] **Step 5: Run the cache and shell syntax tests**

Run:

```bash
python3 tests/ios_build_setup_tests.py PodsCacheTests -v
bash -n scripts/ios_pods_cache.sh scripts/ios_init.sh
```

Expected: all tests PASS.

- [ ] **Step 6: Commit the portable Pods cache**

```bash
git add scripts/ios_pods_cache.sh scripts/ios_init.sh tests/ios_build_setup_tests.py
git commit -m "build(ios): restore Pods cache portably"
```

### Task 4: Update guidance and verify true incremental builds

**Files:**
- Modify: `tests/ios_build_setup_tests.py`
- Modify: `AGENTS.md:17-25`

**Interfaces:**
- Consumes: All outputs from Tasks 1-3.
- Produces: Accurate agent guidance and verified zero-application-recompile behavior for the second unchanged build.

- [ ] **Step 1: Add a failing documentation regression test**

Add this constant to `tests/ios_build_setup_tests.py`:

```python
AGENT_GUIDANCE = ROOT / "AGENTS.md"
```

Add this method to `IOSBuildSetupTests`:

```python
    def test_agent_guidance_describes_automatic_ios_sources(self):
        guidance = AGENT_GUIDANCE.read_text(encoding="utf-8")
        self.assertNotIn("add its path to `membershipExceptions`", guidance)
        self.assertIn("automatically discovers supported files under `src`", guidance)
        self.assertIn("checkout-specific DerivedData", guidance)
```

- [ ] **Step 2: Run the documentation test and verify the old instruction fails**

Run:

```bash
python3 tests/ios_build_setup_tests.py \
  IOSBuildSetupTests.test_agent_guidance_describes_automatic_ios_sources -v
```

Expected: FAIL because `AGENTS.md` still requires manual `membershipExceptions` edits.

- [ ] **Step 3: Replace the obsolete iOS build guidance**

Replace the manual source-list bullet in `AGENTS.md` with:

```markdown
- The iOS app target uses an Xcode file-system-synchronized group and
  automatically discovers supported files under `src`. Keep platform-only
  exclusions in the synchronized group's small `membershipExceptions` list;
  do not add normal new source files there.
- Firebase and local build-only runs use checkout-specific DerivedData so one
  checkout can build incrementally without colliding with other Git worktrees.
  `IOS_DERIVED_DATA_PATH` remains available as an explicit override.
```

- [ ] **Step 4: Run all fast repository guards**

Run:

```bash
python3 tests/ios_build_setup_tests.py -v
scripts/check_application_startup_gate.sh
scripts/check_result_persistence_flow.sh
git diff --check
```

Expected: all tests/audits PASS and `git diff --check` prints nothing.

- [ ] **Step 5: Verify `ios_init.sh` is project-file-idempotent**

Run:

```bash
ruby_version="$(tr -d '[:space:]' < ios/Xcode/AsoBMaShow/.ruby-version)"
export PATH="${HOME}/.asdf/installs/ruby/${ruby_version}/bin:${PATH}"
project=ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
workspace=ios/Xcode/AsoBMaShow/AsoBMaShow.xcworkspace/contents.xcworkspacedata
project_before="$(stat -f '%m:%z' "${project}")"
workspace_before="$(stat -f '%m:%z' "${workspace}")"
scripts/ios_init.sh
scripts/ios_init.sh
test "${project_before}" = "$(stat -f '%m:%z' "${project}")"
test "${workspace_before}" = "$(stat -f '%m:%z' "${workspace}")"
test -d ios/Xcode/AsoBMaShow/Pods
test ! -L ios/Xcode/AsoBMaShow/Pods
```

Expected: both initializations succeed, tracked project/workspace metadata stays unchanged, and local `Pods` is a real directory.

- [ ] **Step 6: Verify a second build compiles no application sources**

Run the sanctioned non-uploading build twice:

```bash
verification_root="$(mktemp -d /tmp/asobmashow-ios-derived-root.XXXXXX)"
export IOS_DERIVED_DATA_PATH="$(scripts/ios_derived_data_path.sh --root "${verification_root}")"
scripts/ios_firebase_deploy.sh --build-only --skip-init 2>&1 | tee /tmp/asobmashow-ios-first.log
scripts/ios_firebase_deploy.sh --build-only --skip-init 2>&1 | tee /tmp/asobmashow-ios-second.log
app_compile_count="$({ rg 'CompileC .*AsoBMaShow\.build.*AsoBMaShow/src/' /tmp/asobmashow-ios-second.log || true; } | wc -l | tr -d ' ')"
test "${app_compile_count}" = "0"
rm -rf "${IOS_DERIVED_DATA_PATH}" "${verification_root}"
unset IOS_DERIVED_DATA_PATH
```

Expected: both builds report `BUILD SUCCEEDED`; the second log contains zero app-target `CompileC` actions. Always-run dependency and packaging phases may still execute.

- [ ] **Step 7: Run the desktop compile verification**

```bash
cmake --build cmake-build-debug --target main -j 6
```

Expected: target `main` builds successfully.

- [ ] **Step 8: Commit guidance and final verification guards**

```bash
git add AGENTS.md tests/ios_build_setup_tests.py
git commit -m "docs(ios): document automatic source discovery"
```

- [ ] **Step 9: Confirm the branch is clean and review its diff**

```bash
git status --short
git diff --stat develop...HEAD
git log --oneline develop..HEAD
```

Expected: clean status and four implementation commits after the design/plan commits.
