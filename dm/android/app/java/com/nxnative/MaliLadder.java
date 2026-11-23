// The app's side of the Mali white-screen check (the engine's nvn_vk.c mali_white_vk.inc; its rules dm/gfx/nvn/mali_white.h): on Arm's own Mali driver the engine
// checks Sword / Shield's lit scene while playing and moves from one fix to the next, start by start, keeping its state in mali_ladder.txt in the app's files folder
// (android_main.c DM_MALI_LADDER_FILE). Here: the one-line note over the game when a fix did not take (note: the engine's front_note, android_main.c), and, once
// both fixes failed, the PanVK offer at the app's next start (offer: the file's panvk=offer, shown once, then panvk=shown). The GPU is the one the engine wrote into
// the file (gpu=; the game's gpu.txt for an older file); its PanVK build is DriverPackages.sourceFor's, the same as the menu's, and a GPU no PanVK build is known for
// gets the setup screen's advice instead (report it with the log). The file handling is in static methods free of Android (fields / due / shown / gpuOr), tested
// on a desktop JVM against the engine's own files: dm/android/tools/tests/MaliLadderTest.java
package com.nxnative;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.widget.Toast;
import java.io.*;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

final class MaliLadder {
    private MaliLadder() { }
    static File file(Context c) { return new File(GameSettings.root(c), "mali_ladder.txt"); }
    // the engine's note, by its key, translated, over the game (called from native code: android_main.c note_main). "mali-env:<keys>" names the env.txt switches
    //   that hold the fix back (DM_VK_B10_AS_RGBA16, DM_VK_B10_NOCOMPRESS): the note gives their names in the menu
    static void note(final Activity a, final String key) {
        a.runOnUiThread(() -> {
            String[] kn = noteKey(key); String k = kn[0], t = k;   // (if / else, not a ?: chain: lang_check.py reads a quoted value after ? or : on a T.t line as a text)
            if ("mali-try-b".equals(k)) t = T.t(a, "Mali: the white-screen fix did not take; trying the second fix at the next start");
            else if ("mali-retry".equals(k)) t = T.t(a, "Mali: the white-screen fix did not take; the next start tries the other fix again");
            else if ("mali-panvk".equals(k)) t = T.t(a, "Mali: neither white-screen fix took; the next start offers the PanVK driver");
            else if ("mali-env".equals(k)) {
                StringBuilder names = new StringBuilder();
                for (String e : kn[1].split(",")) {
                    String n = e.trim(), l = switchLabel(n);
                    if (l != null) n = T.t(a, l);
                    if (n.isEmpty()) continue;
                    if (names.length() > 0) names.append(", ");
                    names.append(n); }
                if (names.length() == 0) t = T.t(a, "Mali: a test switch in the settings overrides the white-screen fix");
                else t = T.t(a, "Mali: a test switch in the settings (%s) overrides the white-screen fix", names.toString());
            }
            Toast.makeText(a, t, Toast.LENGTH_LONG).show();
        });
    }
    // a note's key and what follows its first ':' ("" when nothing does)
    static String[] noteKey(String key) {
        if (key == null) return new String[] { "", "" };
        int c = key.indexOf(':');
        if (c < 0) return new String[] { key, "" };
        return new String[] { key.substring(0, c), key.substring(c + 1) };
    }
    // the menu's name (its English text, the translation's key: GameMenu's White screen tests) of a switch the engine named; null for one the menu does not show
    static String switchLabel(String env) {
        if ("DM_VK_B10_AS_RGBA16".equals(env)) return "Bright scenes in a wider format";
        if ("DM_VK_B10_NOCOMPRESS".equals(env)) return "Bright scenes without compression";
        return null;
    }

