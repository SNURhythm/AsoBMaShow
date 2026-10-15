package com.snurhythm.asobmashow;

import static org.junit.Assert.*;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;
import org.junit.Test;

public final class NativePcmMusicPlayerTest {
    static final class Source implements NativePcmMusicPlayer.Source {
        volatile boolean enabled, closed, pending = true;
        volatile int failure, reads;
        long frames = 100;
        volatile long lastRead = -1, lastSeek = -1;
        CountDownLatch entered, unblock;
        public int read(long frame, short[] data) {
            reads++;
            lastRead = frame;
            if (entered != null) {
                entered.countDown();
                try { unblock.await(1, TimeUnit.SECONDS); } catch (InterruptedException e) { }
                entered = null;
            }
            if (failure != 0) return failure;
            if (pending) return 0;
            if (frame >= frames) return NativePcmMusicPlayer.END;
            int count = (int) Math.min(frames - frame, data.length / 2);
            for (int i = 0; i < count; i++) data[i * 2] = data[i * 2 + 1] = (short) (frame + i);
            return count;
        }
        public long durationFrames() { return frames; }
        public String error() { return "render failed"; }
        public void enable(boolean value) { enabled = value; }
        public void seek(long frame) { lastSeek = frame; }
        public void close() { closed = true; }
    }
    static final class Output implements NativePcmMusicPlayer.Output {
        volatile long head, written;
        volatile boolean playing, closed;
        volatile int writeLimit = Integer.MAX_VALUE;
        volatile int writeCalls, headCalls;
        long capacity = Integer.MAX_VALUE;
        volatile float speed = 1;
        volatile boolean stretch;
        final java.util.List<Short> samples = new java.util.concurrent.CopyOnWriteArrayList<>();
        public int write(short[] data, int offset, int count) {
            writeCalls++;
            count = Math.min(count, writeLimit);
            if (count > 0) count = (int) Math.min(count, Math.max(0, capacity - written + head) * 2);
            if (count < 0) return count;
            for (int i = offset; i < offset + count; i += 2) samples.add(data[i]);
            written += count / 2;
            return count;
        }
        public long headFrames() { headCalls++; return head; }
        public void play() { playing = true; }
        public void pause() { playing = false; }
        public void flush() { head = written = 0; }
        public void rate(float speed, boolean stretch) { this.speed = speed; this.stretch = stretch; playing = true; }
        public void end() { }
        public void close() { closed = true; }
    }
    static final class Events implements NativePcmMusicPlayer.Listener {
        volatile int finished, errors;
        volatile long lastGeneration;
        public void changed(long generation) { }
        public void finished(long generation) { lastGeneration = generation; finished++; }
        public void error(long generation, String message) { errors++; }
    }
    static void await(BooleanSupplier condition) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
        while (!condition.getAsBoolean() && System.nanoTime() < deadline) Thread.sleep(2);
        assertTrue("Timed out waiting for stream state", condition.getAsBoolean());
    }
    @Test public void pendingDoesNotCompleteOrAdvanceTheHardwareClock() throws Exception {
        Source source = new Source(); Output output = new Output(); Events events = new Events();
        NativePcmMusicPlayer player = new NativePcmMusicPlayer(source, output, events);
        try {
            player.play(); await(() -> source.lastRead == 0);
            Thread.sleep(40);
            assertEquals(0, player.positionFrames()); assertEquals(0, events.finished);
            source.pending = false; await(() -> output.written == 100);
            assertEquals(0, player.positionFrames()); assertEquals(0, events.finished);
            output.head = 100; await(() -> events.finished == 1);
            assertEquals(100, player.positionFrames()); assertFalse(player.isPlaying());
        } finally { player.close(); }
    }
    @Test public void pauseAndStopDisableProductionButRemainRestartable() throws Exception {
        Source source = new Source(); Output output = new Output(); Events events = new Events();
        NativePcmMusicPlayer player = new NativePcmMusicPlayer(source, output, events);
        try {
            source.pending = false; player.play(); await(() -> output.written == 100);
            output.head = 40; player.pause();
            assertFalse(source.enabled); assertEquals(40, player.positionFrames());
            player.play(); assertTrue(source.enabled);
            player.stop(); assertFalse(source.enabled); assertEquals(0, player.positionFrames());
            player.play(); await(() -> output.written == 100); assertTrue(source.enabled);
        } finally { player.close(); }
    }
    @Test public void seekDiscardsAnInFlightReadWithoutBlockingControls() throws Exception {
        Source source = new Source(); Output output = new Output(); Events events = new Events();
        source.pending = false; source.entered = new CountDownLatch(1); source.unblock = new CountDownLatch(1);
        CountDownLatch entered = source.entered;
        NativePcmMusicPlayer player = new NativePcmMusicPlayer(source, output, events);
        try {
            player.play(); assertTrue(entered.await(1, TimeUnit.SECONDS));
            player.seek(80); assertEquals(80, player.positionFrames());
            assertEquals("seek must notify the source before the old read finishes", 80, source.lastSeek);
            source.unblock.countDown(); await(() -> output.written == 20);
            assertEquals(80, player.positionFrames());
            output.head = 20; await(() -> events.finished == 1);
            assertEquals(100, player.positionFrames());
        } finally { player.close(); }
    }
    @Test public void renderErrorsNeverBecomeCompletion() throws Exception {
        Source source = new Source(); Output output = new Output(); Events events = new Events();
        source.failure = NativePcmMusicPlayer.ERROR;
        NativePcmMusicPlayer player = new NativePcmMusicPlayer(source, output, events);
        try {
            player.play(); await(() -> events.errors == 1);
            assertFalse(player.isPlaying()); assertEquals(0, events.finished); assertFalse(source.enabled);
        } finally { player.close(); }
    }
    @Test public void closingDuringReadReleasesResourcesWithoutPublishingEvents() throws Exception {
        Source source = new Source(); Output output = new Output(); Events events = new Events();
        source.entered = new CountDownLatch(1); source.unblock = new CountDownLatch(1);
        CountDownLatch entered = source.entered;
        NativePcmMusicPlayer player = new NativePcmMusicPlayer(source, output, events);
        player.play(); assertTrue(entered.await(1, TimeUnit.SECONDS));
        player.close(); source.unblock.countDown(); await(() -> source.closed && output.closed);
        assertEquals(0, events.finished); assertEquals(0, events.errors);
    }
    @Test public void completedTrackCanReplayAndOldCompletionIsInvalidAfterSeek() throws Exception {
        Source source = new Source(); Output output = new Output(); Events events = new Events();
        source.pending = false;
        NativePcmMusicPlayer player = new NativePcmMusicPlayer(source, output, events);
        try {
            player.play(); await(() -> output.written == 100);
            output.head = 100; await(() -> events.finished == 1);
            long oldGeneration = events.lastGeneration;
            player.play(); await(() -> output.written == 100 && player.isPlaying());
            assertEquals(0, player.positionFrames());
            assertFalse(player.acceptsEvent(oldGeneration));
            output.head = 100; await(() -> events.finished == 2);
            oldGeneration = events.lastGeneration;
            player.stop(); assertFalse(player.acceptsEvent(oldGeneration));
        } finally { player.close(); }
    }

    @Test public void completedTrackPausesOutputAndLeavesTheFeederAsleep() throws Exception {
        Source source = new Source(); Output output = new Output(); Events events = new Events();
        source.pending = false;
        NativePcmMusicPlayer player = new NativePcmMusicPlayer(source, output, events);
        try {
            player.play(); await(() -> output.written == 100);
            assertTrue(output.playing);
            output.head = 100; await(() -> events.finished == 1);
            assertFalse(output.playing); assertFalse(source.enabled);
            int reads = source.reads, writes = output.writeCalls, heads = output.headCalls;
            Thread.sleep(100);
            assertEquals(reads, source.reads);
            assertEquals(writes, output.writeCalls);
            assertEquals(heads, output.headCalls);
        } finally { player.close(); }
    }

    @Test public void terminalSourceStatesPauseAndReleaseOutput() throws Exception {
        for (int status : new int[] {NativePcmMusicPlayer.ERROR, NativePcmMusicPlayer.CANCELLED}) {
            Source source = new Source(); Output output = new Output(); Events events = new Events();
            source.failure = status;
            NativePcmMusicPlayer player = new NativePcmMusicPlayer(source, output, events);
            player.play(); await(() -> source.closed && output.closed);
            assertFalse(output.playing); assertFalse(source.enabled);
            assertFalse(player.isPlaying()); assertEquals(0, events.finished);
        }
    }

    @Test public void partialWritesPreserveEveryFrameAndRateChangesKeepPause() throws Exception {
        Source source = new Source(); Output output = new Output(); Events events = new Events();
        source.pending = false; output.writeLimit = 14;
        NativePcmMusicPlayer player = new NativePcmMusicPlayer(source, output, events);
        try {
            player.rate(1.5f, true);
            assertFalse(output.playing); assertFalse(source.enabled);
            assertEquals(1.5f, output.speed, 0); assertTrue(output.stretch);
            player.play(); await(() -> output.written == 100);
            assertEquals(100, output.samples.size());
            for (int i = 0; i < 100; i++) assertEquals((short) i, (short) output.samples.get(i));
            output.head = 25;
            player.rate(0.75f, false);
            assertEquals(25, player.positionFrames()); assertTrue(player.isPlaying());
            assertEquals(0.75f, output.speed, 0); assertFalse(output.stretch);
        } finally { player.close(); }
    }
    @Test public void audioDeviceWriteFailureIsAnErrorAndReleasesTheSource() throws Exception {
        Source source = new Source(); Output output = new Output(); Events events = new Events();
        source.pending = false; output.writeLimit = -6;
        NativePcmMusicPlayer player = new NativePcmMusicPlayer(source, output, events);
        player.play(); await(() -> events.errors == 1 && source.closed);
        assertFalse(player.isPlaying()); assertEquals(0, events.finished); assertTrue(output.closed);
    }

    @Test public void fullOutputDoesNotRetryEveryFiveMilliseconds() throws Exception {
        Source source = new Source(); Output output = new Output(); Events events = new Events();
        source.pending = false; source.frames = 16384; output.capacity = 4096;
        NativePcmMusicPlayer player = new NativePcmMusicPlayer(source, output, events);
        try {
            player.play(); await(() -> output.written == 4096 && output.writeCalls >= 2);
            int calls = output.writeCalls;
            Thread.sleep(250);
            assertTrue("A full output should batch retries; calls=" + (output.writeCalls - calls),
                    output.writeCalls - calls < 15);
            assertEquals(4096, output.written);
            player.seek(12000); await(() -> output.written == 4096);
            assertEquals(12000, (short) output.samples.get(4096));
        } finally { player.close(); }
    }

    @Test public void queuedEndDoesNotPollThePlaybackHeadEveryFiveMilliseconds() throws Exception {
        Source source = new Source(); Output output = new Output(); Events events = new Events();
        source.pending = false; source.frames = 4096;
        NativePcmMusicPlayer player = new NativePcmMusicPlayer(source, output, events);
        try {
            player.play(); await(() -> output.headCalls > 0);
            int calls = output.headCalls;
            Thread.sleep(250);
            assertTrue("Queued EOF should wait for buffered audio; calls=" + (output.headCalls - calls),
                    output.headCalls - calls < 15);
            assertEquals(0, events.finished);
            output.head = 4096; await(() -> events.finished == 1);
            assertEquals(4096, player.positionFrames());
        } finally { player.close(); }
    }

    @Test public void pendingSourceRetriesAreBoundedAndRecover() throws Exception {
        Source source = new Source(); Output output = new Output(); Events events = new Events();
        NativePcmMusicPlayer player = new NativePcmMusicPlayer(source, output, events);
        try {
            player.play(); await(() -> source.reads > 0);
            int reads = source.reads;
            Thread.sleep(250);
            assertTrue("An empty source should not be polled at 200 Hz; reads=" + (source.reads - reads),
                    source.reads - reads < 15);
            source.pending = false; await(() -> output.written == 100);
            output.head = 100; await(() -> events.finished == 1);
        } finally { player.close(); }
    }

    @Test public void refillCadenceFollowsPlaybackSpeedAndPauseStopsPolling() throws Exception {
        Source source = new Source(); Output output = new Output(); Events events = new Events();
        source.pending = false; source.frames = 16384; output.capacity = 1323;
        NativePcmMusicPlayer player = new NativePcmMusicPlayer(source, output, events);
        try {
            player.rate(0.5f, true); player.play(); await(() -> output.written == 1323);
            int calls = output.writeCalls;
            Thread.sleep(300);
            int slowCalls = output.writeCalls - calls;
            player.rate(2.0f, false);
            calls = output.writeCalls;
            Thread.sleep(300);
            int fastCalls = output.writeCalls - calls;
            assertTrue("Refill cadence must follow playback speed: slow=" + slowCalls + ", fast=" + fastCalls,
                    fastCalls > slowCalls * 2);
            player.pause(); calls = output.writeCalls;
            Thread.sleep(70);
            assertEquals("Paused output must not be polled", calls, output.writeCalls);
            player.close(); await(() -> output.closed && source.closed);
            assertEquals(0, events.errors);
        } finally { player.close(); }
    }

}
