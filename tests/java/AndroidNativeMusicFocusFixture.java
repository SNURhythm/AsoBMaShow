import java.util.ArrayList;

// Only platform/player boundaries are substituted. Focus listener routing,
// transport decisions, stale-event checks and release run from the activity.
public class AndroidNativeMusicFocusFixture {
    static class Context { static final String AUDIO_SERVICE = "audio"; }
    static class Looper { static Looper getMainLooper() { return new Looper(); } }
    static class Handler { Handler(Looper looper) {} }
    interface FocusListener { void changed(int change); }
    static class AudioAttributes {
        static final int USAGE_MEDIA = 1, CONTENT_TYPE_MUSIC = 2;
        static class Builder {
            Builder setUsage(int value) { return this; }
            Builder setContentType(int value) { return this; }
            AudioAttributes build() { return new AudioAttributes(); }
        }
    }
    static class AudioFocusRequest {
        FocusListener listener;
        static class Builder {
            AudioFocusRequest request = new AudioFocusRequest();
            Builder(int gain) {}
            Builder setAudioAttributes(AudioAttributes value) { return this; }
            Builder setWillPauseWhenDucked(boolean value) { return this; }
            Builder setOnAudioFocusChangeListener(FocusListener listener, Handler handler) {
                request.listener = listener; return this;
            }
            AudioFocusRequest build() { return request; }
        }
    }
    static class AudioManager {
        static final int AUDIOFOCUS_GAIN = 1, AUDIOFOCUS_LOSS = -1,
            AUDIOFOCUS_LOSS_TRANSIENT = -2, AUDIOFOCUS_REQUEST_GRANTED = 1;
        int requests, abandoned;
        AudioFocusRequest request;
        int requestAudioFocus(AudioFocusRequest value) { ++requests; request = value; return 1; }
        void abandonAudioFocusRequest(AudioFocusRequest value) {
            assert request == value; ++abandoned; request = null;
        }
    }
    static class NativePcmMusicPlayer {
        interface Listener {
            void changed(long generation);
            void finished(long generation);
            void error(long generation, String message);
        }
        Listener listener;
        boolean playing, closed, failStart;
        void play() { if (failStart) throw new IllegalStateException("start failed"); playing = true; }
        void start() { play(); }
        void pause() { playing = false; }
        void close() { closed = true; playing = false; }
        void release() { close(); }
        void rate(float rate, boolean stretch) {}
        boolean isPlaying() { return playing; }
        boolean acceptsEvent(long generation) { return generation == 7; }
        long durationFrames() { return 44100; }
        long positionFrames() { return 0; }
    }
    static class AndroidMusicStream {
        static final int SAMPLE_RATE = 44100;
        static int errors;
        static NativePcmMusicPlayer create(long handle, NativePcmMusicPlayer.Listener listener) {
            NativePcmMusicPlayer player = new NativePcmMusicPlayer(); player.listener = listener; return player;
        }
        static void reportError(String error) { assert error.equals("output failed"); ++errors; }
    }
    static class Session {
        boolean active, released;
        void setActive(boolean value) { active = value; }
        void release() { released = true; }
    }
    static final String ERROR_PREFIX = "ERROR:";
    static final long NATIVE_MUSIC_UNKNOWN_QUEUE_ID = -1;
    final Object nativeMusicLock = new Object();
    final AudioManager manager = new AudioManager();
    NativePcmMusicPlayer nativeMusicStream, nativeMusicPlayer;
    AudioFocusRequest nativeMusicFocusRequest;
    Session nativeMusicSession;
    boolean nativeMusicHasFocus, nativeMusicResumeAfterFocus, nativeMusicHandoff;
    long nativeMusicFocusGeneration, nativeMusicDurationMicros;
    float nativeMusicPlaybackRate = 1;
    boolean nativeMusicTimeStretch;
    String nativeMusicTitle, nativeMusicArtist, nativeMusicAlbum, nativeMusicArtworkPath, nativeMusicQueueTitle;
    Object nativeMusicArtwork;
    ArrayList<String> nativeMusicQueue = new ArrayList<>();
    long nativeMusicActiveQueueItemId = -1;
    boolean serviceActive, failMetadata;
    int finished;
    Object getSystemService(String name) { return manager; }
    void runOnUiThread(Runnable runnable) { runnable.run(); }
    String messageForException(Exception error, String fallback) { return error.getMessage(); }
    String metadataLine(String[] lines, int index, String fallback) { return index < lines.length ? lines[index] : fallback; }
    Object decodeNativeMusicArtwork(String path) { return null; }
    void ensureNativeMusicSessionLocked() { if (nativeMusicSession == null) nativeMusicSession = new Session(); }
    void updateNativeMusicPlaybackStateLocked(boolean playing, long position) {}
    void updateNativeMusicSessionLocked(boolean playing) { updateNativeMusicSessionLocked(playing, true); }
    void updateNativeMusicSessionLocked(boolean playing, boolean updateService) {
        if (failMetadata) { failMetadata = false; throw new IllegalStateException("metadata failed"); }
        ensureNativeMusicSessionLocked();
        nativeMusicSession.active = hasNativeMusicLocked();
        if (updateService) serviceActive = hasNativeMusicLocked();
    }
    void stopNativeMusicForegroundServiceLocked() { serviceActive = false; }
    void nativeMusicControlEvent(String event) { assert event.equals("finished"); ++finished; }

