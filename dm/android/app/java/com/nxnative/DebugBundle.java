// The debug report: one ZIP in Download (<game>-debug-report.zip) with everything a bug report needs, collected by the app instead of typed by the player:
//   env.json            the phone (Build.*, SoC, page size, kernel), the GPU and its driver (the phone's own or a package, each library's SHA-256), the game and
//                       its version, the app's version and signer, memory, storage, heat, the display, the Auto profile, the mods
//   settings.txt        env.txt as it is (the player's settings and any test switches)
//   logs/               the engine's log of this run and of the run before (log.txt, log.prev.txt), the setup log, boot_state, safe_start, gpu.txt
//   exits/summary.txt   why Android ended this app's last runs (ApplicationExitInfo: crash, native crash, ANR, low memory, swiped away...)
//   exits/*.tombstone.pb  Android's own record of the last native crashes (protobuf: every thread's backtrace, registers, memory maps; crash.c hands each
//                       crash over to Android after writing its own record, so one exists); read it on a PC with dm/android/tools/read_debug_report.py
//   exits/*.anr.txt     the thread dump of an "app not responding"
//   shaders_at_crash.txt  the last pipelines the engine built before each log ended (vertex / fragment shader keys), the GPU breadcrumbs (vk_crumbs.h: the
//                       passes and pipelines of the submission that was on the GPU at a device lost / hang / crash) and the pack's translation chain: the shaders
//                       themselves are not copied (they come from the game), we take them from our own pack by key
//   quirks.txt          the driver workarounds each run used (vk_quirks.h "[quirk]" lines)
//   memory.txt          the run's memory every 30 s (android_main.c [mem] / [mem-by]: what grew, for a run Android ended for memory)
//   warnings.txt        the engine's [RED] / FATAL lines counted by kind (a picture problem's clues: missing shaders, failed pipelines, skipped draws)
//   frame-capture.zip   the newest ☰ → Capture a frame, when one was made in the last two days
//   logcat.txt          the system log lines this app may read (its own process: Java exceptions, the Vulkan loader, the driver's messages)
// Nothing from the game's files, the keys or the save goes in. Made from ☰ → Bug reports, and offered once after a run that crashed (SetupActivity).
// The GitHub issue it goes with is opened pre-filled (issueUrl: phone, GPU, version, game, driver, and a crash summary in case the player never attaches the ZIP).
package com.nxnative;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import org.json.JSONArray;
import org.json.JSONObject;

final class DebugBundle {
    private DebugBundle() { }
    static final String DL = "/storage/emulated/0/Download";
    private static final long CAP = 8L << 20;   // a log bigger than this goes in as its last 8 MB
    private static volatile boolean busy;

    // ---------- making it ----------
    // Builds the ZIP (a few seconds: the driver hashes, logcat) and returns the copy the player can reach: Download/<game>-debug-report.zip, else (Download not
    //   writable) the one in the game's folder. Not on the main thread
    static File make(Context c) throws IOException {
        File dir = GameSettings.dir(c), tmp = new File(dir, "debug-report.zip.part"), own = new File(dir, "debug-report.zip");
        try (ZipOutputStream z = new ZipOutputStream(new FileOutputStream(tmp))) {
            text(z, "README.txt", readme());
            String env; try { env = env(c).toString(2); } catch (Exception e) { env = "{\"error\": \"" + e + "\"}"; }
            text(z, "env.json", env);
            file(z, "settings.txt", new File(dir, "env.txt"));
            for (String n : new String[] { "driver-check.txt", "driver-check.log", "driver-check.env" }) file(z, "driver-check/" + n, new File(dir, n));   // (DriverCheck.java)
            file(z, "logs/log.txt", new File(dir, "log.txt"));
            file(z, "logs/log.prev.txt", new File(dir, "log.prev.txt"));
            file(z, "logs/setup.log", new File(new File(dir, "setup_work"), "setup.log"));
            for (String n : new String[] { "boot_state", "safe_start", "gpu.txt", "notice.txt", "ready", "captures_done" }) file(z, "logs/" + n, new File(dir, n));
            file(z, "logs/save_place.txt", new File(GameSettings.saveRoot(c), "save_place.txt"));   // (the chosen player's: Profiles)
            exits(c, z);
            text(z, "crash-summary.txt", crashSummary(c));   // (the lines the issue text carries)
            text(z, "shaders_at_crash.txt", shaders(c, dir));
            text(z, "memory.txt", memory(dir));
            text(z, "quirks.txt", grep(dir, "[quirk]", "The driver workarounds each run used (vk_quirks.h: the GPU described once, every workaround's value and whether env.txt or the driver check set it).\n"));
            text(z, "warnings.txt", warnings(dir));
            // the newest frame capture (☰ → Capture a frame), when one was made in the last two days: a picture problem's evidence, beside the logs
            File fc = new File(dir, "frame-capture.zip");
            if (fc.isFile() && fc.length() < (60L << 20) && System.currentTimeMillis() - fc.lastModified() < 48L * 3600 * 1000) file(z, "frame-capture.zip", fc, fc.length());
            text(z, "logcat.txt", logcat());
        }
        if (!tmp.renameTo(own)) { own.delete(); if (!tmp.renameTo(own)) throw new IOException("could not save " + own); }
        // a copy in Download, where the player finds it and the browser's file picker shows it. Android lets an app overwrite only files it made itself: after a
        //   reinstall the old name is someone else's, so the first of -debug-report.zip, -debug-report-2.zip ... that opens (as android_main.c dl_name does)
        String stem = GameSettings.game(c)[2];
        for (int k = 1; k <= 9; k++) {
            File d = new File(DL, stem + "-debug-report" + (k == 1 ? "" : "-" + k) + ".zip");
            try (InputStream in = new FileInputStream(own); OutputStream out = new FileOutputStream(d)) { copy(in, out, Long.MAX_VALUE); return d; }
            catch (IOException e) { }
        }
        return own;
    }
    private static String readme() {
        return "Debug report (DebugBundle.java). Open env.json first: phone, GPU, driver, game, app version.\n"
             + "exits/summary.txt: why Android ended the last runs; exits/*.tombstone.pb: Android's native crash records (dm/android/tools/read_debug_report.py prints them).\n"
             + "logs/log.prev.txt: the engine's log of the run before this report was made (the crashed one, after a crash); logs/log.txt: the current or last run.\n"
             + "shaders_at_crash.txt: the last pipelines built before each log ended (shader keys; take the shaders from a pack of the same chain).\n"
             + "No game files, keys or saves are in this file.\n";
    }

