package com.snurhythm.asobmashow;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.media.session.MediaController;
import android.media.session.MediaSession;
import android.os.Build;
import android.os.IBinder;

public class AsoBMaShowMusicService extends Service {
    public static final String ACTION_UPDATE = "com.snurhythm.asobmashow.music.UPDATE";
    public static final String ACTION_STOP_SERVICE = "com.snurhythm.asobmashow.music.STOP_SERVICE";
    public static final String ACTION_PLAY = "com.snurhythm.asobmashow.music.PLAY";
    public static final String ACTION_PAUSE = "com.snurhythm.asobmashow.music.PAUSE";
    public static final String ACTION_STOP = "com.snurhythm.asobmashow.music.STOP";
    public static final String ACTION_PREVIOUS = "com.snurhythm.asobmashow.music.PREVIOUS";
    public static final String ACTION_NEXT = "com.snurhythm.asobmashow.music.NEXT";
    public static final String EXTRA_TITLE = "title";
    public static final String EXTRA_ARTIST = "artist";
    public static final String EXTRA_ALBUM = "album";
    public static final String EXTRA_ARTWORK_PATH = "artwork_path";
    public static final String EXTRA_PLAYING = "playing";
    public static final String EXTRA_SESSION_TOKEN = "session_token";

    private static final String CHANNEL_ID = "asobmashow_music";
    static final int NOTIFICATION_ID = 0x41534d53;

    private String title = "AsoBMaShow";
    private String artist = "AsoBMaShow";
    private String album = "";
    private String artworkPath = "";
    private boolean playing = false;
    private boolean foreground = false;
    private MediaSession.Token sessionToken;

    @Override
    public void onCreate() {
        super.onCreate();
        createNotificationChannel();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) {
            if (playing) {
                updateNotification();
                return START_STICKY;
            }
            stopSelf(startId);
            return START_NOT_STICKY;
        }

        String action = intent.getAction();
        readUpdate(intent);
        if (ACTION_STOP_SERVICE.equals(action)) {
            removeNotification();
            stopSelf();
            return START_NOT_STICKY;
        }

        if (ACTION_PLAY.equals(action)) {
            sendTransportAction(intent, ACTION_PLAY);
            playing = sessionToken != null;
        } else if (ACTION_PAUSE.equals(action)) {
            sendTransportAction(intent, ACTION_PAUSE);
            playing = false;
        } else if (ACTION_STOP.equals(action)) {
            sendTransportAction(intent, ACTION_STOP);
            removeNotification();
            stopSelf();
            return START_NOT_STICKY;
        } else if (ACTION_PREVIOUS.equals(action)) {
            AsoBMaShowActivity.nativeMusicControlEvent("previous");
        } else if (ACTION_NEXT.equals(action)) {
            AsoBMaShowActivity.nativeMusicControlEvent("next");
        }

