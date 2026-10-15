import java.util.ArrayList;
import java.util.ArrayDeque;
import java.util.HashMap;

// Platform/player boundaries and the native queue worker are substituted.
// Activity transport/focus methods and service dispatch/promotion run from source.
// The queue worker follows MusicPlayerService.cpp cleanup and preparation calls.
public class AndroidNativeMusicFocusFixture {
    static class Context { static final String AUDIO_SERVICE = "audio", NOTIFICATION_SERVICE = "notification"; }
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
    class AudioManager {
        static final int AUDIOFOCUS_GAIN = 1, AUDIOFOCUS_LOSS = -1,
            AUDIOFOCUS_LOSS_TRANSIENT = -2, AUDIOFOCUS_REQUEST_GRANTED = 1;
        int requests, abandoned;
        AudioFocusRequest request;
        int requestAudioFocus(AudioFocusRequest value) {
            ++requests;
            if (denyFocus || (background && !service.platformForeground)) return 0;
            request = value; return 1;
        }
        void abandonAudioFocusRequest(AudioFocusRequest value) {
            assert request == null || request == value; ++abandoned; request = null;
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
        void stop() { playing = false; }
        void seekTo(int position, int mode) {}
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
    static class Bundle extends HashMap<String, Object> {
        long getLong(String key) { return ((Number) get(key)).longValue(); }
    }
    static class Build {
        static class VERSION { static int SDK_INT = 35; }
        static class VERSION_CODES { static final int O = 26, Q = 29, TIRAMISU = 33; }
    }
    static class Manifest { static class permission { static final String POST_NOTIFICATIONS = "notifications"; } }
    static class PackageManager { static final int PERMISSION_GRANTED = 0; }
    static class ServiceInfo { static final int FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK = 2; }
    static class MediaPlayer { static final int SEEK_CLOSEST = 3; }
    static class Notification {}
    static class NotificationManager { void notify(int id, Notification notification) {} }
    static class Intent {
        String action;
        Bundle extras = new Bundle();
        Bundle getExtras() { return extras; }
        Intent(Object context, Class<?> type) {}
        Intent setAction(String value) { action = value; return this; }
        String getAction() { return action; }
        Intent putExtra(String key, Object value) { extras.put(key, value); return this; }
        boolean hasExtra(String key) { return extras.containsKey(key); }
        long getLongExtra(String key, long fallback) { return ((Number) extras.getOrDefault(key, fallback)).longValue(); }
        String getStringExtra(String key) { return (String) extras.get(key); }
        boolean getBooleanExtra(String key, boolean fallback) { return (boolean) extras.getOrDefault(key, fallback); }
        <T> T getParcelableExtra(String key, Class<T> type) { return type.cast(extras.get(key)); }
        @SuppressWarnings("unchecked") <T> T getParcelableExtra(String key) { return (T) extras.get(key); }
    }
    static class PendingIntent {
        static final int FLAG_UPDATE_CURRENT = 1, FLAG_IMMUTABLE = 2;
        final Intent intent;
        PendingIntent(Intent value) { intent = value; }
        static PendingIntent getService(Object context, int code, Intent intent, int flags) {
            return new PendingIntent(intent);
        }
    }
    static class MediaSession {
        static final int FLAG_HANDLES_MEDIA_BUTTONS = 1, FLAG_HANDLES_TRANSPORT_CONTROLS = 2;
        static class Token { MediaSession session; Token(MediaSession value) { session = value; } }
        static class Callback {
            public void onPlay() {} public void onPause() {} public void onStop() {}
            public void onSeekTo(long position) {} public void onSkipToPrevious() {} public void onSkipToNext() {}
            public void onCustomAction(String action, Bundle extras) {}
        }
        Callback callback;
        boolean active, released;
        MediaSession(Object context, String tag) {}
        void setFlags(int flags) {}
        void setCallback(Callback value, Handler handler) { callback = value; }
        void setActive(boolean value) { active = value; }
        void release() { released = true; }
        Token getSessionToken() { return new Token(this); }
    }
    static class MediaController {
        MediaSession session;
        MediaController(Object context, MediaSession.Token token) { session = token.session; }
        TransportControls getTransportControls() { return new TransportControls(); }
        class TransportControls {
            void play() { events.add(() -> session.callback.onPlay()); }
            void pause() { events.add(() -> session.callback.onPause()); }
            void stop() { events.add(() -> session.callback.onStop()); }
            void sendCustomAction(String action, Bundle extras) { events.add(() -> session.callback.onCustomAction(action, extras)); }
        }
    }
    static class Service {
        static final int START_STICKY = 1, START_NOT_STICKY = 2;
        static final int STOP_FOREGROUND_DETACH = 1;
        void stopSelf(int id) {} void stopSelf() {}
        boolean platformForeground;
        void startForeground(int id, Notification notification) { platformForeground = true; }
        void startForeground(int id, Notification notification, int type) { platformForeground = true; }
        void stopForeground(int flags) { platformForeground = false; }
        Object getSystemService(String name) { return new NotificationManager(); }
        int checkSelfPermission(String name) { return PackageManager.PERMISSION_GRANTED; }
    }
    static class AsoBMaShowMusicService extends Service {
        static final int NOTIFICATION_ID = 1;
        String title = "", artist = "", album = "", artworkPath = "";
        boolean playing, foreground;
        long transportGeneration;
        MediaSession.Token sessionToken;
        Notification buildNotification() { return new Notification(); }
        void removeNotification() { foreground = false; platformForeground = false; }
        // SERVICE_METHODS
    }
    static class AsoBMaShowActivity {
        static void nativeMusicControlEvent(String event) { current.nativeMusicControlEvent(event); }
    }
    static AndroidNativeMusicFocusFixture current;
    static final ArrayDeque<Runnable> events = new ArrayDeque<>();
    final AsoBMaShowMusicService service = new AsoBMaShowMusicService();
    boolean background, denyFocus;
    String skipOutcome = "play", lastSkip;
    int skips;
    void startService(Intent intent) { events.add(() -> service.onStartCommand(intent, 0, 1)); }
    void startForegroundService(Intent intent) { startService(intent); }
    void requestMusicNotificationPermissionIfNeeded() {}
    String seekNativeMusic(String value) { return "OK"; }
    static void drain() {
        for (int i = 0; !events.isEmpty(); ++i) {
            assert i < 40 : "recursive media-session/service dispatch";
            events.remove().run();
        }
    }
    static final String ERROR_PREFIX = "ERROR:";
    static final long NATIVE_MUSIC_UNKNOWN_QUEUE_ID = -1;
    final Object nativeMusicLock = new Object();
    final AudioManager manager = new AudioManager();
    NativePcmMusicPlayer nativeMusicStream, nativeMusicPlayer;
    AudioFocusRequest nativeMusicFocusRequest;
    MediaSession nativeMusicSession;
    boolean nativeMusicHasFocus, nativeMusicResumeAfterFocus, nativeMusicHandoff, nativeMusicTransportPending;
    long nativeMusicFocusGeneration, nativeMusicDurationMicros, nativeMusicTransportGeneration;
    float nativeMusicPlaybackRate = 1;
    boolean nativeMusicTimeStretch;
    String nativeMusicTitle = "AsoBMaShow", nativeMusicArtist = "AsoBMaShow", nativeMusicAlbum = "",
            nativeMusicArtworkPath = "", nativeMusicQueueTitle = "";
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
    void updateNativeMusicPlaybackStateLocked(boolean playing, long position) {}
    void updateNativeMusicSessionLocked(boolean playing) { updateNativeMusicSessionLocked(playing, true); }
    void updateNativeMusicSessionLocked(boolean playing, boolean updateService) {
        if (failMetadata) { failMetadata = false; throw new IllegalStateException("metadata failed"); }
        ensureNativeMusicSessionLocked();
        nativeMusicSession.active = hasNativeMusicLocked();
        if (updateService) { serviceActive = hasNativeMusicLocked(); updateNativeMusicForegroundServiceLocked(playing); }
    }
    void stopNativeMusicForegroundServiceLocked() { serviceActive = false; service.foreground = false; service.platformForeground = false; service.playing = false; }
    void nativeMusicControlEvent(String event) {
        if (event.equals("finished")) { ++finished; return; }
        ++skips;
        lastSkip = event;
        // Model the native event pump/worker contract: boundary failure pauses,
        // preparation failure stops, and success loads/rates then plays later.
        events.add(() -> {
            if (skipOutcome.equals("boundary")) pauseNativeMusic();
            else if (skipOutcome.equals("failure")) stopNativeMusic();
            else {
                assert service.platformForeground : "skip reached native preparation before foreground";
                assert loadNativeMusicStream("2", "next", 1000000).equals("OK");
                updateNativeMusicSessionLocked(false); // rate/metadata publication during preparation
                events.add(() -> playNativeMusic());
            }
        });
    }

    // ACTIVITY_METHODS

    static AndroidNativeMusicFocusFixture loaded(boolean stream) {
        AndroidNativeMusicFocusFixture activity = new AndroidNativeMusicFocusFixture();
        current = activity; events.clear();
        if (stream) assert activity.loadNativeMusicStream("1", "fixture", 1000000).equals("OK");
        else activity.nativeMusicPlayer = new NativePcmMusicPlayer();
        return activity;
    }
    static void testNotificationCancellation() {
        for (boolean stream : new boolean[]{true, false}) {
            for (String action : new String[]{AsoBMaShowMusicService.ACTION_PLAY,
                    AsoBMaShowMusicService.ACTION_PREVIOUS, AsoBMaShowMusicService.ACTION_NEXT}) {
                for (String newer : new String[]{"pause", "stop", "load"}) {
                    for (boolean serviceAlreadyDispatched : new boolean[]{true, false}) {
                        AndroidNativeMusicFocusFixture a = loaded(stream);
                        assert a.playNativeMusic().equals("OK"); drain();
                        assert a.pauseNativeMusic().equals("OK"); drain();
                        a.background = true;
                        Intent notification = a.service.serviceIntent(action, 1).intent;
                        if (serviceAlreadyDispatched) a.service.onStartCommand(notification, 0, 2);
                        if (newer.equals("pause")) a.pauseNativeMusic();
                        else if (newer.equals("stop")) a.stopNativeMusic();
                        else assert a.loadNativeMusicStream("3", "replacement", 1000000).equals("OK");
                        if (!serviceAlreadyDispatched) a.service.onStartCommand(notification, 0, 2);
                        drain();
                        assert !a.isNativeMusicPlayingLocked() && a.skips == 0
                                : "stale notification " + action + " overrode " + newer;
                        assert !a.nativeMusicHasFocus && !a.service.foreground
                                : "stale notification retained foreground eligibility";
                    }
                }
            }
        }
    }
    public static void main(String[] args) {
        if (args[0].equals("notification-cancel")) { testNotificationCancellation(); return; }
        for (boolean stream : new boolean[]{true, false}) {
            AndroidNativeMusicFocusFixture a = loaded(stream);
            NativePcmMusicPlayer player = stream ? a.nativeMusicStream : a.nativeMusicPlayer;
            if (args[0].startsWith("remote-") || args[0].equals("notification-play")) {
                assert a.playNativeMusic().equals("OK"); drain();
                a.background = true;
                a.nativeMusicSession.callback.onPause(); drain();
                assert !a.service.foreground && !player.playing && !a.nativeMusicHasFocus;
            }
            switch (args[0]) {
            case "remote-latest":
                a.nativeMusicSession.callback.onPlay();
                a.nativeMusicSession.callback.onSkipToNext(); drain();
                assert a.isNativeMusicPlayingLocked() && a.service.foreground;
                assert a.skips == 1 && a.lastSkip.equals("next");
                a.pauseNativeMusic(); drain();
                a.nativeMusicSession.callback.onSkipToPrevious();
                a.pauseNativeMusic(); drain();
                assert !a.isNativeMusicPlayingLocked() && !a.service.foreground && a.skips == 1;
                break;
            case "remote-cancel":
                a.nativeMusicSession.callback.onPlay();
                a.pauseNativeMusic(); drain();
                assert !a.isNativeMusicPlayingLocked() && !a.service.foreground;
                a.nativeMusicSession.callback.onPlay();
                events.remove().run(); // service promotes, then callback is still queued
                a.stopNativeMusic(); drain();
                assert !a.isNativeMusicPlayingLocked() && !a.service.foreground;
                a.nativeMusicSession.callback.onPlay();
                assert a.loadNativeMusicStream("3", "replacement", 1000000).equals("OK");
                drain();
                assert !a.isNativeMusicPlayingLocked() && !a.service.foreground;
                break;
            case "remote-play":
                a.nativeMusicSession.callback.onPlay(); drain();
                assert player.playing : "background remote Play did not resume";
                assert a.nativeMusicHasFocus && a.service.foreground;
                assert a.manager.requests == 2;
                assert a.service.title.equals(a.nativeMusicTitle);
                break;
            case "notification-play":
                a.service.onStartCommand(a.service.serviceIntent(
                        AsoBMaShowMusicService.ACTION_PLAY, 2).intent, 0, 2);
                drain();
                assert player.playing && a.service.foreground && a.manager.requests == 2;
                break;
            case "remote-skip":
                a.nativeMusicSession.callback.onSkipToPrevious(); drain();
                assert a.isNativeMusicPlayingLocked() && a.service.foreground;
                assert a.skips == 1 && a.lastSkip.equals("previous");
                a.pauseNativeMusic(); drain();
                a.nativeMusicSession.callback.onSkipToNext(); drain();
                assert a.isNativeMusicPlayingLocked() && a.service.foreground;
                assert a.skips == 2 && a.lastSkip.equals("next");
                break;
            case "remote-failure":
                a.denyFocus = true;
                a.nativeMusicSession.callback.onPlay(); drain();
                assert !player.playing && !a.nativeMusicHasFocus && !a.service.foreground;
                a.denyFocus = false;
                player.failStart = true;
                a.nativeMusicSession.callback.onPlay(); drain();
                assert !player.playing && !a.nativeMusicHasFocus && !a.service.foreground;
                player.failStart = false;
                for (String outcome : new String[]{"boundary", "failure"}) {
                    a.skipOutcome = outcome;
                    a.nativeMusicSession.callback.onSkipToPrevious(); drain();
                    assert !a.service.foreground && !a.nativeMusicHasFocus && !a.nativeMusicHandoff;
                    a.nativeMusicSession.callback.onSkipToNext(); drain();
                    assert !a.service.foreground && !a.nativeMusicHasFocus && !a.nativeMusicHandoff;
                }
                assert a.skips == 4;
                break;
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
                MediaSession session = a.nativeMusicSession;
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