    // ---------- env.json ----------
    static JSONObject env(Context c) throws Exception {
        JSONObject j = new JSONObject();
        j.put("made", new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", Locale.ROOT).format(new java.util.Date()));
        JSONObject app = new JSONObject(); String[] an = GameSettings.app(c);
        app.put("name", an[0]); app.put("repo", an[1]); app.put("package", c.getPackageName());
        try { android.content.pm.PackageInfo pi = c.getPackageManager().getPackageInfo(c.getPackageName(), 0); app.put("versionName", pi.versionName); app.put("versionCode", pi.getLongVersionCode());
              app.put("installed", pi.firstInstallTime); app.put("updated", pi.lastUpdateTime); } catch (Exception e) { }
        try { String[] s = BuildSigner.digests(c); app.put("signer", BuildSigner.label(s)); app.put("official", BuildSigner.official(s)); } catch (Exception e) { }
        app.put("shaderChainWanted", asset(c, "shader_chain"));
        j.put("app", app);

        JSONObject g = new JSONObject(); String[] gm = GameSettings.game(c); File dir = GameSettings.dir(c);
        g.put("titleId", gm[0]); g.put("name", gm[1]); g.put("version", GameSettings.version(c));
        g.put("setUp", new File(dir, "ready").exists()); g.put("shaderChain", first(new File(dir, "ready")));
        g.put("packComplete", packComplete(dir)); g.put("packShaders", packCount(new File(dir, "shaders.pak")));
        g.put("savePlace", first(new File(GameSettings.saveRoot(c), "save_place.txt"))); g.put("saveProfile", Profiles.current(GameSettings.root(c)));   // (the player's id, not their name)
        j.put("game", g);

        JSONObject d = new JSONObject();
        d.put("manufacturer", Build.MANUFACTURER); d.put("brand", Build.BRAND); d.put("model", Build.MODEL); d.put("device", Build.DEVICE); d.put("product", Build.PRODUCT);
        d.put("hardware", Build.HARDWARE); d.put("board", Build.BOARD); d.put("socManufacturer", Build.SOC_MANUFACTURER); d.put("socModel", Build.SOC_MODEL);
        d.put("android", Build.VERSION.RELEASE); d.put("sdk", Build.VERSION.SDK_INT); d.put("securityPatch", Build.VERSION.SECURITY_PATCH); d.put("fingerprint", Build.FINGERPRINT);
        d.put("abis", new JSONArray(java.util.Arrays.asList(Build.SUPPORTED_ABIS))); d.put("kernel", System.getProperty("os.version"));
        try { d.put("pageSize", android.system.Os.sysconf(android.system.OsConstants._SC_PAGESIZE)); } catch (Exception e) { }
        d.put("cpuMaxKhz", GameSettings.maxCpuKhz()); d.put("cores", Runtime.getRuntime().availableProcessors());
        d.put("handheld", GameSettings.handheld());
        j.put("device", d);

        JSONObject gpu = new JSONObject();
        gpu.put("name", first(new File(dir, "gpu.txt")));
        String drv = DriverPackages.current(c);
        gpu.put("driver", drv.isEmpty() ? "system" : DriverPackages.label(drv));
        if (!drv.isEmpty()) { gpu.put("driverPath", drv); gpu.put("driverSha256", sha256(new File(drv))); gpu.put("driverSha256Setting", GameSettings.get(c, "DM_VK_DRIVER_SHA256"));
            File meta = new File(new File(drv).getParentFile(), "meta.json"); if (meta.isFile()) gpu.put("driverMeta", read(meta, 4096)); }
        // the phone's own driver libraries: which file the Vulkan loader opens (ro.hardware.vulkan) and its hash, so two players' "stock r44p1" can be told apart
        String hv = prop("ro.hardware.vulkan"), he = prop("ro.hardware.egl"); gpu.put("roHardwareVulkan", hv); gpu.put("roHardwareEgl", he); gpu.put("roBoardPlatform", prop("ro.board.platform"));
        JSONArray libs = new JSONArray();
        List<String> cand = new ArrayList<>();
        if (!hv.isEmpty()) { cand.add("/vendor/lib64/hw/vulkan." + hv + ".so"); }
        cand.add("/vendor/lib64/hw/vulkan.adreno.so"); cand.add("/vendor/lib64/hw/vulkan.mali.so"); cand.add("/vendor/lib64/egl/libGLES_mali.so"); cand.add("/vendor/lib64/libvulkan_adreno.so");
        if (!he.isEmpty()) cand.add("/vendor/lib64/egl/libGLES_" + he + ".so");
        for (String p : cand) { File f = new File(p); if (!f.isFile()) continue; boolean dup = false; for (int i = 0; i < libs.length(); i++) if (libs.getJSONObject(i).getString("path").equals(p)) dup = true; if (dup) continue;
            JSONObject l = new JSONObject(); l.put("path", p); l.put("size", f.length()); l.put("modified", f.lastModified()); l.put("sha256", sha256(f)); libs.put(l); }
        gpu.put("systemLibraries", libs);
        for (android.content.pm.FeatureInfo fi : c.getPackageManager().getSystemAvailableFeatures()) if (fi.name != null && fi.name.startsWith("android.hardware.vulkan"))
            gpu.put(fi.name.substring("android.hardware.".length()), String.format(Locale.ROOT, "%#x", fi.version));
        j.put("gpu", gpu);

        JSONObject s = new JSONObject();
        ActivityManager am = c.getSystemService(ActivityManager.class); ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo(); am.getMemoryInfo(mi);
        s.put("ramTotalMB", mi.totalMem >> 20); s.put("ramFreeMB", mi.availMem >> 20); s.put("lowMemoryLineMB", mi.threshold >> 20); s.put("lowMemory", mi.lowMemory);
        File ext = GameSettings.root(c); if (ext != null) s.put("storageFreeMB", ext.getUsableSpace() >> 20);
        android.os.PowerManager pm = c.getSystemService(android.os.PowerManager.class);
        if (pm != null) { s.put("thermalStatus", pm.getCurrentThermalStatus()); try { s.put("thermalHeadroom10s", pm.getThermalHeadroom(10)); } catch (Exception e) { } s.put("powerSave", pm.isPowerSaveMode());
            s.put("sustainedSupported", pm.isSustainedPerformanceModeSupported()); }
        Intent bat = c.registerReceiver(null, new android.content.IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        if (bat != null) { s.put("batteryTempC", bat.getIntExtra(android.os.BatteryManager.EXTRA_TEMPERATURE, 0) / 10.0); s.put("batteryLevel", bat.getIntExtra(android.os.BatteryManager.EXTRA_LEVEL, -1));
            s.put("charging", bat.getIntExtra(android.os.BatteryManager.EXTRA_PLUGGED, 0) != 0); }
        JSONArray hz = new JSONArray(); for (float r : GameSettings.refreshRates(c)) hz.put(Math.round(r)); s.put("refreshRates", hz);
        android.util.DisplayMetrics dm = c.getResources().getDisplayMetrics(); s.put("display", dm.widthPixels + "x" + dm.heightPixels + " @" + dm.densityDpi + "dpi");
        j.put("state", s);

        JSONArray prof = new JSONArray(); for (String p : GameSettings.profile(c)) prof.put(p); j.put("autoProfile", prof);   // { chip family, description, graphics, textures, aniso, fps, scaler, shadows, hd }
        JSONArray mods = new JSONArray(); File[] ms = new File(dir, "mods").listFiles();
        if (ms != null) for (File m : ms) { if (m.getName().startsWith(".")) continue; JSONObject o = new JSONObject(); o.put("name", m.getName());
            o.put("on", !m.getName().startsWith("-") && !m.getName().endsWith(".off")); o.put("folder", m.isDirectory()); mods.put(o); }
        j.put("mods", mods);
        j.put("lastExit", lastExitLine(c));
        return j;
    }

    // ---------- why Android ended the last runs ----------
    static String reason(int r) {
        switch (r) {
            case ApplicationExitInfo.REASON_EXIT_SELF: return "exit self"; case ApplicationExitInfo.REASON_SIGNALED: return "signaled";
            case ApplicationExitInfo.REASON_LOW_MEMORY: return "low memory"; case ApplicationExitInfo.REASON_CRASH: return "crash (Java)";
            case ApplicationExitInfo.REASON_CRASH_NATIVE: return "native crash"; case ApplicationExitInfo.REASON_ANR: return "not responding (ANR)";
            case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE: return "initialization failure"; case ApplicationExitInfo.REASON_PERMISSION_CHANGE: return "permission change";
            case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "excessive resource usage"; case ApplicationExitInfo.REASON_USER_REQUESTED: return "user requested";
            case ApplicationExitInfo.REASON_USER_STOPPED: return "user stopped"; case ApplicationExitInfo.REASON_DEPENDENCY_DIED: return "dependency died";
            case ApplicationExitInfo.REASON_OTHER: return "other"; case ApplicationExitInfo.REASON_FREEZER: return "freezer";
            case ApplicationExitInfo.REASON_PACKAGE_STATE_CHANGE: return "package state change"; case ApplicationExitInfo.REASON_PACKAGE_UPDATED: return "package updated";
            default: return "reason " + r;
        }
    }
    static List<ApplicationExitInfo> exitList(Context c, int n) {
        try { ActivityManager am = c.getSystemService(ActivityManager.class); return am.getHistoricalProcessExitReasons(null, 0, n); } catch (Exception e) { return new ArrayList<>(); }
    }
    // a run that ended badly: a crash of either kind or an ANR; also our own crash handler's old exit (status 128 + a fatal signal), for runs from before crash.c chained to Android
    static boolean crashed(ApplicationExitInfo e) {
        int r = e.getReason();
        if (r == ApplicationExitInfo.REASON_CRASH || r == ApplicationExitInfo.REASON_CRASH_NATIVE || r == ApplicationExitInfo.REASON_ANR) return true;
        // ended by Android for memory (or too much resource use) while on screen: the player saw the game vanish. In the background that is Android's normal tidying
        if ((r == ApplicationExitInfo.REASON_LOW_MEMORY || r == ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE)
            && e.getImportance() <= ActivityManager.RunningAppProcessInfo.IMPORTANCE_VISIBLE) return true;
        int st = e.getStatus(); return r == ApplicationExitInfo.REASON_EXIT_SELF && (st == 128 + 11 || st == 128 + 7 || st == 128 + 4 || st == 128 + 5 || st == 128 + 6);
    }
    private static String exitLine(ApplicationExitInfo e) {
        return String.format(Locale.ROOT, "%s  pid %d  %s  status %d  importance %d  pss %d MB  rss %d MB  %s",
            new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.ROOT).format(new java.util.Date(e.getTimestamp())), e.getPid(), reason(e.getReason()), e.getStatus(),
            e.getImportance(), e.getPss() >> 10, e.getRss() >> 10, e.getDescription() == null ? "" : e.getDescription().replace('\n', ' '));
    }
    private static String lastExitLine(Context c) { List<ApplicationExitInfo> l = exitList(c, 1); return l.isEmpty() ? "" : exitLine(l.get(0)); }
    private static void exits(Context c, ZipOutputStream z) throws IOException {
        List<ApplicationExitInfo> l = exitList(c, 16); StringBuilder sb = new StringBuilder("Newest first. Reasons: ApplicationExitInfo.\n\n"); int traces = 0;
        for (ApplicationExitInfo e : l) {
            sb.append(exitLine(e)).append('\n');
            if (traces >= 4) continue;
            int r = e.getReason(); if (r != ApplicationExitInfo.REASON_CRASH_NATIVE && r != ApplicationExitInfo.REASON_ANR) continue;
            try (InputStream in = e.getTraceInputStream()) {
                if (in == null) continue;
                String name = "exits/" + new java.text.SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new java.util.Date(e.getTimestamp())) + "-pid" + e.getPid()
                            + (r == ApplicationExitInfo.REASON_ANR ? ".anr.txt" : ".tombstone.pb");
                z.putNextEntry(new ZipEntry(name)); copy(in, z, 32L << 20); z.closeEntry(); traces++;
                sb.append("    → ").append(name).append('\n');
            } catch (IOException | RuntimeException ex) { sb.append("    (trace not readable: ").append(ex).append(")\n"); }
        }
        if (l.isEmpty()) sb.append("(none recorded)\n");
        text(z, "exits/summary.txt", sb.toString());
    }

