package com.snurhythm.asobmashow;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Comparator;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.stream.Stream;

public final class DocumentsMutationGuardTests {
    interface Operation { void run() throws Exception; }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void rejected(Operation operation, boolean busy) throws Exception {
        try {
            operation.run();
        } catch (IOException expected) {
            if (busy) check(expected.getMessage().toLowerCase().contains("import in progress"),
                    "The provider must explain that an import is in progress");
            return;
        }
        throw new AssertionError("Conflicting mutation was not rejected");
    }

    private static void reserveAndClose(File file) throws IOException {
        try (DocumentsMutationGuard.Reservation ignored =
                     DocumentsMutationGuard.reserveNewDestination(file)) {}
    }

    private static void existing(Path root) throws Exception {
        Path file = Files.write(root.resolve("existing.bms"), new byte[]{1});
        Path directory = Files.createDirectory(root.resolve("existing"));
        Path dangling = Files.createSymbolicLink(root.resolve("dangling"), root.resolve("absent"));
        Path alias = Files.createSymbolicLink(root.resolve("alias"), directory);
        for (Path path : new Path[]{file, directory, dangling, alias}) {
            rejected(() -> reserveAndClose(path.toFile()), false);
        }
        check(Files.readAllBytes(file)[0] == 1 && Files.isDirectory(directory),
                "Rejected reservation must leave existing destinations untouched");
        reserveAndClose(root.resolve("absent").toFile());
    }

    private static void descendants(Path root) throws Exception {
        File destination = root.resolve("new-chart").toFile();
        try (DocumentsMutationGuard.Reservation lease =
                     DocumentsMutationGuard.reserveNewDestination(destination)) {
            check(lease.canonicalOutput.equals(destination.getCanonicalFile()),
                    "Import must use the reserved canonical destination");
            check(!destination.exists(), "Reservation must not create the destination");
            for (boolean recursive : new boolean[]{false, true}) {
                rejected(() -> DocumentsMutationGuard.requireUnreserved(destination, recursive), true);
                rejected(() -> DocumentsMutationGuard.requireUnreserved(
                        new File(destination, "nested/chart.bms"), recursive), true);
            }
            Files.createDirectory(destination.toPath());
            Files.write(destination.toPath().resolve("chart.bms"), new byte[]{2});
            rejected(() -> DocumentsMutationGuard.requireUnreserved(
                    new File(destination, "chart.bms"), false), true);
        }
        DocumentsMutationGuard.requireUnreserved(destination, true);
    }

    private static void ancestors(Path root) throws Exception {
        File parent = Files.createDirectory(root.resolve("BMS")).toFile();
        File destination = new File(parent, "chart");
        try (DocumentsMutationGuard.Reservation ignored =
                     DocumentsMutationGuard.reserveNewDestination(destination)) {
            rejected(() -> DocumentsMutationGuard.requireUnreserved(parent, true), true);
            rejected(() -> DocumentsMutationGuard.requireUnreserved(root.toFile(), true), true);
            DocumentsMutationGuard.requireUnreserved(parent, false);
            DocumentsMutationGuard.requireUnreserved(new File(parent, "chart-sibling"), true);
            DocumentsMutationGuard.requireUnreserved(new File(parent, "other/nested.bms"), false);
            reserveAndClose(new File(parent, "chart-sibling"));
        }
    }

    private static void overlap(Path root) throws Exception {
        File destination = root.resolve("missing/chart").toFile();
        try (DocumentsMutationGuard.Reservation ignored =
                     DocumentsMutationGuard.reserveNewDestination(destination)) {
            rejected(() -> reserveAndClose(destination), true);
            rejected(() -> reserveAndClose(root.resolve("missing").toFile()), true);
            rejected(() -> reserveAndClose(new File(destination, "nested")), true);
            rejected(() -> reserveAndClose(root.resolve("missing/../missing/chart").toFile()), true);
            Path alias = Files.createSymbolicLink(root.resolve("alias"), root);
            rejected(() -> DocumentsMutationGuard.requireUnreserved(
                    alias.resolve("missing/chart/file").toFile(), false), true);
            reserveAndClose(root.resolve("missing/chart-2").toFile());
        }
    }

    private static void caseAliases(Path root) throws Exception {
        File destination = root.resolve("BMS/Chart").toFile();
        try (DocumentsMutationGuard.Reservation ignored =
                     DocumentsMutationGuard.reserveNewDestination(destination)) {
            for (boolean recursive : new boolean[]{false, true}) {
                rejected(() -> DocumentsMutationGuard.requireUnreserved(
                        root.resolve("bms/CHART").toFile(), recursive), true);
                rejected(() -> DocumentsMutationGuard.requireUnreserved(
                        root.resolve("bms/chart/nested/song.bms").toFile(), recursive), true);
            }
            rejected(() -> DocumentsMutationGuard.requireUnreserved(
                    root.resolve("bms").toFile(), true), true);
            rejected(() -> reserveAndClose(root.resolve("bms/CHART").toFile()), true);
            rejected(() -> reserveAndClose(root.resolve("bms/chart/nested").toFile()), true);
            rejected(() -> reserveAndClose(root.resolve("bms").toFile()), true);
            DocumentsMutationGuard.requireUnreserved(root.resolve("bms").toFile(), false);
            DocumentsMutationGuard.requireUnreserved(root.resolve("bms/chart-extra").toFile(), true);
            DocumentsMutationGuard.requireUnreserved(root.resolve("bms-other/chart").toFile(), true);
            reserveAndClose(root.resolve("bms/chart-extra").toFile());
        }
        reserveAndClose(root.resolve("bms/CHART").toFile());
    }