    // ---- the state file (no Android here) ----
    // its key=value lines (the engine's mw_state_format), # comments and lines without = left out; a value keeps everything after the first =
    static Map<String, String> fields(List<String> lines) {
        Map<String, String> m = new LinkedHashMap<>();
        for (String l : lines) { if (l.startsWith("#")) continue; int e = l.indexOf('='); if (e > 0) m.put(l.substring(0, e).trim(), l.substring(e + 1).trim()); }
        return m;
    }
    // the offer is due: the engine asked for it (panvk=offer) and its last start ran this app version (it writes app= at each start; an app update keeps the ladder,
    //   so an offer pending from before the update is due once the engine has started on it), and the phone's own driver is still the one
    //   in use (no driver package chosen meanwhile)
    static boolean due(Map<String, String> f, long versionCode, String driverPackage) {
        String app = f.get("app"), pv = f.get("panvk");
        return "offer".equals(pv) && app != null && app.equals(String.valueOf(versionCode)) && (driverPackage == null || driverPackage.isEmpty());
    }
    // the lines with the offer marked shown (the others as they were)
    static List<String> shown(List<String> lines) {
        List<String> out = new ArrayList<>(); for (String l : lines) out.add(l.startsWith("panvk=") ? "panvk=shown" : l); return out;
    }
    // the GPU's name as the engine wrote it (gpu=), else the fallback (the game's gpu.txt: a file from before gpu= was written)
    static String gpuOr(Map<String, String> f, String fallback) { String g = f.get("gpu"); return g == null || g.isEmpty() ? (fallback == null ? "" : fallback.trim()) : g; }
    static List<String> read(File f) throws IOException {
        List<String> lines = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(f), "UTF-8"))) { String l; while ((l = r.readLine()) != null) lines.add(l); }
        return lines;
    }
    static void write(File f, List<String> lines) throws IOException {
        File t = new File(f.getPath() + ".tmp");
        try (Writer w = new OutputStreamWriter(new FileOutputStream(t), "UTF-8")) { for (String l : lines) w.write(l + "\n"); }
        try { java.nio.file.Files.move(t.toPath(), f.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING, java.nio.file.StandardCopyOption.ATOMIC_MOVE); }   // (whole or not at all, as the engine writes it)
        catch (IOException e) { t.delete(); throw e; }
    }

    // At the app's start (SetupActivity): the PanVK offer, once, when the engine asked for it on this app version and the phone's own driver is still the one in use.
    //   true: the dialog is up (its buttons open the PanVK download page for this GPU when one is known, pick a driver package, or start the game)
    static boolean offer(final Activity a, final Runnable start, final Runnable pickDriver) {
        File f = file(a); if (!f.exists()) return false;
        List<String> lines; try { lines = read(f); } catch (IOException e) { return false; }
        Map<String, String> fl = fields(lines);
        long ver; try { ver = a.getPackageManager().getPackageInfo(a.getPackageName(), 0).getLongVersionCode(); } catch (Exception e) { ver = -1; }
        if (!due(fl, ver, DriverPackages.current(a))) return false;
        try { write(f, shown(lines)); } catch (IOException e) { }   // (once)
        String txt = ""; try (BufferedReader r = new BufferedReader(new FileReader(new File(GameSettings.dir(a), "gpu.txt")))) { String l = r.readLine(); if (l != null) txt = l; } catch (IOException e) { }
        String gpu = gpuOr(fl, txt);
        final String[] src = DriverPackages.source(a, gpu.toLowerCase(Locale.ROOT));
        // a GPU no PanVK build is known for (Mali-G31 / G51 / G71, G310 / G510, the Mali-G1 family, an unknown name): the setup screen's advice, not a dead end
        String msg = T.t(a, "On this phone's own Mali driver (%s), %s drew its lit scenes white, and both of the app's fixes were tried at earlier starts without success.",
                         gpu.isEmpty() ? "Mali" : gpu, GameSettings.game(a)[1]) + " "
                   + (src != null ? T.t(a, "The PanVK driver draws the game on many Mali phones: download its .zip and choose it as the graphics driver.") : SetupActivity.driverAdvice(a, gpu));
        android.app.AlertDialog.Builder b = new android.app.AlertDialog.Builder(a).setTitle(T.t(a, "White screen on this phone's own driver")).setMessage(msg)
            .setNeutralButton(T.t(a, "Choose a driver"), (d, w) -> pickDriver.run())
            .setNegativeButton(T.t(a, "Start anyway"), (d, w) -> start.run());
        if (src != null) b.setPositiveButton(src[0], (d, w) -> a.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(src[1]))));
        b.setCancelable(false).show();
        return true;
    }
}
