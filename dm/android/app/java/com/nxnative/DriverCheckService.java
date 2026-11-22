package com.nxnative;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

// The driver check's process (":drvcheck" in the manifest): libdmprobe.so's GPU self-tests on the driver the game uses (DriverCheck.java starts it and reads its journal).
// A driver crash here ends only this process; the app sees the test that was running and leaves it out the next time. The process ends itself when the tests are done.
public final class DriverCheckService extends Service {
    static native int run(String journal, String report, String envTxt, String skip);
    // one run in this process: the self-tests keep their state in globals (selftest.c), and the first run's end ends the process (a second start: ignored)
    private static final java.util.concurrent.atomic.AtomicBoolean RUNNING = new java.util.concurrent.atomic.AtomicBoolean();

    @Override public int onStartCommand(Intent it, int flags, int id) {
        final String journal = it == null ? null : it.getStringExtra("journal"), report = it == null ? "" : it.getStringExtra("report"),
                     env = it == null ? "" : it.getStringExtra("env"), skip = it == null ? "" : it.getStringExtra("skip");
        if (journal == null) { stopSelf(); return START_NOT_STICKY; }
        if (!RUNNING.compareAndSet(false, true)) return START_NOT_STICKY;
        new Thread(() -> {
            try { System.loadLibrary("dmprobe"); run(journal, report == null ? "" : report, env == null ? "" : env, skip == null ? "" : skip); }
            catch (Throwable t) { try (java.io.FileWriter w = new java.io.FileWriter(journal, true)) { w.write("error " + t + "\n"); } catch (java.io.IOException e) { } }
            System.exit(0);   // (the driver's objects with it: nothing of this process is kept)
        }, "driver-check").start();
        return START_NOT_STICKY;
    }
    @Override public IBinder onBind(Intent it) { return null; }
}
