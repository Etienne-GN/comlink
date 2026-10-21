package io.github.etienne_gn.comlink;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.os.Build;

/* What the phone says when the window is not on screen.
 *
 * KeepAliveService holds the process up; it says only "Reachable",
 * deliberately, because an ongoing notification that changes with
 * every event is an ongoing notification nobody reads. An incoming
 * call is the one thing that has to interrupt, so it gets its own
 * channel at high importance and its own notification -- which is
 * what makes it a heads-up rather than a line in the shade.
 *
 * Tapping it opens the window. Not a full-screen intent: that needs
 * USE_FULL_SCREEN_INTENT, which on Android 14 and up is granted only
 * to applications the system recognises as calling or alarm apps, and
 * claiming to be one without the Telecom integration to back it would
 * be asking for a permission this application has not earned yet.
 * Telecom integration proper is a later milestone; until then a
 * heads-up notification is the honest amount of interruption. */
public final class Notifier {

    private static final String CHANNEL = "comlink.incoming";
    private static final int NOTE_ID = 2;

    private static NotificationManager manager(Context ctx) {
        NotificationManager nm =
            (NotificationManager) ctx.getSystemService(
                Context.NOTIFICATION_SERVICE);
        if (nm != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel c = new NotificationChannel(
                CHANNEL, "Incoming calls",
                NotificationManager.IMPORTANCE_HIGH);
            c.setShowBadge(true);
            nm.createNotificationChannel(c);
        }
        return nm;
    }

    public static void incoming(Context ctx, String who) {
        NotificationManager nm = manager(ctx);
        if (nm == null)
            return;

        Intent open = new Intent();
        open.setClassName(ctx.getPackageName(),
                          "org.qtproject.qt.android.bindings.QtActivity");
        open.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                      | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent pi = PendingIntent.getActivity(
            ctx, 0, open, PendingIntent.FLAG_IMMUTABLE
                          | PendingIntent.FLAG_UPDATE_CURRENT);

        Notification.Builder b =
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? new Notification.Builder(ctx, CHANNEL)
                : new Notification.Builder(ctx);
        Notification n = b.setContentTitle("Incoming call")
                          .setContentText(who)
                          .setSmallIcon(android.R.drawable.sym_call_incoming)
                          .setContentIntent(pi)
                          .setCategory(Notification.CATEGORY_CALL)
                          .setAutoCancel(true)
                          .setOngoing(true)
                          .build();
        nm.notify(NOTE_ID, n);
    }

    public static void clearIncoming(Context ctx) {
        NotificationManager nm =
            (NotificationManager) ctx.getSystemService(
                Context.NOTIFICATION_SERVICE);
        if (nm != null)
            nm.cancel(NOTE_ID);
    }

    private Notifier() {}
}
