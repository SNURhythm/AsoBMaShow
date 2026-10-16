// JNI, AudioTrack and thread-start boundaries are substituted; both production
// stream/player implementations run unchanged inside this headless fixture.
public final class AndroidMusicStreamFixture {
    static String failureAt;
    static Throwable failure;
    static boolean cleanupFails;
    static volatile int sourceReleases, trackReleases;
    static int tracks, starts;
    static final long HANDLE = 42;

    static void failAt(String boundary) {
        if (!boundary.equals(failureAt)) return;
        if (failure instanceof Error) throw (Error) failure;
        throw (RuntimeException) failure;
    }
    static void checkHandle(long handle) { assert handle == HANDLE : "wrong native source"; }
    static final class NativeBoundary {
        static int read(long handle, long frame, short[] samples) { checkHandle(handle); return 0; }
        static long duration(long handle) { checkHandle(handle); failAt("duration"); return 44100; }
        static String error(long handle) { checkHandle(handle); return "render failed"; }
        static void enable(long handle, boolean value) { checkHandle(handle); failAt("enable"); }
        static void seek(long handle, long frame) { checkHandle(handle); }
        static void release(long handle) {
            checkHandle(handle); sourceReleases++;
            if (cleanupFails) throw new IllegalStateException("source cleanup failed");
        }
        static void reportError(String message) { }
    }
    static final class Thread extends java.lang.Thread {
        Thread(Runnable runnable, String name) { super(runnable, name); }
        @Override public void start() { failAt("start"); starts++; super.start(); }
    }
    static final class AudioAttributes {
        static final int USAGE_MEDIA = 1, CONTENT_TYPE_MUSIC = 2;
        static final class Builder {
            Builder setUsage(int value) { return this; }
            Builder setContentType(int value) { return this; }
            AudioAttributes build() { return new AudioAttributes(); }
        }
    }
    static final class AudioFormat {
        static final int CHANNEL_OUT_STEREO = 1, ENCODING_PCM_16BIT = 2;
        static final class Builder {
            Builder setSampleRate(int value) { return this; }
            Builder setChannelMask(int value) { return this; }
            Builder setEncoding(int value) { return this; }
            AudioFormat build() { return new AudioFormat(); }
        }
    }
    static final class PlaybackParams {
        PlaybackParams allowDefaults() { return this; }
        PlaybackParams setSpeed(float value) { return this; }
        PlaybackParams setPitch(float value) { return this; }
    }
    static final class AudioTrack {
        static final int MODE_STREAM = 1, WRITE_NON_BLOCKING = 1, STATE_INITIALIZED = 1;
        static int getMinBufferSize(int rate, int channel, int encoding) {
            failAt("minimum"); return failureAt.equals("unavailable") ? -1 : 4096;
        }
        static final class Builder {
            Builder setAudioAttributes(AudioAttributes value) { return this; }
            Builder setAudioFormat(AudioFormat value) { return this; }
            Builder setTransferMode(int value) { return this; }
            Builder setBufferSizeInBytes(int value) { return this; }
            AudioTrack build() { failAt("build"); tracks++; return new AudioTrack(); }
        }
        int getState() { failAt("state"); return failureAt.equals("uninitialized") ? 0 : STATE_INITIALIZED; }
        void release() {
            trackReleases++;
            if (cleanupFails) throw new IllegalStateException("track cleanup failed");
        }
        int write(short[] samples, int offset, int count, int mode) { return count; }
        int getPlaybackHeadPosition() { return 0; }
        void play() { }
        void pause() { }
        void flush() { }
        void setPlaybackParams(PlaybackParams value) { }
    }

    // STREAM_IMPLEMENTATION
    // PLAYER_IMPLEMENTATION

    static final NativePcmMusicPlayer.Listener LISTENER = new NativePcmMusicPlayer.Listener() {
        public void changed(long generation) { }
        public void finished(long generation) { }
        public void error(long generation, String message) { throw new AssertionError(message); }
    };

    public static void main(String[] args) throws Exception {
        failureAt = args[0];
        cleanupFails = args.length > 1 && args[1].equals("cleanup");
        failure = args.length > 2 && args[2].equals("error")
                ? new OutOfMemoryError("construction failed") : new IllegalStateException("construction failed");
        Throwable caught = null;
        NativePcmMusicPlayer player = null;
        try { player = AndroidMusicStream.create(HANDLE, LISTENER); }
        catch (RuntimeException | Error problem) { caught = problem; }
        if (failureAt.equals("success")) {
            assert caught == null : caught;
            assert sourceReleases == 0 && trackReleases == 0 : "factory released transferred ownership";
            player.close();
            long deadline = System.nanoTime() + 2_000_000_000L;
            while ((sourceReleases == 0 || trackReleases == 0) && System.nanoTime() < deadline)
                java.lang.Thread.sleep(2);
            player.close();
            assert starts == 1;
        } else {
            assert caught != null : "construction unexpectedly succeeded";
            if (failureAt.equals("unavailable") || failureAt.equals("uninitialized")) {
                assert caught instanceof IllegalStateException;
                assert !caught.getMessage().contains("cleanup") : "cleanup masked initialization failure";
            } else assert caught == failure : "construction failure was replaced: " + caught;
            assert starts == 0 : "failed construction left a running feeder";
            if (cleanupFails) {
                int suppressed = caught.getSuppressed().length;
                assert suppressed == (tracks == 0 ? 1 : 2) : "lost cleanup failure: " + suppressed;
            }
        }
        assert sourceReleases == 1 : "source releases: " + sourceReleases;
        assert trackReleases == tracks : "track releases: " + trackReleases + ", allocated: " + tracks;
    }
}
