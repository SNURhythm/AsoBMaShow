# Android Document Rollback Coordinator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Guarantee that every touched, originally empty Android export destination gets one rollback attempt before its same-URI write lease is released, including when the bounded rollback queue is saturated or executor submission throws.

**Architecture:** Add a package-private coordinator as the sole owner of the restore registry, executor, and lease terminal state. The Activity acquires and resolves coordinator leases but cannot complete registry tickets directly. A single daemon worker retains the bounded queue, with unconditional caller-runs backpressure and a run-once synchronous fallback at the executor boundary.

**Tech Stack:** Java 8-compatible Android source on Java 17, Android `ContentResolver`, `java.util.concurrent`, JUnit 4.13.2, Gradle 8.7, CMake/CTest.

## Global Constraints

- Work from `/Users/xf/workspace/SNURhythm/AsoBMaShow` on `chore/refactor-2`.
- Do not edit generated `src/bms_parser.hpp` or `src/bms_parser.cpp`.
- Do not deploy to Firebase or run a release upload.
- Keep the coordinator free of Android APIs so local JVM tests can execute it.
- Use latches and captured runnables for concurrency control; do not use sleeps.
- Preserve the existing provider policy: only a destination verified empty may be opened, and a failed restore still releases the lease so later non-empty checks can refuse overwrite.
- Use `apply_patch` for source edits and inspect existing user changes before each patch.

The Android test commands below assume this environment bootstrap in the same shell:

```zsh
set -a
source .env
if [[ -f .env.local ]]; then source .env.local; fi
if [[ -f android/.env ]]; then source android/.env; fi
if [[ -f android/.env.local ]]; then source android/.env.local; fi
set +a
export VCPKG_ROOT=/Users/xf/vcpkg
export JAVA_HOME=$(/usr/libexec/java_home -v 17)
export PATH="$JAVA_HOME/bin:$PATH"
```

---

## Task 1: Specify and implement the rollback coordinator

**Files:**

- Create: `android/app/src/test/java/com/snurhythm/asobmashow/DocumentHandoffRollbackCoordinatorTest.java`
- Create: `android/app/src/main/java/com/snurhythm/asobmashow/DocumentHandoffRollbackCoordinator.java`
- Read only: `android/app/src/main/java/com/snurhythm/asobmashow/DocumentHandoffRestoreRegistry.java`

- [ ] **Step 1: Add focused failing coordinator tests**

Create `DocumentHandoffRollbackCoordinatorTest` in package `com.snurhythm.asobmashow`. Use package access to the coordinator constructor and API. Add these deterministic tests:

1. `finishReleasesLeaseAndDuplicateResolutionCannotReleaseNewerLease`
   - Construct with `Runnable::run` and a thread-safe failure list.
   - Acquire the first lease, call `finish` twice, then acquire a second lease for the same URI.
   - Call `rollback` on the already finished first lease and assert its action count remains zero.
   - Start a daemon waiter for a third lease. Its cancellation supplier must signal a latch while it observes the still-pending second lease and wait on another latch before returning `false`.
   - Assert the third lease has not been acquired, release the supplier, finish the second lease, then assert the waiter acquires and finishes the third lease.

2. `sameUriWaiterAcquiresOnlyAfterRollbackTaskFinishes`
   - Inject an executor that captures one `Runnable` without executing it.
   - Acquire a lease, request rollback, and start a same-URI waiter using the latch-controlled cancellation supplier described above.
   - Assert the waiter is inside the registry's pending loop and has not acquired.
   - Release the supplier, execute the captured rollback runnable, and assert the waiter then acquires.

3. `falseAndThrownRestoreFailuresAreReportedAndReleaseLeases`
   - With a direct executor, return `false` from one restore and throw a known `IOException` from another.
   - Assert the false result reports an `IOException`, the thrown instance is reported unchanged, and both URI keys can be acquired again afterward.

4. `defaultExecutorRunsOverflowRollbackOnCallingThread`
   - Use `createDefault` so the test covers the real one-worker/eight-slot production configuration.
   - Block the first rollback action on a latch, enqueue eight distinct-URI rollback actions, and submit a tenth action.
   - Assert the tenth action runs before the call returns and records `Thread.currentThread()` as its execution thread.
   - Release the worker and await all eight queued actions in `finally`.

5. `executorThrowAfterRetainingTaskRunsRestoreOnlyOnce`
   - Inject an executor that stores the command and then throws `RejectedExecutionException`.
   - Assert fallback performs the restore once and releases the lease.
   - Run the stored command afterward and assert the restore count is still one, proving the task-level guard is separate from lease resolution.

Use daemon waiter threads, bounded latch waits of five seconds, and `finally` blocks that release every blocking latch. A helper that acquires on a daemon thread and returns through `AtomicReference` should fail with a bounded timeout instead of hanging the Gradle worker.

- [ ] **Step 2: Run the focused test and confirm the RED failure**

Run:

```zsh
SDL/android-project/gradlew -p android testPlayDebugUnitTest \
  --tests 'com.snurhythm.asobmashow.DocumentHandoffRollbackCoordinatorTest' \
  --no-daemon
```