    // ACTIVITY_METHODS

    static AndroidNativeMusicFocusFixture loaded(boolean stream) {
        AndroidNativeMusicFocusFixture activity = new AndroidNativeMusicFocusFixture();
        if (stream) assert activity.loadNativeMusicStream("1", "fixture", 1000000).equals("OK");
        else activity.nativeMusicPlayer = new NativePcmMusicPlayer();
        return activity;
    }
    public static void main(String[] args) {
        for (boolean stream : new boolean[]{true, false}) {
            AndroidNativeMusicFocusFixture a = loaded(stream);
            NativePcmMusicPlayer player = stream ? a.nativeMusicStream : a.nativeMusicPlayer;
            switch (args[0]) {
            case "start-failure":
                player.failStart = true;
                assert a.playNativeMusic().startsWith(ERROR_PREFIX);
                assert !player.playing && !a.nativeMusicHasFocus && a.manager.request == null;
                assert a.manager.abandoned == 1;
                player.failStart = false;
                assert a.playNativeMusic().equals("OK") && a.manager.requests == 2;
                break;
            case "pause":
                assert a.playNativeMusic().equals("OK");
                AudioFocusRequest old = a.manager.request;
                assert a.pauseNativeMusic().equals("OK");
                assert !player.playing && !a.nativeMusicHasFocus && a.manager.request == null;
                assert a.nativeMusicFocusRequest == null;
                old.listener.changed(AudioManager.AUDIOFOCUS_GAIN);
                assert !player.playing && !a.nativeMusicHasFocus;
                assert a.playNativeMusic().equals("OK") && a.manager.requests == 2;
                break;
            case "interruption":
                assert a.playNativeMusic().equals("OK");
                AudioFocusRequest focus = a.manager.request;
                focus.listener.changed(AudioManager.AUDIOFOCUS_LOSS_TRANSIENT);
                assert !player.playing && a.nativeMusicResumeAfterFocus;
                assert a.manager.request == focus && a.manager.abandoned == 0;
                focus.listener.changed(AudioManager.AUDIOFOCUS_LOSS_TRANSIENT);
                focus.listener.changed(AudioManager.AUDIOFOCUS_GAIN);
                assert player.playing && a.manager.requests == 1;
                focus.listener.changed(AudioManager.AUDIOFOCUS_LOSS);
                assert !player.playing && !a.nativeMusicResumeAfterFocus;
                focus.listener.changed(AudioManager.AUDIOFOCUS_GAIN);
                assert !player.playing;
                break;
            case "metadata-failure":
                a.failMetadata = true;
                assert a.playNativeMusic().startsWith(ERROR_PREFIX);
                assert player.playing && a.nativeMusicHasFocus && a.manager.abandoned == 0;
                break;
            case "stream-failure":
                if (!stream) break;
                assert a.playNativeMusic().equals("OK");
                Session session = a.nativeMusicSession;
                player.listener.error(6, "output failed");
                assert a.nativeMusicStream == player && AndroidMusicStream.errors == 0;
                player.playing = false; // the feeder stopped output before posting the error
                player.listener.error(7, "output failed");
                assert player.closed && !a.hasNativeMusicLocked();
                assert a.nativeMusicSession == null && session.released && !session.active;
                assert !a.serviceActive && !a.nativeMusicHasFocus && a.manager.request == null;
                assert AndroidMusicStream.errors == 1;
                assert a.loadNativeMusicStream("2", "next", 1000000).equals("OK");
                player.listener.error(7, "output failed");
                assert a.hasNativeMusicLocked() && AndroidMusicStream.errors == 1;
                a.nativeMusicStream.listener.finished(7);
                assert a.hasNativeMusicLocked() && a.nativeMusicHandoff && a.finished == 1;
                break;
            default: throw new AssertionError(args[0]);
            }
        }
    }
}
