package com.nxnative;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

// The driver check before Play: a few seconds of GPU self-tests (the probe app's, libdmprobe.so) on the driver the game will use, once per driver: again when the
//   phone's system (and with it the stock driver), the chosen driver package or the app changes. Crash-safe: the tests run in DriverCheckService's own process and note
//   each test in a journal before it starts; a test that takes that process down is left out of the next attempt and its setting applied. A test that says nothing
//   for TEST_TIMEOUT_MS (hung, or only slow: building the BC layer's pipelines on an old Mali) is left out too, but its setting is not applied: it is named in
//   driver-check.txt and driver-check.env's comment (the game's own crash markers, bc_gpu_crashed and heap_import_crashed, still cover a hang the player ends).
//   What the tests ask for goes to driver-check.env, which the game reads after env.txt and the safe-start ladder and before the phone profile (android_main.c): the
//   player's own settings and the ladder's levels always win, and the game takes the check's values as this driver's defaults (its crash markers still apply to them).
//   Files in the game's folder: driver-check.log (the journal), driver-check.txt (the readable report), driver-check.env, driver-check.key. DM_DRIVER_CHECK=0 in
//   env.txt: no check (and no driver-check.env read: android_main.c)
final class DriverCheck {
    static final int VERSION = 2;   // raise when the tests or what they set change: every phone checks again (2: int-view on 32-bit textures, int-view-own; no setting after a hang)
    private static final int ATTEMPTS = 4;
    private static final long TEST_TIMEOUT_MS = 20000, TOTAL_TIMEOUT_MS = 90000;

    static File dir(Context c) { return GameSettings.dir(c); }
    // what the results are for: this system build (its stock driver), the chosen driver package, this app
    static String key(Context c) {
        long vc = 0; try { vc = c.getPackageManager().getPackageInfo(c.getPackageName(), 0).getLongVersionCode(); } catch (Exception e) { }
        return VERSION + "|" + Build.FINGERPRINT + "|" + DriverPackages.current(c) + "|" + vc;
    }
    static boolean wanted(Context c) {
        if ("0".equals(GameSettings.get(c, "DM_DRIVER_CHECK"))) return false;
        return !key(c).equals(read(new File(dir(c), "driver-check.key")).trim());
    }
    // every game's results dropped (DriverPackages: the driver changed for all of them)
    static void forget(File gameDir) { for (String n : new String[] { "driver-check.key", "driver-check.env" }) new File(gameDir, n).delete(); }

    // Runs the check when this driver has not had one, then go (on the main thread). Whatever happens, go runs: the check never stands between the player and the game
    static void thenRun(Activity a, Runnable go) {
        if (!wanted(a)) { go.run(); return; }
        android.widget.Toast.makeText(a, T.t(a, "Checking the graphics driver (once, a few seconds)…"), android.widget.Toast.LENGTH_LONG).show();
        Handler main = new Handler(Looper.getMainLooper());
        new Thread(() -> {
            try { check(a.getApplicationContext()); } catch (Throwable t) { append(new File(dir(a), "driver-check.log"), "error " + t + "\n"); }
            main.post(go);
        }, "driver-check-wait").start();
    }