Expected: Java compilation fails with `cannot find symbol: class DocumentHandoffRollbackCoordinator`. If it fails for environment setup instead, correct the Java/VCPKG/SDK environment and rerun until the failure is specifically the missing production class.

- [ ] **Step 3: Implement the minimal pure-Java coordinator**

Create `DocumentHandoffRollbackCoordinator.java` with this API and lifecycle:

```java
package com.snurhythm.asobmashow;

import java.io.IOException;
import java.util.Objects;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.Executor;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;

/** Owns Android provider write leases through terminal rollback or completion. */
final class DocumentHandoffRollbackCoordinator {
    private static final int DEFAULT_QUEUE_CAPACITY = 8;

    @FunctionalInterface
    interface RestoreAction {
        boolean restore() throws Exception;
    }

    static final class Lease {
        private final DocumentHandoffRestoreRegistry.Ticket ticket;
        private final AtomicBoolean resolutionStarted = new AtomicBoolean(false);

        private Lease(DocumentHandoffRestoreRegistry.Ticket ticket) {
            this.ticket = ticket;
        }

        private boolean beginResolution() {
            return resolutionStarted.compareAndSet(false, true);
        }
    }

    private final DocumentHandoffRestoreRegistry registry;
    private final Executor executor;
    private final Consumer<Throwable> errorSink;

    static DocumentHandoffRollbackCoordinator createDefault(
            Consumer<Throwable> errorSink) {
        ThreadPoolExecutor executor = new ThreadPoolExecutor(
                1,
                1,
                0L,
                TimeUnit.MILLISECONDS,
                new ArrayBlockingQueue<>(DEFAULT_QUEUE_CAPACITY),
                runnable -> {
                    Thread thread = new Thread(
                            runnable, "AsoBMaShow-document-rollback");
                    thread.setDaemon(true);
                    return thread;
                },
                (task, ignored) -> task.run());
        return new DocumentHandoffRollbackCoordinator(
                new DocumentHandoffRestoreRegistry(), executor, errorSink);
    }

    DocumentHandoffRollbackCoordinator(
            DocumentHandoffRestoreRegistry registry,
            Executor executor,
            Consumer<Throwable> errorSink) {
        this.registry = Objects.requireNonNull(registry, "registry");
        this.executor = Objects.requireNonNull(executor, "executor");
        this.errorSink = Objects.requireNonNull(errorSink, "errorSink");
    }

    Lease acquire(String key, BooleanSupplier cancelled)
            throws InterruptedException {
        DocumentHandoffRestoreRegistry.Ticket ticket =
                registry.acquire(key, cancelled);
        return ticket == null ? null : new Lease(ticket);
    }

    void finish(Lease lease) {
        if (lease != null && lease.beginResolution()) {
            registry.complete(lease.ticket);
        }
    }

    void rollback(Lease lease, RestoreAction restoreAction) {
        if (lease == null) {
            return;
        }
        Objects.requireNonNull(restoreAction, "restoreAction");
        if (!lease.beginResolution()) {
            return;
        }

        AtomicBoolean attempted = new AtomicBoolean(false);
        Runnable task = () -> {
            if (!attempted.compareAndSet(false, true)) {
                return;
            }
            try {
                try {
                    if (!restoreAction.restore()) {
                        report(new IOException(
                                "The empty export destination could not be restored."));
                    }
                } catch (Exception failure) {
                    report(failure);
                }
            } finally {
                registry.complete(lease.ticket);
            }
        };

        try {
            executor.execute(task);
        } catch (RuntimeException ignored) {
            task.run();
        }
    }

    private void report(Throwable failure) {
        try {
            errorSink.accept(failure);
        } catch (RuntimeException ignored) {
            // Diagnostics must never strand a provider write lease.
        }
    }
}
```

The two atomics serve different invariants: `resolutionStarted` arbitrates `finish` versus `rollback`, while `attempted` prevents a retained executor command and synchronous fallback from running the restore twice.

- [ ] **Step 4: Run the focused coordinator tests and confirm GREEN**

Run the same focused Gradle command from Step 2.

Expected: `DocumentHandoffRollbackCoordinatorTest` passes with no hangs and `BUILD SUCCESSFUL`.

- [ ] **Step 5: Run the existing registry tests as a primitive regression check**

```zsh
SDL/android-project/gradlew -p android testPlayDebugUnitTest \
  --tests 'com.snurhythm.asobmashow.DocumentHandoffRestoreRegistryTest' \
  --no-daemon
```

Expected: all existing restore-registry tests pass.

- [ ] **Step 6: Commit the coordinator and tests**

```zsh
git add \
  android/app/src/main/java/com/snurhythm/asobmashow/DocumentHandoffRollbackCoordinator.java \
  android/app/src/test/java/com/snurhythm/asobmashow/DocumentHandoffRollbackCoordinatorTest.java
git commit -m "fix: guarantee Android document rollback scheduling"
```

---

## Task 2: Make the coordinator the Activity's single rollback authority

**Files:**

- Modify: `android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowActivity.java:1-105`
- Modify: `android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowActivity.java:829-915`
- Modify: `android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowActivity.java:2066-2086`

