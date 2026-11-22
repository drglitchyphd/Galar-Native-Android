// The app's side of the Mali white-screen check (the engine's nvn_vk.c mali_white_vk.inc; its rules dm/gfx/nvn/mali_white.h): on Arm's own Mali driver the engine
// checks Sword / Shield's lit scene while playing and moves from one fix to the next, start by start, keeping its state in mali_ladder.txt in the app's files folder
// (android_main.c DM_MALI_LADDER_FILE). Here: the one-line note over the game when a fix did not take (note: the engine's front_note, android_main.c), and, once
// both fixes were white, the PanVK offer at the app's next start (offer: the file's panvk=offer, shown once, then panvk=shown; the per-GPU suggestion is
// DriverPackages.source's, the same as the menu's)
package com.nxnative;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.widget.Toast;
import java.io.*;
import java.util.ArrayList;
import java.util.List;

final class MaliLadder {
    private MaliLadder() { }
    static File file(Context c) { return new File(GameSettings.root(c), "mali_ladder.txt"); }
    // the engine's note, by its key, translated, over the game (called from native code: android_main.c note_main)
    static void note(final Activity a, final String key) {
        a.runOnUiThread(() -> {
            String t = key;
            if ("mali-try-b".equals(key)) t = T.t(a, "Mali: the white-screen fix did not take; trying the second fix at the next start");
            else if ("mali-panvk".equals(key)) t = T.t(a, "Mali: neither white-screen fix took; the next start offers the PanVK driver");
            Toast.makeText(a, t, Toast.LENGTH_LONG).show();
        });
    }
    // At the app's start (SetupActivity): the PanVK offer, once, when the engine asked for it on this app version and the phone's own driver is still the one in use.
    //   true: the dialog is up (its buttons open the PanVK download page for this GPU, pick a driver package, or start the game)
    static boolean offer(final Activity a, final Runnable start, final Runnable pickDriver) {
        File f = file(a); if (!f.exists()) return false;
        List<String> lines = new ArrayList<>(); String app = "", panvk = "";
        try (BufferedReader r = new BufferedReader(new FileReader(f))) { String l;
            while ((l = r.readLine()) != null) { lines.add(l); if (l.startsWith("app=")) app = l.substring(4).trim(); if (l.startsWith("panvk=")) panvk = l.substring(6).trim(); } }
        catch (IOException e) { return false; }
        if (!"offer".equals(panvk)) return false;
        long ver; try { ver = a.getPackageManager().getPackageInfo(a.getPackageName(), 0).getLongVersionCode(); } catch (Exception e) { ver = -1; }
        if (!app.equals(String.valueOf(ver))) return false;   // (made by another app version: the engine starts the check over at its next start)
        if (!DriverPackages.current(a).isEmpty()) return false;   // (a driver package is the driver already)
        try (FileWriter w = new FileWriter(f)) { for (String l : lines) w.write((l.startsWith("panvk=") ? "panvk=shown" : l) + "\n"); } catch (IOException e) { }   // (once)
        String gpu = ""; try (BufferedReader r = new BufferedReader(new FileReader(new File(GameSettings.dir(a), "gpu.txt")))) { String l = r.readLine(); if (l != null) gpu = l.trim(); } catch (IOException e) { }
        final String[] src = DriverPackages.source(a);
        android.app.AlertDialog.Builder b = new android.app.AlertDialog.Builder(a).setTitle(T.t(a, "White screen on this phone's own driver"))
            .setMessage(T.t(a, "On this phone's own Mali driver (%s), %s drew its lit scenes white, and both of the app's fixes were tried at earlier starts without success. The PanVK driver draws the game on many Mali phones: download its .zip and choose it as the graphics driver.",
                            gpu.isEmpty() ? "Mali" : gpu, GameSettings.game(a)[1]))
            .setNeutralButton(T.t(a, "Choose a driver"), (d, w) -> pickDriver.run())
            .setNegativeButton(T.t(a, "Start anyway"), (d, w) -> start.run());
        if (src != null) b.setPositiveButton(src[0], (d, w) -> a.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(src[1]))));
        b.setCancelable(false).show();
        return true;
    }
}
