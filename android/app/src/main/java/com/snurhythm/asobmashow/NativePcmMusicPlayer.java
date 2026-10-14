package com.snurhythm.asobmashow;

final class NativePcmMusicPlayer {
    static final int END = -1, CANCELLED = -2, ERROR = -3;
    interface Source {
        int read(long frame, short[] data);
        long durationFrames();
        String error();
        void enable(boolean value);
        void close();
    }
    interface Output {
        int write(short[] data, int offset, int count);
        long headFrames();
        void play();
        void pause();
        void flush();
        void rate(float speed, boolean stretch);
        void end();
        void close();
    }
    interface Listener { void changed(long generation); void finished(long generation); void error(long generation, String message); }
    private final Source source;
    private final Output output;
    private final Listener listener;
    private boolean playing, failed, completed;
    private volatile boolean closed;
    private long generation, baseFrame, submitted, duration;
    private long lastHead, headWrap;

    NativePcmMusicPlayer(Source source, Output output, Listener listener) {
        this.source = source;
        this.output = output;
        this.listener = listener;
        duration = source.durationFrames();
        source.enable(false);
        new Thread(this::feed, "Music PCM feeder").start();
    }

    synchronized void play() {
        if (closed || failed) throw new IllegalStateException("Music stream is unavailable.");
        if (completed) seek(0);
        source.enable(true);
        try {
            output.play();
        } catch (RuntimeException e) {
            source.enable(false);
            throw e;
        }
        playing = true;
        notifyAll();
    }

    synchronized void pause() {
        if (closed) return;
        output.pause();
        playing = false;
        source.enable(false);
    }

    synchronized void stop() {
        pause();
        seek(0);
    }

    synchronized void seek(long frame) {
        if (closed) return;
        output.pause();
        output.flush();
        generation++;
        completed = false;
        baseFrame = Math.max(0, frame);
        submitted = lastHead = headWrap = 0;
        if (playing) output.play();
        notifyAll();
    }

    synchronized void rate(float speed, boolean stretch) {
        if (closed) return;
        output.rate(speed, stretch);
        // Applying PlaybackParams can implicitly start a stopped AudioTrack.
        if (!playing) output.pause();
    }

    synchronized void close() {
        if (closed) return;
        closed = true;
        playing = false;
        try { output.pause(); } catch (RuntimeException ignored) { }
        source.enable(false);
        notifyAll();
        // The feeder owns release/cancellation; never join it under an Activity lock.
    }

    synchronized boolean acceptsEvent(long eventGeneration) { return generation == eventGeneration; }
    synchronized boolean isPlaying() { return playing; }
    synchronized long durationFrames() { return duration; }
    synchronized long positionFrames() {
        if (closed) return baseFrame;
        long head = output.headFrames() & 0xffffffffL;
        if (head < lastHead) headWrap += 1L << 32;
        lastHead = head;
        return baseFrame + Math.min(submitted, headWrap + head);
    }

    private void feed() {
        short[] data = new short[4096 * 2];
        long readGeneration = -1;
        int offset = 0, count = 0;
        boolean end = false;
        long lastUpdate = 0;
        try {
            while (true) {
                long frame, currentGeneration;
                boolean finished = false;
                synchronized (this) {
                    while (!closed && !playing) wait();
                    if (closed) return;
                    currentGeneration = generation;
                    if (readGeneration != generation) {
                        readGeneration = generation;
                        offset = count = 0;
                        end = false;
                    }
                    if (count > 0) {
                        int written = output.write(data, offset, count);
                        if (written < 0 || written > count || written % 2 != 0)
                            throw new IllegalStateException("AudioTrack could not write music PCM: " + written);
                        offset += written;
                        count -= written;
                        submitted += written / 2;
                    }
                    if (end && positionFrames() >= baseFrame + submitted) {
                        playing = false;
                        completed = true;
                        output.pause();
                        source.enable(false);
                        finished = true;
                    }
                    frame = baseFrame + submitted;
                }
                if (finished) {
                    if (!closed) listener.finished(currentGeneration);
                    continue;
                }
                if (System.nanoTime() - lastUpdate > 250_000_000L) {
                    lastUpdate = System.nanoTime();
                    if (!closed) listener.changed(currentGeneration);
                }
                if (count != 0 || end) {
                    synchronized (this) { if (!closed) wait(5); }
                    continue;
                }
                // JNI may wait for up to 100 ms. No transport or Activity lock is held.
                int result = source.read(frame, data);
                long nextDuration = source.durationFrames();
                synchronized (this) {
                    if (closed) return;
                    if (currentGeneration != generation) continue;
                    duration = nextDuration;
                    if (result > 0) {
                        if (result > data.length / 2)
                            throw new IllegalStateException("Invalid music PCM read size.");
                        offset = 0;
                        count = result * 2;
                    } else if (result == END) {
                        end = true;
                        output.end();
                    } else if (result == CANCELLED) {
                        throw new IllegalStateException("Music preparation was cancelled.");
                    } else if (result == ERROR) {
                        throw new IllegalStateException(source.error());
                    } else {
                        wait(5);
                    }
                }
            }
        } catch (Exception e) {
            long errorGeneration;
            synchronized (this) {
                errorGeneration = generation;
                playing = false;
                failed = true;
                if (!closed) {
                    try { output.pause(); } catch (RuntimeException ignored) { }
                    source.enable(false);
                }
            }
            if (!closed) listener.error(errorGeneration, e.getMessage() == null ? "Music stream failed." : e.getMessage());
        } finally {
            synchronized (this) {
                closed = true;
                playing = false;
                try { output.close(); } catch (RuntimeException ignored) { }
            }
            source.close();
        }
    }
}