- [ ] **Step 1: Replace the Activity-owned registry and executor**

Add `import android.util.Log;`. Remove the rollback-only imports `ArrayBlockingQueue`, `Executor`, `ThreadPoolExecutor`, and `TimeUnit`; retain `ConcurrentHashMap`, `CountDownLatch`, and `AtomicReference`.

Add a private Activity tag and replace both old static fields with:

```java
private static final String TAG = "AsoBMaShow";
private static final DocumentHandoffRollbackCoordinator
        DOCUMENT_HANDOFF_ROLLBACKS =
        DocumentHandoffRollbackCoordinator.createDefault(
                failure -> Log.e(
                        TAG,
                        "Could not restore an empty export destination.",
                        failure));
```

Do not log the destination URI or provider document id.

- [ ] **Step 2: Convert export ownership from Ticket to Lease**

In `exportDocument`, change the local ticket to:

```java
DocumentHandoffRollbackCoordinator.Lease uriWriteLease = null;
```

Acquire through the coordinator:

```java
uriWriteLease = DOCUMENT_HANDOFF_ROLLBACKS.acquire(
        destinationKey,
        () -> selection.operation.cancelled);
if (uriWriteLease == null) {
    throw new DocumentHandoffCancelledException();
}
```

On the successful native/activity commit path, finish the lease before returning:

```java
DOCUMENT_HANDOFF_ROLLBACKS.finish(uriWriteLease);
return SUCCESS_RESULT;
```

Replace the failure/cancellation terminal block with exactly one ownership branch:

```java
if (restoreEmpty && uriWriteLease != null) {
    restoreEmptyDocument(selection.uri, uriWriteLease);
} else if (uriWriteLease != null) {
    DOCUMENT_HANDOFF_ROLLBACKS.finish(uriWriteLease);
}
```

The Activity must not finish the lease after giving it to `rollback`.

- [ ] **Step 3: Replace the boolean async helper with a restore action**

Delete `restoreEmptyDocumentAsync`. Add:

```java
private void restoreEmptyDocument(
        Uri uri, DocumentHandoffRollbackCoordinator.Lease lease) {
    ContentResolver resolver =
            getApplicationContext().getContentResolver();
    DOCUMENT_HANDOFF_ROLLBACKS.rollback(lease, () -> {
        try (OutputStream output = resolver.openOutputStream(uri, "wt")) {
            if (output == null) {
                return false;
            }
            output.flush();
            return true;
        }
    });
}
```

This maps a null provider stream to a reported coordinator failure and lets provider exceptions reach the injected error sink. The method name intentionally omits `Async` because queue pressure may run it synchronously on the export thread.

- [ ] **Step 4: Verify the source-of-truth boundary before compiling**

Run:

```zsh
rg -n 'DocumentHandoffRestoreRegistry|DOCUMENT_HANDOFF_ROLLBACK_EXECUTOR|restoreEmptyDocumentAsync' \
  android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowActivity.java
```

Expected: no matches.

Then run:

```zsh
rg -n 'DOCUMENT_HANDOFF_ROLLBACKS|uriWriteLease|restoreEmptyDocument' \
  android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowActivity.java
```

Expected: one static coordinator, one lease lifecycle in `exportDocument`, and the new restore helper.

- [ ] **Step 5: Force Java compilation and run all Android unit tests**

```zsh
SDL/android-project/gradlew -p android compilePlayDebugJavaWithJavac \
  --rerun-tasks --no-daemon
SDL/android-project/gradlew -p android testPlayDebugUnitTest --no-daemon
```

Expected: Activity compilation succeeds and the complete Android JVM suite passes.

- [ ] **Step 6: Commit the Activity integration**

```zsh
git add android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowActivity.java
git commit -m "refactor: centralize Android rollback ownership"
```

---

## Task 3: Verify the cross-platform document handoff boundary

**Files:**

- Verify only: `cmake-build-debug`
- Verify only: all files changed since `b211cd4`

- [ ] **Step 1: Run the focused native document-handoff test**

```zsh
ctest --test-dir cmake-build-debug \
  --output-on-failure \
  -R '^foundation_platform_document_handoff$'
```

Expected: the focused test passes.

- [ ] **Step 2: Run the complete desktop test suite**

```zsh
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: all configured desktop tests pass.

- [ ] **Step 3: Compile the desktop application target**

```zsh
cmake --build cmake-build-debug --target main -j 6
```

Expected: target `main` builds successfully.

- [ ] **Step 4: Review the final diff and whitespace**

```zsh
git diff --check b211cd4..HEAD
git diff --stat b211cd4..HEAD
git status --short --branch
```

Expected: no whitespace errors; only the design/plan, coordinator, coordinator tests, and Activity integration are in scope; the worktree is clean after commits.

- [ ] **Step 5: Request independent specification and code-quality review**

Ask one reviewer to verify every design acceptance criterion against the diff and tests. Ask a separate reviewer to inspect concurrency, executor rejection behavior, Android lifecycle/capture safety, and test determinism. Address any high-confidence issues, rerun the affected checks, and commit corrections before reporting completion.
