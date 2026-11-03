package dev.wakamex.gesso;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Build;
import android.util.Log;

// gs_notify on Android (src/gs_notify_android.c): notifications on a channel of the app's. A click
// opens NotifyActivity, which hands the tag to the app and brings the app forward; an app whose
// manifest does not declare NotifyActivity is brought forward without the tag.
public final class Notify {
    static final String TAG = "dev.wakamex.gesso.tag";

    static native void clicked(String tag);

    static volatile boolean ready;  // the native side is listening
    private static Context context;
    private static String channel;
    private static Bitmap icon;
    private static boolean trampoline;

    public static boolean init(Activity activity, String name, String id, String iconPath) {
        context = activity.getApplicationContext();
        NotificationManager manager = context.getSystemService(NotificationManager.class);
        if (manager == null) return false;
        channel = id;
        if (Build.VERSION.SDK_INT >= 26) manager.createNotificationChannel(new NotificationChannel(id, name, NotificationManager.IMPORTANCE_DEFAULT));
        icon = iconPath != null ? BitmapFactory.decodeFile(iconPath) : null;
        try {
            context.getPackageManager().getActivityInfo(new ComponentName(context, NotifyActivity.class), 0);
            trampoline = true;
        } catch (Exception e) {
            Log.w("gesso", "dev.wakamex.gesso.NotifyActivity is not in the manifest: a click opens the app without its tag");
        }
        ready = true;
        return true;
    }

    public static boolean show(String tag, String title, String body) {
        try {
            NotificationManager manager = context.getSystemService(NotificationManager.class);
            if (Build.VERSION.SDK_INT >= 24 && !manager.areNotificationsEnabled()) return false;
            Intent open = trampoline
                ? new Intent(context, NotifyActivity.class).putExtra(TAG, tag).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                : context.getPackageManager().getLaunchIntentForPackage(context.getPackageName());
            PendingIntent click = PendingIntent.getActivity(context, tag.hashCode(), open, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            Notification.Builder b = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(context, channel) : new Notification.Builder(context);
            int small = context.getApplicationInfo().icon;
            b.setSmallIcon(small != 0 ? small : android.R.drawable.sym_def_app_icon)
                .setContentTitle(title)
                .setContentText(body)
                .setStyle(new Notification.BigTextStyle().bigText(body))
                .setContentIntent(click)
                .setAutoCancel(true);
            if (icon != null) b.setLargeIcon(icon);
            manager.notify(tag, 0, b.build());
            return true;
        } catch (Exception e) {
            Log.w("gesso", "notification not shown", e);
            return false;
        }
    }

    public static void close(String tag) {
        context.getSystemService(NotificationManager.class).cancel(tag, 0);
    }
}
