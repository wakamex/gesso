package dev.wakamex.gesso;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

// gs_background on Android (src/gs_background.c): a foreground service with an ongoing notification,
// which keeps the app's process running and online while another app is in front. Its type is the one
// the app's manifest declares for it (see gs_background.h).
public final class Background extends Service {
    private static final String CHANNEL = "dev.wakamex.gesso.background";
    private static volatile String title = "", text = "";

    public static boolean start(Activity activity, String title, String text) {
        Background.title = title;
        Background.text = text;
        try {
            Intent intent = new Intent(activity, Background.class);
            if (Build.VERSION.SDK_INT >= 26) activity.startForegroundService(intent);
            else activity.startService(intent);
            return true;
        } catch (Exception e) {  // (not declared, or started from the background on Android 12 and later)
            Log.w("gesso", "the background service did not start", e);
            return false;
        }
    }

    public static void stop(Activity activity) {
        activity.stopService(new Intent(activity, Background.class));
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        try {
            NotificationManager manager = getSystemService(NotificationManager.class);
            if (Build.VERSION.SDK_INT >= 26) manager.createNotificationChannel(new NotificationChannel(CHANNEL, title, NotificationManager.IMPORTANCE_LOW));
            Intent open = getPackageManager().getLaunchIntentForPackage(getPackageName());
            Notification.Builder b = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(this, CHANNEL) : new Notification.Builder(this);
            int small = getApplicationInfo().icon;
            b.setSmallIcon(small != 0 ? small : android.R.drawable.sym_def_app_icon)
                .setContentTitle(title)
                .setContentText(text)
                .setOngoing(true)
                .setContentIntent(PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE));
            if (Build.VERSION.SDK_INT >= 29) {
                int type = getPackageManager().getServiceInfo(new ComponentName(this, Background.class), 0).getForegroundServiceType();
                startForeground(1, b.build(), type);
            } else {
                startForeground(1, b.build());
            }
        } catch (Exception e) {
            Log.w("gesso", "the background service could not come to the foreground", e);
            stopSelf();
        }
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }
}
