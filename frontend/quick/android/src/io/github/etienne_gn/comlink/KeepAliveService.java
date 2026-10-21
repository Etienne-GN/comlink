package io.github.etienne_gn.comlink;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

/* Keeps the process alive, and nothing else.
 *
 * The core, the SIP stack and the whole interface run in the
 * activity's process. Android kills that process once the window goes
 * away, and a softphone that stops being reachable when it is not on
 * screen is not a phone. This service does no work of its own: it
 * holds a foreground notification so the process it shares is left
 * alone.
 *
 * Deliberately Java, deliberately empty, and deliberately in the same
 * process. A Qt service would be a second QGuiApplication in a second
 * process, with the core on the wrong side of it.
 *
 * Two constraints come with the phoneCall type, both recorded in
 * docs/roadmap.md: startForeground() must happen within five seconds
 * of the start request or the service is lost to
 * ForegroundServiceDidNotStartInTimeException, and on Android 15 and
 * up this type cannot be started from BOOT_COMPLETED. Nothing here
 * blocks, so the five seconds are never in question. */
public class KeepAliveService extends Service {

    private static final String CHANNEL = "comlink.registration";
    private static final int NOTE_ID = 1;

    @Override
    public void onCreate() {
        super.onCreate();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationManager nm =
                getSystemService(NotificationManager.class);
            /* LOW: an ongoing notification that makes a sound every
               time it is touched is worse than none. */
            NotificationChannel c = new NotificationChannel(
                CHANNEL, "Comlink", NotificationManager.IMPORTANCE_LOW);
            c.setShowBadge(false);
            nm.createNotificationChannel(c);
        }
        startInForeground();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        /* START_STICKY: if Android kills the process for memory this
           is what comes back, and the window comes back with it,
           because they share one process. */
        return START_STICKY;
    }

    private void startInForeground() {
        Intent open = new Intent();
        open.setClassName(getPackageName(),
                          "org.qtproject.qt.android.bindings.QtActivity");
        open.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                      | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent pi = PendingIntent.getActivity(
            this, 0, open, PendingIntent.FLAG_IMMUTABLE);

        Notification.Builder b =
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? new Notification.Builder(this, CHANNEL)
                : new Notification.Builder(this);
        Notification n = b.setContentTitle("Comlink")
                          .setContentText("Reachable")
                          .setSmallIcon(android.R.drawable.stat_sys_phone_call)
                          .setContentIntent(pi)
                          .setOngoing(true)
                          .build();

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
            startForeground(NOTE_ID, n,
                            ServiceInfo.FOREGROUND_SERVICE_TYPE_PHONE_CALL);
        else
            startForeground(NOTE_ID, n);
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }
}
