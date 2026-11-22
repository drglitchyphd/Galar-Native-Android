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
//   each test in a journal before it starts; a test that takes that process down is left out of the next attempt and its setting applied (the one its last "begin"
//   line named: a later step of a test can name another, selftest.c crash_fix). A test that says nothing
//   for TEST_TIMEOUT_MS (hung, or only slow: building the BC layer's pipelines on an old Mali) is left out too, but its setting is not applied: it is named in
//   driver-check.txt and driver-check.env's comment (the game's own crash markers, bc_gpu_crashed and heap_import_crashed, still cover a hang the player ends).
//   What the tests ask for goes to driver-check.env, which the game reads after env.txt and the safe-start ladder and before the phone profile (android_main.c): the
//   player's own settings and the ladder's levels always win, and the game takes the check's values as this driver's defaults (its crash markers still apply to them).
//   One check at a time (claim / release): a second Play, or the setup screen made again while one runs, only takes over what runs after it; DriverCheckService
//   ignores a second start too (the tests' state is global in libdmprobe.so). A check that does not finish (the app itself closed during it: the player left, or the
//   driver's crash took the app's GPU context with it) is resumed at the next Play from its journal (driver-check.run counts such starts): what it learned is kept,
//   the test that was running when the app closed left out with no setting; after MAX_STARTS unfinished starts the check gives up with what it has.
//   Files in the game's folder: driver-check.log (the journal), driver-check.txt (the readable report), driver-check.env, driver-check.key, driver-check.run (an
//   unfinished check). DM_DRIVER_CHECK=0 in env.txt: no check (and no driver-check.env read: android_main.c)
final class DriverCheck {
    // raise when the tests or what they set change: every phone checks again (2: int-view on 32-bit textures, int-view-own; no setting after a hang. 3: 3d-layered clears
    //   inside its pass as the engine does on this driver, a pass of its own per slice on Xclipse, and a crash in its vkCmdClearAttachments asks for DM_VK_CLEAR_BY_PASS=1,
    //   not slice 0 only, which version 2 could write on Xclipse; int-view-own also reads a 16-bit integer under a float sampler. 4: 3d-layered records what the
    //   engine records on the driver: dynamic rendering where the engine uses it, the engine's own clear (layer 0, a part), each step a submission of its own, so
    //   DM_VK_CLEAR_BY_PASS=1 comes only from the clear (or a clear drawn wrong); the Vulkan 1.0 device as the engine makes it)
    static final int VERSION = 4;
    private static final int ATTEMPTS = 4, MAX_STARTS = 2;   // (MAX_STARTS: checks of one driver that may end unfinished; the next one gives up)
    private static final long TEST_TIMEOUT_MS = 20000, TOTAL_TIMEOUT_MS = 90000;
    private static final java.util.concurrent.atomic.AtomicBoolean RUNNING = new java.util.concurrent.atomic.AtomicBoolean();
    private static Runnable s_go;   // what runs when the running check ends: the latest thenRun's (main thread only)

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
    static void forget(File gameDir) { for (String n : new String[] { "driver-check.key", "driver-check.env", "driver-check.run" }) new File(gameDir, n).delete(); }

    // Runs the check when this driver has not had one, then go (on the main thread). Whatever happens, go runs: the check never stands between the player and the game.
    //   Called on the main thread. While a check runs, another call only replaces what runs after it (claim / release): one check, one go, the latest one asked for
    static void thenRun(Activity a, Runnable go) {
        if (!wanted(a)) { go.run(); return; }
        if (!claim(go)) return;   // (a check is running: its end runs this go)
        android.widget.Toast.makeText(a, T.t(a, "Checking the graphics driver (once, a few seconds)…"), android.widget.Toast.LENGTH_LONG).show();
        Handler main = new Handler(Looper.getMainLooper()); Context app = a.getApplicationContext();
        new Thread(() -> {
            try { check(app); } catch (Throwable t) { append(new File(dir(app), "driver-check.log"), "error " + t + "\n"); }
            finally { main.post(() -> { Runnable g = release(); if (g != null) g.run(); }); }
        }, "driver-check-wait").start();
    }
    // (main thread) go becomes what runs after the check; true: no check was running, the caller starts one
    static boolean claim(Runnable go) { s_go = go; return RUNNING.compareAndSet(false, true); }
    // (main thread, the check over) what runs now; the next thenRun may check again
    static Runnable release() { Runnable g = s_go; s_go = null; RUNNING.set(false); return g; }