    // ---------- the shaders a crash was building ----------
    // nvn_vk.c pso_make logs "pipeline N: vertex <key> fragment <key>" before each vkCreateGraphicsPipelines, and a failed one as "[RED] graphics pipeline (...)
    //   failed"; a driver that crashes compiling a pipeline ends the log right after its line. The last few of each log, with the chain of the pack they came from
    private static String shaders(Context c, File dir) {
        StringBuilder sb = new StringBuilder("Shader chain (ready): ").append(first(new File(dir, "ready"))).append("\nThe app expects: ").append(asset(c, "shader_chain")).append("\n");
        for (String n : new String[] { "log.prev.txt", "log.txt" }) {
            File f = new File(dir, n); sb.append("\n== ").append(n).append(f.isFile() ? "" : " (none)").append('\n'); if (!f.isFile()) continue;
            java.util.ArrayDeque<String> last = new java.util.ArrayDeque<>(), crumbs = new java.util.ArrayDeque<>(); List<String> failed = new ArrayList<>(); String tail = "";
            try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8))) {
                String l; while ((l = r.readLine()) != null) {
                    if (l.contains("[gpu-crumbs]")) { crumbs.addLast(l); if (crumbs.size() > 400) crumbs.removeFirst(); continue; }   // (vk_crumbs.h: the GPU work when it failed)
                    if (l.contains("[nvn-vk] pipeline ") && l.contains(" vertex ")) { last.addLast(l); if (last.size() > 8) last.removeFirst(); }
                    else if (l.contains("graphics pipeline (") && l.contains("failed")) { failed.add(l); if (failed.size() > 8) failed.remove(0); }
                    else if (l.contains("compute pipeline") && l.contains("failed")) { failed.add(l); if (failed.size() > 8) failed.remove(0); }
                    if (l.contains("CRASH") || l.contains("FATAL")) tail = l;
                }
            } catch (IOException e) { sb.append("(").append(e).append(")\n"); continue; }
            sb.append("last pipelines built:\n"); for (String s : last) sb.append("  ").append(s.trim()).append('\n');
            if (!failed.isEmpty()) { sb.append("pipelines that failed:\n"); for (String s : failed) sb.append("  ").append(s.trim()).append('\n'); }
            if (!tail.isEmpty()) sb.append("last crash / fatal line: ").append(tail.trim()).append('\n');
            if (!crumbs.isEmpty()) { sb.append("GPU breadcrumbs (the work on the GPU when it failed):\n"); for (String s : crumbs) sb.append("  ").append(s.trim()).append('\n'); }
        }
        return sb.toString();
    }

    // ---------- memory over the run ----------
    // android_main.c log_mirror writes "[mem]" (resident, swapped, the phone's free memory, heat) and "[mem-by]" (anonymous / files / shared / the engine's GPU
    //   blocks) every 30 s. Gathered in order, with the growth from the first to the last line: a run Android ended for memory shows what grew
    private static String memory(File dir) {
        StringBuilder sb = new StringBuilder("Every 30 s of each run (android_main.c log_mirror). [mem-by]: anonymous = the game's and the driver's heaps, shared = buffers shared with the GPU driver.\n");
        for (String n : new String[] { "log.prev.txt", "log.txt" }) {
            File f = new File(dir, n); if (!f.isFile()) continue;
            List<String> ls = new ArrayList<>(); long first = -1, last = -1, peak = -1;
            try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8))) {
                String l; while ((l = r.readLine()) != null) {
                    if (!l.startsWith("[mem]") && !l.startsWith("[mem-by]")) continue;
                    ls.add(l);
                    if (l.startsWith("[mem] app ")) { try { long v = Long.parseLong(l.substring(10, l.indexOf(' ', 10))); if (first < 0) first = v; last = v; peak = Math.max(peak, v); } catch (RuntimeException e) { } }
                }
            } catch (IOException e) { continue; }
            sb.append("\n== ").append(n).append(": ").append(ls.size()).append(" lines");
            if (first >= 0) sb.append(String.format(Locale.ROOT, "; resident %d MB at the first line, %d MB at the last (%+d MB), peak %d MB", first, last, last - first, peak));
            sb.append('\n');
            int from = Math.max(0, ls.size() - 160); if (from > 0) { sb.append(ls.get(0)).append('\n').append(ls.get(1)).append("\n...\n"); }
            for (int i = from; i < ls.size(); i++) sb.append(ls.get(i)).append('\n');
        }
        return sb.toString();
    }
    // the lines of both logs that hold a tag, in order (at most 400 a log)
    private static String grep(File dir, String tag, String head) {
        StringBuilder sb = new StringBuilder(head);
        for (String n : new String[] { "log.prev.txt", "log.txt" }) {
            File f = new File(dir, n); if (!f.isFile()) continue; int k = 0; sb.append("\n== ").append(n).append('\n');
            try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8))) {
                String l; while ((l = r.readLine()) != null && k < 400) if (l.contains(tag)) { sb.append(l).append('\n'); k++; }
            } catch (IOException e) { }
        }
        return sb.toString();
    }
    // ---------- the engine's warnings, counted ----------
    // Picture problems leave no crash, but the engine names what it could not do: a shader missing from the pack, a pipeline that failed, draws skipped, a format
    //   the driver lacks ("[RED]" lines). Each kind once, with how often it came (numbers in it ignored for the count), the most frequent first
    private static String warnings(File dir) {
        StringBuilder sb = new StringBuilder("The engine's [RED] / FATAL lines, each kind once with its count (numbers ignored when counting). A picture problem's first clues.\n");
        for (String n : new String[] { "log.prev.txt", "log.txt" }) {
            File f = new File(dir, n); if (!f.isFile()) continue;
            java.util.LinkedHashMap<String, int[]> count = new java.util.LinkedHashMap<>(); java.util.HashMap<String, String> example = new java.util.HashMap<>();
            try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8))) {
                String l; while ((l = r.readLine()) != null) {
                    if (!l.contains("[RED]") && !l.contains("FATAL")) continue;
                    String k = l.replaceAll("0x[0-9a-fA-F]+|\\b[0-9a-f]{12,}\\b|\\d+", "#");
                    int[] c = count.get(k); if (c == null) { if (count.size() >= 2000) continue; count.put(k, c = new int[1]); example.put(k, l.trim()); } c[0]++;
                }
            } catch (IOException e) { continue; }
            List<java.util.Map.Entry<String, int[]>> es = new ArrayList<>(count.entrySet());
            java.util.Collections.sort(es, (a, b) -> b.getValue()[0] - a.getValue()[0]);
            sb.append("\n== ").append(n).append(": ").append(es.size()).append(" kinds\n");
            for (int i = 0; i < es.size() && i < 120; i++) sb.append(String.format(Locale.ROOT, "%6dx  %s\n", es.get(i).getValue()[0], example.get(es.get(i).getKey())));
        }
        return sb.toString();
    }

    // ---------- logcat ----------
    // An app reads only its own lines (Android 4.1+), from every buffer it may see; the last 6,000, with thread and time. A few seconds at most
    private static String logcat() {
        try {
            Process p = new ProcessBuilder("logcat", "-d", "-v", "threadtime", "-b", "main,system,crash", "-t", "6000").redirectErrorStream(true).start();
            java.io.ByteArrayOutputStream o = new java.io.ByteArrayOutputStream();
            try (InputStream in = p.getInputStream()) { copy(in, o, 8L << 20); }
            p.waitFor(); return o.toString("UTF-8");
        } catch (Exception e) { return "(logcat not readable: " + e + ")\n"; }
    }

    // ---------- the GitHub issue ----------
    // The issue form (.github/ISSUE_TEMPLATE/bug_report.yml) with what the app knows filled in: a field's id as a query parameter fills it. The player writes what
    //   happened and attaches the ZIP
    static String issueUrl(Context c, File zip) {
        String ver = "?"; try { ver = c.getPackageManager().getPackageInfo(c.getPackageName(), 0).versionName; } catch (Exception e) { }
        String phone = (Build.MANUFACTURER + " " + Build.MODEL).trim(), gpu = first(new File(GameSettings.dir(c), "gpu.txt"));
        String soc = Build.SOC_MODEL == null ? "" : Build.SOC_MODEL;
        List<ApplicationExitInfo> l = exitList(c, 1); boolean crash = !l.isEmpty() && crashed(l.get(0));
        String drv = DriverPackages.current(c).toLowerCase(Locale.ROOT), d;
        if (drv.isEmpty()) d = "The phone's own driver";
        else if (drv.contains("turnip") || DriverPackages.isTurnip(DriverPackages.current(c))) d = "Turnip (custom Adreno driver)";
        else if (drv.contains("panvk") || drv.contains("malisx") || drv.contains("panfrost")) d = "PanVK / malisx2 (custom Mali driver)";
        else d = "Other custom driver";
        String repo = GameSettings.app(c)[1];   // (the issue form exists in Galar-Native-Android only; elsewhere the plain body below fills the issue)
        StringBuilder u = new StringBuilder("https://").append(repo).append(repo.contains("Galar-Native") ? "/issues/new?template=bug_report.yml" : "/issues/new?labels=bug");
        int why = l.isEmpty() ? -1 : l.get(0).getReason();
        String kind = !crash ? "Problem" : why == ApplicationExitInfo.REASON_LOW_MEMORY || why == ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE ? "Closed by Android (memory)"
                    : why == ApplicationExitInfo.REASON_ANR ? "Not responding" : "Crash";
        q(u, "title", "[" + ver + "] " + kind + " on " + phone);
        q(u, "phone", phone); q(u, "gpu", (soc.isEmpty() ? "" : soc + " / ") + gpu); q(u, "version", ver); q(u, "game", GameSettings.game(c)[1]); q(u, "driver", d);
        // GitHub cannot take a file through a link: the player has to attach the ZIP by hand, and many do not (Galar #340). So the crash's essentials go in the
        //   text itself (crashSummary: Android's crash record and the engine's own crash lines), cut until the whole link stays under GitHub's ~8 KB
        String name = zip != null ? zip.getName() : "debug-report.zip";
        String head = "⚠️ ATTACH Download/" + name + " HERE (tap \"Paste, drop, or click to add files\" under this box). GitHub cannot add it for you.\n\n"
                    + (crash ? "Last exit: " + lastExitLine(c) + "\n" : "");
        String sum = crash ? crashSummary(c) : "", logs = head;
        for (int keep = sum.length(); ; keep = keep * 3 / 4) {
            String s = keep <= 16 ? "" : sum.substring(0, Math.min(keep, sum.length()));
            logs = s.isEmpty() ? head : head + "\nCrash summary (made by the app):\n```\n" + s + (keep < sum.length() ? "\n(cut: the rest is in the ZIP)" : "") + "\n```\n";
            if (s.isEmpty() || u.length() + enc(logs).length() < 7600) break;
        }
        boolean form = repo.contains("Galar-Native");
        if (form) { q(u, "logs", logs); return u.toString(); }
        // a repository without the issue form (Arceus-Native-Android) takes the plain body instead (with the form, GitHub ignores it: left out, it only made the link longer)
        q(u, "body", "**What happens**\n\n(say what you did and what went wrong)\n\n**Phone:** " + phone + "\n**GPU:** " + (soc.isEmpty() ? "" : soc + " / ") + gpu + "\n**Version:** " + ver
                     + "\n**Game:** " + GameSettings.game(c)[1] + "\n**Driver:** " + d + "\n\n" + logs + "\n");
        return u.toString();
    }
    private static String enc(String v) { try { return java.net.URLEncoder.encode(v, "UTF-8"); } catch (java.io.UnsupportedEncodingException e) { return v; } }

    // ---------- the crash in a few lines ----------
    // For the issue text and crash-summary.txt: the newest native crash's record from Android (signal, abort message, the crashed thread and its top frames,
    //   read as dm/android/tools/read_debug_report.py does) and the engine's own crash lines from the newest log that has them ("[dm] CRASH" and the
    //   "[dm]" lines after it, the last FATAL line)
    static String crashSummary(Context c) {
        StringBuilder sb = new StringBuilder();
        for (ApplicationExitInfo e : exitList(c, 4)) {
            if (e.getReason() != ApplicationExitInfo.REASON_CRASH_NATIVE) continue;
            try (InputStream in = e.getTraceInputStream()) {
                if (in != null) { java.io.ByteArrayOutputStream o = new java.io.ByteArrayOutputStream(); copy(in, o, 32L << 20); sb.append(tombstone(o.toByteArray())); }
            } catch (Exception ex) { sb.append("(crash record not readable: ").append(ex.getClass().getSimpleName()).append(")\n"); }
            break;
        }
        File dir = GameSettings.dir(c);
        for (String n : new String[] { "log.txt", "log.prev.txt" }) {
            File f = new File(dir, n); if (!f.isFile()) continue;
            List<String> got = new ArrayList<>(); String fatal = "";
            try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8))) {
                String l; int after = 0;
                while ((l = r.readLine()) != null) {
                    if (l.contains("[dm] CRASH")) { got.clear(); after = 8; }
                    if (after > 0 && l.contains("[dm]")) { got.add(cut(l.trim(), 400)); after--; }
                    if (l.contains("FATAL")) fatal = cut(l.trim(), 300);
                }
            } catch (IOException ex) { continue; }
            if (got.isEmpty() && fatal.isEmpty()) continue;
            sb.append("engine log (").append(n).append("):\n"); for (String l : got) sb.append(l).append('\n');
            if (!fatal.isEmpty() && !got.contains(fatal)) sb.append(fatal).append('\n');
            break;
        }
        return sb.toString().trim();
    }
    private static String cut(String s, int n) { return s.length() > n ? s.substring(0, n) : s; }
    // tombstone.proto by hand: Tombstone tid 6, signal_info 10 (number 1, name 2, code_name 4, has_fault_address 8, fault_address 9), abort_message 14,
    //   causes 15 (human_readable 1), threads 16 (map entry: key 1, Thread 2: name 2, current_backtrace 4: rel_pc 1, function_name 4, function_offset 5, file_name 6)
    static String tombstone(byte[] b) {
        if (b.length > 3 && b[0] == '*' && b[1] == '*' && b[2] == '*') return new String(b, 0, Math.min(b.length, 3000), StandardCharsets.UTF_8) + "\n";   // (the text form, on some phones)
        java.util.Map<Integer, List<Object>> t = pb(b); StringBuilder sb = new StringBuilder();
        if (t.containsKey(10)) {
            java.util.Map<Integer, List<Object>> g = pbm(t, 10);
            sb.append("signal ").append(pv(g, 1)).append(' ').append(ps(g, 2)).append(' ').append(ps(g, 4));
            if (pv(g, 8) != 0) sb.append(" fault addr 0x").append(Long.toHexString(pv(g, 9)));
            sb.append('\n');
        }
        if (t.containsKey(14)) sb.append("abort: ").append(cut(ps(t, 14), 400)).append('\n');
        if (t.containsKey(15)) for (Object o : t.get(15)) if (o instanceof byte[]) sb.append("cause: ").append(cut(ps(pb((byte[]) o), 1), 300)).append('\n');
        long tid = pv(t, 6);
        if (t.containsKey(16)) for (Object o : t.get(16)) {
            if (!(o instanceof byte[])) continue;
            java.util.Map<Integer, List<Object>> m = pb((byte[]) o); if (pv(m, 1) != tid) continue;
            java.util.Map<Integer, List<Object>> th = pbm(m, 2);
            sb.append("thread ").append(tid).append(" \"").append(ps(th, 2)).append("\"\n");
            List<Object> fr = th.containsKey(4) ? th.get(4) : new ArrayList<>();
            for (int i = 0; i < fr.size() && i < 16; i++) {
                if (!(fr.get(i) instanceof byte[])) continue;
                java.util.Map<Integer, List<Object>> f = pb((byte[]) fr.get(i));
                String file = ps(f, 6), fn = ps(f, 4); int sl = file.lastIndexOf('/'); if (sl >= 0) file = file.substring(sl + 1);
                sb.append(String.format(Locale.ROOT, "#%02d %s+0x%x", i, file.isEmpty() ? "?" : file, pv(f, 1)));
                if (!fn.isEmpty()) sb.append(" (").append(cut(fn, 80)).append('+').append(pv(f, 5)).append(')');
                sb.append('\n');
            }
            break;
        }
        return sb.toString();
    }
    // one protobuf message: field number -> its values (Long, or byte[] for length-delimited); stops at the first thing that does not parse
    static java.util.Map<Integer, List<Object>> pb(byte[] b) {
        java.util.Map<Integer, List<Object>> d = new java.util.HashMap<>(); int i = 0;
        try {
            while (i < b.length) {
                long[] k = vi(b, i); i = (int) k[1]; int f = (int) (k[0] >>> 3), w = (int) (k[0] & 7); Object v;
                if (w == 0) { long[] x = vi(b, i); v = x[0]; i = (int) x[1]; }
                else if (w == 1 || w == 5) { int n = w == 1 ? 8 : 4; long x = 0; for (int j = n - 1; j >= 0; j--) x = x << 8 | (b[i + j] & 0xff); v = x; i += n; }
                else if (w == 2) { long[] n = vi(b, i); i = (int) n[1]; if (n[0] < 0 || n[0] > b.length - i) break; v = java.util.Arrays.copyOfRange(b, i, i + (int) n[0]); i += (int) n[0]; }
                else break;
                List<Object> l = d.get(f); if (l == null) d.put(f, l = new ArrayList<>()); l.add(v);
            }
        } catch (RuntimeException e) { }
        return d;
    }
    private static long[] vi(byte[] b, int i) { long v = 0; int s = 0; while (true) { int x = b[i++] & 0xff; v |= (long) (x & 0x7f) << s; if (x < 0x80) break; s += 7; if (s > 63) throw new IllegalArgumentException("varint"); } return new long[] { v, i }; }
    private static long pv(java.util.Map<Integer, List<Object>> d, int f) { List<Object> l = d.get(f); return l != null && l.get(0) instanceof Long ? (Long) l.get(0) : 0; }
    private static String ps(java.util.Map<Integer, List<Object>> d, int f) { List<Object> l = d.get(f); return l != null && l.get(0) instanceof byte[] ? new String((byte[]) l.get(0), StandardCharsets.UTF_8) : ""; }
    private static java.util.Map<Integer, List<Object>> pbm(java.util.Map<Integer, List<Object>> d, int f) { List<Object> l = d.get(f); return pb(l != null && l.get(0) instanceof byte[] ? (byte[]) l.get(0) : new byte[0]); }

    private static void q(StringBuilder u, String k, String v) {
        try { u.append('&').append(k).append('=').append(java.net.URLEncoder.encode(v, "UTF-8").replace("+", "%20")); } catch (java.io.UnsupportedEncodingException e) { }
    }

    // ---------- from the menu / after a crash ----------
    // Makes the report in the background, then says where it is and offers the pre-filled issue
    static void run(Activity a) {
        if (busy) return; busy = true;
        android.widget.Toast.makeText(a, T.t(a, "Making the debug report…"), android.widget.Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            File z = null; String err = null;
            try { z = make(a); } catch (Exception e) { err = String.valueOf(e); } finally { busy = false; }
            final File zf = z; final String er = err;
            a.runOnUiThread(() -> {
                if (a.isFinishing() || a.isDestroyed()) return;
                if (zf == null) { android.widget.Toast.makeText(a, T.t(a, "The debug report could not be made: %s", er), android.widget.Toast.LENGTH_LONG).show(); return; }
                String where = zf.getParent() != null && zf.getParent().equals(DL) ? "Download/" + zf.getName() : zf.getPath();
                new android.app.AlertDialog.Builder(a).setTitle(T.t(a, "Debug report ready"))
                    .setMessage(T.t(a, "Saved as %s. Open a GitHub issue (phone, GPU, version, driver and a crash summary are filled in), say what happened, and attach this file in the Logs box: GitHub cannot add it by itself.", where))
                    .setPositiveButton(T.t(a, "Open a GitHub issue"), (dl, w) -> { try { a.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(issueUrl(a, zf)))); } catch (Exception e) { } })
                    .setNegativeButton(T.t(a, "Close"), null).show();
            });
        }, "debug report").start();
    }
    // The newest run that ended badly and was not offered yet (its time in <files>/debug_offered): offered once with a dialog. Returns true when the dialog shows;
    //   then: play (the game as usual) runs after either button that does not report
    static boolean offerAfterCrash(Activity a, Runnable play) {
        List<ApplicationExitInfo> l = exitList(a, 1); if (l.isEmpty() || !crashed(l.get(0))) return false;
        long t = l.get(0).getTimestamp(); File mark = new File(GameSettings.root(a), "debug_offered");
        long seen = 0; try { seen = Long.parseLong(first(mark)); } catch (NumberFormatException e) { }
        if (t <= seen) return false;
        try (java.io.FileWriter w = new java.io.FileWriter(mark)) { w.write(t + "\n"); } catch (IOException e) { }
        new android.app.AlertDialog.Builder(a).setTitle(T.t(a, "The game closed unexpectedly"))
            .setMessage(T.t(a, "Last time, the game stopped (%s). A debug report collects the logs and Android's crash record into one file for a GitHub issue. No game files or saves are included.", T.t(a, reason(l.get(0).getReason()))))
            .setPositiveButton(T.t(a, "Make a debug report"), (d, w) -> run(a))
            .setNeutralButton(T.t(a, "Play"), (d, w) -> { if (play != null) play.run(); })
            .setNegativeButton(T.t(a, "Close"), null).show();
        return true;
    }

    // ---------- small helpers ----------
    private static void text(ZipOutputStream z, String name, String s) throws IOException { z.putNextEntry(new ZipEntry(name)); z.write(s.getBytes(StandardCharsets.UTF_8)); z.closeEntry(); }
    private static void file(ZipOutputStream z, String name, File f) throws IOException { file(z, name, f, CAP); }
    private static void file(ZipOutputStream z, String name, File f, long cap) throws IOException {
        if (!f.isFile()) return;
        try (java.io.RandomAccessFile r = new java.io.RandomAccessFile(f, "r")) {
            long n = r.length(); if (n > cap) r.seek(n - cap);   // (a long log: its end, where a crash is)
            z.putNextEntry(new ZipEntry(name));
            byte[] b = new byte[1 << 16]; int k; long left = Math.min(n, cap);
            while (left > 0 && (k = r.read(b, 0, (int) Math.min(b.length, left))) > 0) { z.write(b, 0, k); left -= k; }
            z.closeEntry();
        } catch (IOException e) { }
    }
    private static void copy(InputStream in, OutputStream out, long max) throws IOException { byte[] b = new byte[1 << 16]; int k; long n = 0; while (n < max && (k = in.read(b)) > 0) { out.write(b, 0, k); n += k; } }
    private static String first(File f) { try (BufferedReader r = new BufferedReader(new FileReader(f))) { String l = r.readLine(); return l == null ? "" : l.trim(); } catch (IOException e) { return ""; } }
    private static String read(File f, int max) { try (InputStream in = new FileInputStream(f)) { byte[] b = new byte[max]; int n = in.read(b); return n > 0 ? new String(b, 0, n, StandardCharsets.UTF_8) : ""; } catch (IOException e) { return ""; } }
    private static String asset(Context c, String n) { try (BufferedReader r = new BufferedReader(new InputStreamReader(c.getAssets().open(n)))) { String l = r.readLine(); return l == null ? "" : l.trim(); } catch (IOException e) { return ""; } }
    // the pack holds at least 9/10 of the setup's shaders (the rule of Galar's SetupActivity.packComplete, kept here so this file is the same in every port)
    private static boolean packComplete(File d) {
        int have = packCount(new File(d, "shaders.pak")), want = 0; if (have < 0) return true;
        try (BufferedReader r = new BufferedReader(new FileReader(new File(d, "shader_map.tsv")))) { String l; while ((l = r.readLine()) != null) if (!l.isEmpty() && l.charAt(0) != '#') want++; } catch (IOException e) { return have >= 10000; }
        return want == 0 || have >= want * 9L / 10;
    }
    private static int packCount(File pak) {
        try (java.io.RandomAccessFile f = new java.io.RandomAccessFile(pak, "r")) { byte[] h = new byte[12]; f.readFully(h); if (h[0] != 'N' || h[1] != 'X' || h[2] != 'S' || h[3] != 'P') return -1;
            return (h[8] & 0xff) | (h[9] & 0xff) << 8 | (h[10] & 0xff) << 16 | (h[11] & 0xff) << 24; } catch (IOException e) { return -1; }
    }
    private static String sha256(File f) {
        try (InputStream in = new FileInputStream(f)) { MessageDigest md = MessageDigest.getInstance("SHA-256"); byte[] b = new byte[1 << 16]; int k; while ((k = in.read(b)) > 0) md.update(b, 0, k);
            StringBuilder s = new StringBuilder(); for (byte x : md.digest()) s.append(String.format("%02x", x)); return s.toString(); } catch (Exception e) { return "(" + e.getClass().getSimpleName() + ")"; }
    }
    private static String prop(String k) {
        try { Process p = new ProcessBuilder("getprop", k).start(); String v; try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()))) { v = r.readLine(); } p.waitFor(); return v == null ? "" : v.trim(); }
        catch (Exception e) { return ""; }
    }
}