    private static void check(Context c) throws InterruptedException {
        File d = dir(c), journal = new File(d, "driver-check.log"), report = new File(d, "driver-check.txt");
        journal.delete(); report.delete();
        List<String> skip = new ArrayList<>(); Map<String, String> fixes = new LinkedHashMap<>(); List<String> crashed = new ArrayList<>();
        long t0 = System.currentTimeMillis(); boolean done = false;
        for (int attempt = 0; attempt < ATTEMPTS && !done && System.currentTimeMillis() - t0 < TOTAL_TIMEOUT_MS; attempt++) {
            append(journal, "attempt " + attempt + " skip " + String.join(",", skip) + "\n");
            long mark = journal.length();
            Intent it = new Intent(c, DriverCheckService.class).putExtra("journal", journal.getPath()).putExtra("report", report.getPath())
                .putExtra("env", GameSettings.envFile(c).getPath()).putExtra("skip", String.join(",", skip));
            try { c.startService(it); } catch (Exception e) { append(journal, "error start " + e + "\n"); return; }
            String proc = c.getPackageName() + ":drvcheck"; boolean seen = false; long last = System.currentTimeMillis(), lastLen = -1; String result = null;
            while (true) {
                Thread.sleep(150);
                String[] tail = since(journal, mark);
                String end = null, open = null, openFix = null;
                for (String l : tail) {
                    if (l.startsWith("done") || l.startsWith("nodevice") || l.startsWith("error")) end = l;
                    else if (l.startsWith("begin ")) { String[] w = l.split(" "); open = w[1]; openFix = w.length > 2 ? w[2] : null; }
                    else if (l.startsWith("result ") && open != null && l.split(" ")[1].equals(open)) { open = null; openFix = null; }
                    else if (l.startsWith("fix ")) { String[] w = l.split(" "); if (w.length > 2) { int eq = w[2].indexOf('='); if (eq > 0) fixes.put(w[2].substring(0, eq), w[2].substring(eq + 1)); } }
                }
                if (end != null) { done = true; break; }
                long len = journal.length(); if (len != lastLen) { lastLen = len; last = System.currentTimeMillis(); }
                int pid = pid(c, proc); if (pid > 0) seen = true;
                boolean gone = (seen && pid <= 0) || (!seen && System.currentTimeMillis() - last > 8000);
                boolean hung = pid > 0 && System.currentTimeMillis() - last > TEST_TIMEOUT_MS;
                if (gone || hung) {
                    if (hung) android.os.Process.killProcess(pid);
                    result = open;
                    if (open != null) { skip.add(open); crashed.add(open + (hung ? " (hung: no setting)" : ""));
                        // (a crash: its setting; a hang may be a slow phone, so it only goes into the report)
                        if (openFix != null && !hung) { int eq = openFix.indexOf('='); if (eq > 0) fixes.put(openFix.substring(0, eq), openFix.substring(eq + 1)); }
                        if (hung) append(report, "[HUNG] " + open + ": no answer for " + TEST_TIMEOUT_MS / 1000 + " s: left out of the next attempt, nothing changed for it"
                                                 + (openFix != null ? " (" + openFix + " only after a crash)" : "") + "\n"); }
                    append(journal, "lost " + (open == null ? "-" : open) + (hung ? " hung" : " crashed") + "\n");
                    break;
                }
            }
            if (!done && result == null) break;   // (the process ended outside any test: another attempt would do the same)
        }
        StringBuilder env = new StringBuilder("# The driver check's settings for this driver (DriverCheck.java; driver-check.txt says why). env.txt and the safe-start ladder win over these\n");
        for (Map.Entry<String, String> e : fixes.entrySet()) env.append(e.getKey()).append('=').append(e.getValue()).append('\n');
        if (!crashed.isEmpty()) env.append("# tests that took the driver down: ").append(String.join(", ", crashed)).append('\n');
        write(new File(d, "driver-check.env"), env.toString());
        write(new File(d, "driver-check.key"), key(c) + "\n");
        append(journal, "summary " + (done ? "complete" : "incomplete") + ", " + fixes.size() + " settings, crashed: " + (crashed.isEmpty() ? "none" : String.join(" ", crashed)) + "\n");
    }

    private static int pid(Context c, String name) {
        ActivityManager am = (ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE);
        List<ActivityManager.RunningAppProcessInfo> ps = am == null ? null : am.getRunningAppProcesses();
        if (ps != null) for (ActivityManager.RunningAppProcessInfo p : ps) if (name.equals(p.processName)) return p.pid;
        return -1;
    }
    private static String[] since(File f, long mark) {
        String s = read(f); if (s.length() <= mark) return new String[0];
        return s.substring((int) Math.min(mark, s.length())).split("\n");
    }
    static String read(File f) {
        StringBuilder b = new StringBuilder();
        try (BufferedReader r = new BufferedReader(new FileReader(f))) { String l; while ((l = r.readLine()) != null) b.append(l).append('\n'); } catch (IOException e) { }
        return b.toString();
    }
    private static void append(File f, String s) { try (FileWriter w = new FileWriter(f, true)) { w.write(s); } catch (IOException e) { } }
    private static void write(File f, String s) { try (FileWriter w = new FileWriter(f)) { w.write(s); } catch (IOException e) { } }
}
