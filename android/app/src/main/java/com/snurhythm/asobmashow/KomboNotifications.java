package com.snurhythm.asobmashow;

import android.Manifest;
import android.app.AlarmManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Build;
import android.provider.Settings;
import org.json.JSONArray;
import org.json.JSONObject;
import java.util.TimeZone;
import java.util.UUID;

/** A bounded, private plan. No exact-alarm permission or background service. */
public class KomboNotifications extends BroadcastReceiver {
    private static final String STORE = "kombo_notifications";
    private static final String CHANNEL = "kombo_reminders";
    private static final String FIRE = "com.snurhythm.asobmashow.KOMBO_REMINDER";
    private static final String TAP = "com.snurhythm.asobmashow.KOMBO_TAP";
    private static final int BASE = 0x434f00;
    private static boolean foreground;
    private static String tapProfile = "";

    private static SharedPreferences prefs(Context context) {
        return context.getSharedPreferences(STORE, Context.MODE_PRIVATE);
    }
    private static NotificationManager manager(Context context) {
        return (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
    }
    private static void channel(Context context) {
        if (Build.VERSION.SDK_INT >= 26) {
            manager(context).createNotificationChannel(new NotificationChannel(
                    CHANNEL, "Kombo reminders", NotificationManager.IMPORTANCE_DEFAULT));
        }
    }
    static String permission(Context context) {
        channel(context);
        if (Build.VERSION.SDK_INT >= 33 && context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED)
            return prefs(context).getBoolean("asked", false) ? "denied" : "ask";
        if (Build.VERSION.SDK_INT >= 24 && !manager(context).areNotificationsEnabled()) return "denied";
        if (Build.VERSION.SDK_INT >= 26 && manager(context).getNotificationChannel(CHANNEL).getImportance()
                == NotificationManager.IMPORTANCE_NONE) return "denied";
        return "granted";
    }
    static void request(AsoBMaShowActivity activity) {
        if (Build.VERSION.SDK_INT >= 33 && permission(activity).equals("ask")) {
            prefs(activity).edit().putBoolean("asked", true).apply();
            activity.requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, BASE);
        }
    }
    static void settings(AsoBMaShowActivity activity) {
        Intent intent = new Intent(Build.VERSION.SDK_INT >= 26
                ? Settings.ACTION_CHANNEL_NOTIFICATION_SETTINGS : Settings.ACTION_APPLICATION_DETAILS_SETTINGS);
        if (Build.VERSION.SDK_INT >= 26) {
            intent.putExtra(Settings.EXTRA_APP_PACKAGE, activity.getPackageName());
            intent.putExtra(Settings.EXTRA_CHANNEL_ID, CHANNEL);
        } else intent.setData(android.net.Uri.parse("package:" + activity.getPackageName()));
        activity.startActivity(intent);
    }
    private static PendingIntent alarm(Context context, int slot, String generation) {
        Intent intent = new Intent(context, KomboNotifications.class).setAction(FIRE)
                .putExtra("slot", slot).putExtra("generation", generation);
        return PendingIntent.getBroadcast(context, BASE + slot, intent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
    }
    private static void cancel(Context context) {
        AlarmManager alarms = (AlarmManager) context.getSystemService(Context.ALARM_SERVICE);
        for (int i = 0; i < 56; ++i) {
            alarms.cancel(alarm(context, i, ""));
            manager(context).cancel(CHANNEL, BASE + i);
        }
    }
    static synchronized void returned(Context context, Intent intent) {
        foreground = true;
        acceptTap(intent);
        cancel(context);
        // Commit before returning: a queued broadcast cannot revive this plan.
        prefs(context).edit().remove("plan").remove("generation").commit();
    }
    static synchronized void acceptTap(Intent intent) {
        if (intent != null && TAP.equals(intent.getAction())) {
            String profile = intent.getStringExtra("komboProfile");
            if (profile != null) tapProfile = profile;
            intent.removeExtra("komboProfile");
        }
    }
    static synchronized void paused() { foreground = false; }
    static synchronized String takeTap() {
        String result = tapProfile; tapProfile = ""; return result;
    }
    static synchronized void replace(Context context, String encoded) {
        cancel(context);
        String generation = UUID.randomUUID().toString();
        prefs(context).edit().putString("plan", encoded).putString("generation", generation).commit();
        reschedule(context);
    }
    private static long time(JSONObject request) throws Exception {
        // Calendar uses the current local zone, including after TIMEZONE_CHANGED.
        return KomboReminderTime.at(request.getInt("year"), request.getInt("month"), request.getInt("day"),
                request.getInt("hour"), request.getInt("minute"), TimeZone.getDefault());
    }
    private static JSONArray plan(Context context) throws Exception {
        return new JSONArray(prefs(context).getString("plan", "[]"));
    }
    private static void reschedule(Context context) {
        if (!permission(context).equals("granted")) return;
        try {
            JSONArray requests = plan(context);
            String generation = prefs(context).getString("generation", "");
            AlarmManager alarms = (AlarmManager) context.getSystemService(Context.ALARM_SERVICE);
            for (int i = 0; i < Math.min(requests.length(), 56); ++i) {
                JSONObject request = requests.optJSONObject(i);
                if (request == null) continue;
                long at = time(request);
                if (at <= System.currentTimeMillis()) continue;
                PendingIntent pending = alarm(context, i, generation);
                if (Build.VERSION.SDK_INT >= 23) alarms.setAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, at, pending);
                else alarms.set(AlarmManager.RTC_WAKEUP, at, pending);
            }
        } catch (Exception e) { android.util.Log.w("KomboNotifications", "Cannot schedule reminders", e); }
    }
    @Override public void onReceive(Context context, Intent intent) {
        synchronized (KomboNotifications.class) {
            if (!FIRE.equals(intent.getAction())) {
                cancel(context);
                reschedule(context);
                return;
            }
            if (!prefs(context).getString("generation", "").equals(intent.getStringExtra("generation"))) return;
            int slot = intent.getIntExtra("slot", -1);
            if (slot < 0 || slot >= 56) return;
            try {
                JSONArray requests = plan(context);
                JSONObject request = requests.optJSONObject(slot);
                if (request == null) return;
                // Consume before delivery: duplicate broadcasts never repeat an alert.
                requests.put(slot, JSONObject.NULL);
                prefs(context).edit().putString("plan", requests.toString()).commit();
                if (foreground || !permission(context).equals("granted") ||
                        !KomboReminderTime.isDue(System.currentTimeMillis(), request.getInt("year"),
                            request.getInt("month"), request.getInt("day"), request.getInt("hour"),
                            request.getInt("minute"), TimeZone.getDefault())) return;
                Intent tap = new Intent(context, AsoBMaShowActivity.class).setAction(TAP)
                        .putExtra("komboProfile", request.getString("profile"))
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
                PendingIntent content = PendingIntent.getActivity(context, BASE + slot, tap,
                        PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
                Notification.Builder builder = Build.VERSION.SDK_INT >= 26
                        ? new Notification.Builder(context, CHANNEL) : new Notification.Builder(context);
                manager(context).notify(CHANNEL, BASE + slot, builder.setSmallIcon(R.drawable.ic_music_note)
                        .setContentTitle(request.getString("title")).setContentText(request.getString("body"))
                        .setContentIntent(content).setAutoCancel(true).build());
            } catch (Exception e) { android.util.Log.w("KomboNotifications", "Cannot deliver reminder", e); }
        }
    }
}