    private static void release(Path root) throws Exception {
        File first = root.resolve("first").toFile();
        File second = root.resolve("second").toFile();
        try (DocumentsMutationGuard.Reservation other =
                     DocumentsMutationGuard.reserveNewDestination(second)) {
            DocumentsMutationGuard.Reservation lease =
                    DocumentsMutationGuard.reserveNewDestination(first);
            lease.close();
            try (DocumentsMutationGuard.Reservation replacement =
                         DocumentsMutationGuard.reserveNewDestination(first)) {
                lease.close();
                rejected(() -> DocumentsMutationGuard.requireUnreserved(first, true), true);
                rejected(() -> DocumentsMutationGuard.requireUnreserved(second, true), true);
            }
            try {
                try (DocumentsMutationGuard.Reservation ignored =
                             DocumentsMutationGuard.reserveNewDestination(first)) {
                    throw new IOException("copy cancelled");
                }
            } catch (IOException expected) {
                check("copy cancelled".equals(expected.getMessage()), "Original error is preserved");
            }
            DocumentsMutationGuard.requireUnreserved(first, true);
            reserveAndClose(first);
            rejected(() -> DocumentsMutationGuard.requireUnreserved(second, false), true);
        }
    }

    private static void blockedBySharedMonitor(Operation operation) throws Exception {
        AtomicReference<Throwable> failure = new AtomicReference<>();
        CountDownLatch entered = new CountDownLatch(1);
        Thread worker = new Thread(() -> {
            entered.countDown();
            try { operation.run(); } catch (Throwable error) { failure.set(error); }
        });
        synchronized (DocumentsMutationGuard.LOCK) {
            worker.start();
            check(entered.await(2, TimeUnit.SECONDS), "Worker must start");
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
            while (worker.isAlive() && worker.getState() != Thread.State.BLOCKED
                    && System.nanoTime() < deadline) Thread.yield();
            check(worker.getState() == Thread.State.BLOCKED,
                    "Guard operation must acquire the shared provider/import monitor");
        }
        worker.join(2000);
        check(!worker.isAlive(), "Guard operation must finish once monitor is released");
        if (failure.get() != null) throw new AssertionError(failure.get());
    }

    private static void monitor(Path root) throws Exception {
        File destination = root.resolve("chart").toFile();
        blockedBySharedMonitor(() -> reserveAndClose(destination));
        blockedBySharedMonitor(() -> DocumentsMutationGuard.requireUnreserved(destination, true));
        DocumentsMutationGuard.Reservation lease =
                DocumentsMutationGuard.reserveNewDestination(destination);
        blockedBySharedMonitor(lease::close);
        reserveAndClose(destination);
    }

    private static void concurrent(Path root) throws Exception {
        CountDownLatch start = new CountDownLatch(1);
        DocumentsMutationGuard.Reservation[] leases = new DocumentsMutationGuard.Reservation[2];
        Throwable[] errors = new Throwable[2];
        Thread[] workers = new Thread[2];
        for (int index = 0; index < 2; ++index) {
            final int slot = index;
            workers[index] = new Thread(() -> {
                try {
                    start.await();
                    leases[slot] = DocumentsMutationGuard.reserveNewDestination(root.resolve("chart").toFile());
                } catch (Throwable error) { errors[slot] = error; }
            });
            workers[index].start();
        }
        start.countDown();
        for (Thread worker : workers) {
            worker.join(2000);
            check(!worker.isAlive(), "Concurrent reservations must finish");
        }
        try {
            check((leases[0] == null) != (leases[1] == null), "Exactly one importer owns the destination");
            int rejected = leases[0] == null ? 0 : 1;
            check(errors[rejected] instanceof IOException,
                    "The competing importer receives a checked conflict error");
        } finally {
            for (DocumentsMutationGuard.Reservation lease : leases) if (lease != null) lease.close();
        }
        reserveAndClose(root.resolve("chart").toFile());
    }

    public static void main(String[] args) throws Exception {
        Path root = Files.createTempDirectory("documents-mutation-guard-");
        try {
            switch (args[0]) {
                case "existing": existing(root); break;
                case "descendants": descendants(root); break;
                case "ancestors": ancestors(root); break;
                case "overlap": overlap(root); break;
                case "case-aliases": caseAliases(root); break;
                case "release": release(root); break;
                case "monitor": monitor(root); break;
                case "concurrent": concurrent(root); break;
                default: throw new AssertionError("Unknown scenario: " + args[0]);
            }
        } finally {
            try (Stream<Path> paths = Files.walk(root)) {
                for (Path path : paths.sorted(Comparator.reverseOrder()).toArray(Path[]::new)) {
                    Files.deleteIfExists(path);
                }
            }
        }
    }
}