        updateNotification();
        if (!playing) {
            stopSelf(startId);
            return START_NOT_STICKY;
        }
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void readUpdate(Intent intent) {
        if (intent.hasExtra(EXTRA_TITLE)) {
            title = nonEmpty(intent.getStringExtra(EXTRA_TITLE), "AsoBMaShow");
        }
        if (intent.hasExtra(EXTRA_ARTIST)) {
            artist = nonEmpty(intent.getStringExtra(EXTRA_ARTIST), "AsoBMaShow");
        }
        if (intent.hasExtra(EXTRA_ALBUM)) {
            album = nonEmpty(intent.getStringExtra(EXTRA_ALBUM), "");
        }
        if (intent.hasExtra(EXTRA_ARTWORK_PATH)) {
            artworkPath = nonEmpty(intent.getStringExtra(EXTRA_ARTWORK_PATH), "");
        }
        if (intent.hasExtra(EXTRA_PLAYING)) {
            playing = intent.getBooleanExtra(EXTRA_PLAYING, false);
        }
        if (intent.hasExtra(EXTRA_SESSION_TOKEN)) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                sessionToken = intent.getParcelableExtra(EXTRA_SESSION_TOKEN,
                        MediaSession.Token.class);
            } else {
                @SuppressWarnings("deprecation")
                MediaSession.Token token = intent.getParcelableExtra(EXTRA_SESSION_TOKEN);
                sessionToken = token;
            }
        }
    }

    private String nonEmpty(String value, String fallback) {
        if (value == null || value.trim().isEmpty()) {
            return fallback;
        }
        return value;
    }

    private void sendTransportAction(Intent intent, String action) {
        MediaSession.Token token = sessionToken;
        if (token == null) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                token = intent.getParcelableExtra(EXTRA_SESSION_TOKEN,
                        MediaSession.Token.class);
            } else {
                @SuppressWarnings("deprecation")
                MediaSession.Token legacyToken =
                        intent.getParcelableExtra(EXTRA_SESSION_TOKEN);
                token = legacyToken;
            }
        }
        if (token == null) {
            return;
        }
        MediaController.TransportControls controls =
                new MediaController(this, token).getTransportControls();
        if (ACTION_PLAY.equals(action)) {
            controls.play();
        } else if (ACTION_PAUSE.equals(action)) {
            controls.pause();
        } else if (ACTION_STOP.equals(action)) {
            controls.stop();
        }
    }

    private void updateNotification() {
        Notification notification = buildNotification();
        if (playing) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                startForeground(NOTIFICATION_ID, notification,
                        ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
            } else {
                startForeground(NOTIFICATION_ID, notification);
            }
            foreground = true;
        } else {
            if (foreground) {
                stopForeground(Service.STOP_FOREGROUND_DETACH);
                foreground = false;
            }
            NotificationManager manager =
                    (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
            if (manager != null &&
                    (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU ||
                            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                                    == PackageManager.PERMISSION_GRANTED)) {
                manager.notify(NOTIFICATION_ID, notification);
            }
        }
    }

    private void removeNotification() {
        if (foreground) {
            stopForeground(Service.STOP_FOREGROUND_REMOVE);
            foreground = false;
        } else {
            stopForeground(Service.STOP_FOREGROUND_REMOVE);
            NotificationManager manager =
                    (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
            if (manager != null) {
                manager.cancel(NOTIFICATION_ID);
            }
        }
    }

    private Notification buildNotification() {
        Intent launchIntent = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (launchIntent == null) {
            launchIntent = new Intent(this, AsoBMaShowActivity.class);
        }
        PendingIntent contentIntent = PendingIntent.getActivity(this, 0, launchIntent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        Notification.Action previousAction = new Notification.Action.Builder(
                android.R.drawable.ic_media_previous, "Previous",
                serviceIntent(ACTION_PREVIOUS, 1)).build();
        Notification.Action playPauseAction = new Notification.Action.Builder(
                playing ? android.R.drawable.ic_media_pause : android.R.drawable.ic_media_play,
                playing ? "Pause" : "Play",
                serviceIntent(playing ? ACTION_PAUSE : ACTION_PLAY, 2)).build();
        Notification.Action nextAction = new Notification.Action.Builder(
                android.R.drawable.ic_media_next, "Next",
                serviceIntent(ACTION_NEXT, 3)).build();
        Notification.Action stopAction = new Notification.Action.Builder(
                android.R.drawable.ic_menu_close_clear_cancel, "Stop",
                serviceIntent(ACTION_STOP, 4)).build();

        Notification.Builder builder = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? new Notification.Builder(this, CHANNEL_ID)
                : new Notification.Builder(this);
        builder.setSmallIcon(R.drawable.ic_music_note)
                .setContentTitle(title)
                .setContentText(artist)
                .setSubText(album)
                .setContentIntent(contentIntent)
                .setShowWhen(false)
                .setOngoing(playing)
                .setOnlyAlertOnce(true)
                .addAction(previousAction)
                .addAction(playPauseAction)
                .addAction(nextAction)
                .addAction(stopAction);

        Bitmap artwork = decodeArtwork();
        if (artwork != null) {
            builder.setLargeIcon(artwork);
        }

        Notification.MediaStyle style = new Notification.MediaStyle()
                .setShowActionsInCompactView(0, 1, 2);
        if (sessionToken != null) {
            style.setMediaSession(sessionToken);
        }
        builder.setStyle(style);
        return builder.build();
    }

    private PendingIntent serviceIntent(String action, int requestCode) {
        Intent intent = new Intent(this, AsoBMaShowMusicService.class)
                .setAction(action)
                .putExtra(EXTRA_TITLE, title)
                .putExtra(EXTRA_ARTIST, artist)
                .putExtra(EXTRA_ALBUM, album)
                .putExtra(EXTRA_ARTWORK_PATH, artworkPath)
                .putExtra(EXTRA_PLAYING, playing);
        if (sessionToken != null) {
            intent.putExtra(EXTRA_SESSION_TOKEN, sessionToken);
        }
        return PendingIntent.getService(this, requestCode, intent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
    }

    private Bitmap decodeArtwork() {
        if (artworkPath == null || artworkPath.trim().isEmpty()) {
            return null;
        }
        try {
            return BitmapFactory.decodeFile(artworkPath);
        } catch (Exception ignored) {
            return null;
        }
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return;
        }
        NotificationChannel channel = new NotificationChannel(CHANNEL_ID,
                "Music playback", NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("AsoBMaShow music playback controls");
        NotificationManager manager =
                (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        if (manager != null) {
            manager.createNotificationChannel(channel);
        }
    }
}