    private static void check(Context c) throws InterruptedException {
        File d = dir(c), journal = new File(d, "driver-check.log"), report = new File(d, "driver-check.txt"), run = new File(d, "driver-check.run");
        String key = key(c);
        List<String> skip = new ArrayList<>(); Map<String, String> fixes = new LinkedHashMap<>(); List<String> crashed = new ArrayList<>();
        // an unfinished check of this driver before (driver-check.run: its key, then the starts it took): resumed from its journal, or given up after MAX_STARTS
        int starts = unfinished(read(run), key);
        if (starts > 0) { resume(read(journal), skip, fixes, crashed); append(journal, "resumed after " + starts + " unfinished start" + (starts == 1 ? "" : "s") + ", skip " + String.join(",", skip) + "\n"); }
        else { journal.delete(); report.delete(); }
        if (starts >= MAX_STARTS) {
            append(report, "[GAVE UP] the check did not finish at " + starts + " starts (the app closed during it): what it found is kept, and it is not run again for this driver\n");
            finish(d, key, fixes, crashed, false, journal, run); return; }
        write(run, key + "\n" + (starts + 1) + "\n");   // (deleted when this check ends; still there when the app closed during it, or the service could not start)
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
                    boolean applied = open != null && openFix != null && !hung && openFix.indexOf('=') > 0;
                    if (open != null) { skip.add(open); crashed.add(open + (hung ? " (hung: no setting)" : ""));
                        // (a crash: its setting; a hang may be a slow phone, so it only goes into the report)
                        if (applied) { int eq = openFix.indexOf('='); fixes.put(openFix.substring(0, eq), openFix.substring(eq + 1)); }
                        if (hung) append(report, "[HUNG] " + open + ": no answer for " + TEST_TIMEOUT_MS / 1000 + " s: left out of the next attempt, nothing changed for it"
                                                 + (openFix != null ? " (" + openFix + " only after a crash)" : "") + "\n"); }
                    append(journal, "lost " + (open == null ? "-" : open) + (hung ? " hung" : " crashed") + (applied ? " " + openFix : "") + "\n");   // (resume reads its setting back)
                    break;
                }
            }
            if (!done && result == null) break;   // (the process ended outside any test: another attempt would do the same)
        }
        finish(d, key, fixes, crashed, done, journal, run);
    }
    // the check's end: its settings into driver-check.env, the key written (this driver is checked), driver-check.run gone
    private static void finish(File d, String key, Map<String, String> fixes, List<String> crashed, boolean done, File journal, File run) {
        StringBuilder env = new StringBuilder("# The driver check's settings for this driver (DriverCheck.java; driver-check.txt says why). env.txt and the safe-start ladder win over these\n");
        for (Map.Entry<String, String> e : fixes.entrySet()) env.append(e.getKey()).append('=').append(e.getValue()).append('\n');
        if (!crashed.isEmpty()) env.append("# tests that took the driver down: ").append(String.join(", ", crashed)).append('\n');
        write(new File(d, "driver-check.env"), env.toString());
        write(new File(d, "driver-check.key"), key + "\n");
        run.delete();
        append(journal, "summary " + (done ? "complete" : "incomplete") + ", " + fixes.size() + " settings, crashed: " + (crashed.isEmpty() ? "none" : String.join(" ", crashed)) + "\n");
    }
    // driver-check.run's text: the starts an unfinished check of this key took so far (0: none, or another key's)
    static int unfinished(String run, String key) {
        String[] rl = run.split("\n");
        if (rl.length < 2 || !rl[0].equals(key)) return 0;
        try { return Math.max(0, Integer.parseInt(rl[1].trim())); } catch (NumberFormatException e) { return 0; }
    }
    // What an unfinished check's journal says, as the check read it while it ran: the tests lost ("lost <test> crashed|hung [KEY=VALUE]": left out, a crash's
    //   setting applied), the settings tests asked for ("fix <test> KEY=VALUE"), and the test that was running when the app itself closed (the last "begin" with no
    //   "result", "lost" or end of its process after it): left out with no setting, as a hang (the app closing may be the player's doing)
    static void resume(String journal, List<String> skip, Map<String, String> fixes, List<String> crashed) {
        String open = null;
        for (String l : journal.split("\n")) {
            String[] w = l.split(" ");
            if (l.startsWith("attempt ") || l.startsWith("done") || l.startsWith("nodevice") || l.startsWith("error") || l.startsWith("summary")) open = null;
            else if (l.startsWith("begin ") && w.length > 1) open = w[1];
            else if (l.startsWith("result ") && open != null && w.length > 1 && w[1].equals(open)) open = null;
            else if (l.startsWith("fix ") && w.length > 2) { int eq = w[2].indexOf('='); if (eq > 0) fixes.put(w[2].substring(0, eq), w[2].substring(eq + 1)); }
            else if (l.startsWith("lost ") && w.length > 2) {
                open = null;
                if (w[1].equals("-")) continue;
                if (!skip.contains(w[1])) { skip.add(w[1]); crashed.add(w[1] + (w[2].equals("hung") ? " (hung: no setting)" : "")); }
                if (w.length > 3) { int eq = w[3].indexOf('='); if (eq > 0) fixes.put(w[3].substring(0, eq), w[3].substring(eq + 1)); } }
        }
        if (open != null && !skip.contains(open)) { skip.add(open); crashed.add(open + " (the app closed during it: no setting)"); }
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
