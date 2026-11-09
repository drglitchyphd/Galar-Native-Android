// FORK: "Keep running in the background" (System menu, DM_FORK_KEEPALIVE=on; default off = stock). While the game is open, a foreground service with a quiet
// notification ("Galar Fork is running") tells Android the app is in use, so it is not one of the first apps closed for memory once it goes to the background
// (the game holds about 2.4 GB). It does not stop a phone that truly runs out of memory, and the game keeps using the battery while it runs in the background:
// "Leaving the game: Pause" (DM_BACKGROUND=off) next to it stops the game itself there.
// Started from GameActivity while it is on screen (Android 12+ refuses a foreground service started from the background); stopped when the game ends.
package com.nxnative;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

public final class ForkKeepAlive extends Service {
    private static final String CH = "fork_keepalive"; private static final int ID = 7701;

    static boolean wanted(Context c) { return "on".equals(GameSettings.get(c, "DM_FORK_KEEPALIVE")); }
    // GameActivity: on its start and each time it comes back on screen (the setting may have changed in the menu)
    static void sync(Context c) {
        try {
            Intent i = new Intent(c, ForkKeepAlive.class);
            if (wanted(c)) c.startForegroundService(i); else c.stopService(i);
        } catch (RuntimeException e) { Log.w(Fork.TAG, "[FORK] keep-alive: " + e); }
    }
    static void stop(Context c) { try { c.stopService(new Intent(c, ForkKeepAlive.class)); } catch (RuntimeException e) { } }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null && nm.getNotificationChannel(CH) == null) {
            NotificationChannel ch = new NotificationChannel(CH, T.t(this, "Game running in the background"), NotificationManager.IMPORTANCE_LOW);
            ch.setShowBadge(false); nm.createNotificationChannel(ch); }
        Intent open = new Intent(this, GameActivity.class).addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
        PendingIntent pi = PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Notification n = new Notification.Builder(this, CH).setSmallIcon(getApplicationInfo().icon).setContentTitle(T.t(this, "%s is running", GameSettings.app(this)[0]))
            .setContentText(T.t(this, "Kept open in the background. Tap to go back to the game.")).setContentIntent(pi).setOngoing(true).build();
        try {
            if (Build.VERSION.SDK_INT >= 34) startForeground(ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE); else startForeground(ID, n);
            Log.i(Fork.TAG, "[FORK] keep-alive: on");
        } catch (RuntimeException e) { Log.w(Fork.TAG, "[FORK] keep-alive: could not start: " + e); stopSelf(); }
        return START_NOT_STICKY;   // (not brought back by itself: the game it keeps is gone then)
    }
    @Override public void onDestroy() { Log.i(Fork.TAG, "[FORK] keep-alive: off"); super.onDestroy(); }
    @Override public IBinder onBind(Intent i) { return null; }
}
