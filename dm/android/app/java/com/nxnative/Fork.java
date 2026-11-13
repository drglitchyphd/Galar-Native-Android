// FORK: where this build keeps its files.
// A fork build (scripts/build_fork.sh) ships assets/data_dir with a folder name. All of the app's per-game files (the game set up from the dump, saves,
// settings, mods, shader caches, logs) then live in that folder at the top of the phone's shared storage (/storage/emulated/0/<name>) instead of
// Android/data/<package>/files. The folder outlives an uninstall or a package rename, and the phone's Files app can reach it. Keys and the unpacked
// tools stay in the app's private storage.
// Without assets/data_dir (a stock build) nothing changes. The shared folder needs all-files access, so the setup screen asks for it before anything else.
package com.nxnative;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStreamReader;

final class Fork {
    private Fork() { }
    static final String TAG = "[FORK]";
    private static String name;   // assets/data_dir, "" when absent

    static String dataDirName(Context c) {
        if (name == null) {
            String n = "";
            try (BufferedReader r = new BufferedReader(new InputStreamReader(c.getAssets().open("data_dir"), "UTF-8"))) { String l = r.readLine(); if (l != null) n = l.trim(); }
            catch (IOException e) { }
            name = n.replaceAll("[^A-Za-z0-9._ -]", "");
        }
        return name;
    }
    static boolean shared(Context c) { return !dataDirName(c).isEmpty(); }
    // assets/only_game (build_fork.sh FORK_ONLY_GAME): the one game this build plays. The app icon goes straight to it: no game picker, no other game's setup
    private static String only;
    static String onlyGame(Context c) {
        if (only == null) { String g = "";
            try (BufferedReader r = new BufferedReader(new InputStreamReader(c.getAssets().open("only_game"), "UTF-8"))) { String l = r.readLine(); if (l != null) g = l.trim(); } catch (IOException e) { }
            for (String[] x : GameSettings.games(c)) if (x[0].equalsIgnoreCase(g)) { only = x[0]; break; }
            if (only == null) only = ""; }
        return only.isEmpty() ? null : only;
    }
    static boolean canWrite() { return Environment.isExternalStorageManager(); }

    // GameSettings.root: the shared folder in a fork build, else Android's own app folder
    static File root(Context c) {
        if (!shared(c)) return c.getExternalFilesDir(null);
        File d = new File(Environment.getExternalStorageDirectory(), dataDirName(c));
        if (!d.isDirectory() && !d.mkdirs()) Log.w(TAG, "[FORK] cannot make " + d + " (all-files access: " + canWrite() + ")");
        File nm = new File(d, ".nomedia");   // keep the gallery and music apps out of the game's textures and sounds
        if (d.isDirectory() && !nm.exists()) try { nm.createNewFile(); } catch (IOException e) { }
        return d;
    }

    // The game folder as the phone's Files app shows it (GalarFork/0100ABF008968000/<sub>), for the setup screen's and the mods menu's hints; null in a stock build
    static String shown(Context c, String sub) {
        if (!shared(c)) return null;
        String base = Environment.getExternalStorageDirectory().getPath() + "/", p = new File(GameSettings.dir(c), sub).getPath();
        return p.startsWith(base) ? p.substring(base.length()) : p;
    }

    // Portrait on the inner screen (DM_FORK_PORTRAIT=on, Display menu; default off = landscape only, as the manifest says). On a nearly square screen (the Fold's inner
    //   one) the game may turn with the phone; on the outer one it stays landscape. Called when the game starts, when the screen changes and when the menu closes
    static void orientation(Activity a) {
        android.content.res.Configuration c = a.getResources().getConfiguration();
        int lo = Math.min(c.screenWidthDp, c.screenHeightDp), hi = Math.max(c.screenWidthDp, c.screenHeightDp);
        boolean inner = lo > 0 && hi < lo * 1.4f, on = "on".equals(GameSettings.get(a, "DM_FORK_PORTRAIT"));
        int want = on && inner ? android.content.pm.ActivityInfo.SCREEN_ORIENTATION_USER : android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE;
        if (a.getRequestedOrientation() != want) { a.setRequestedOrientation(want); Log.i(TAG, "[FORK] orientation: " + (want == android.content.pm.ActivityInfo.SCREEN_ORIENTATION_USER ? "any (inner screen)" : "landscape")); }
    }

    // the player's Lossless.dll, copied in by the setup screen (private storage: never shared, never in the APK); gfx/nvn/fork_fg.h reads it through DM_FORK_LSFG_DLL
    static File lsfgDll(Context c) { return new File(new File(c.getFilesDir(), "lsfg"), "Lossless.dll"); }

    // GameActivity, before NativeActivity starts the engine: android_main.c host_start reads DM_DATA_ROOT in place of the app's externalDataPath
    static void exportRoot(Context c) {
        if (lsfgDll(c).isFile()) try { android.system.Os.setenv("DM_FORK_LSFG_DLL", lsfgDll(c).getPath(), true); } catch (Exception e) { }   // (frame generation)
        if (!shared(c)) return;
        ForkSaves.atStart(c);
        try { android.system.Os.setenv("DM_VK_CAPTURE_SIZE", "960x540", false); } catch (Exception e) { }   // v47: frame captures (only when asked for) at 960x540, not 320x180   // v47: save slots (a pending switch or restore, then this start's backup), before the engine opens the save
        // v47: the fork runs Sword at 30 (60 fps scrapped: the Fold held 30 anyway; Smoothness offers 30 with or without Lossless 2x), so Auto / 60 become 30
        if (!"30".equals(GameSettings.get(c, "DM_FPS_TARGET"))) { GameSettings.set(c, "DM_FPS_TARGET", "30"); try { android.system.Os.setenv("DM_FPS_TARGET", "30", true); } catch (Exception e) { } }
        if ("3x".equals(GameSettings.get(c, "DM_FORK_FG"))) GameSettings.set(c, "DM_FORK_FG", "2x");   // (3x scrapped in v47)
        try { android.system.Os.setenv("DM_DATA_ROOT", root(c).getPath(), true); } catch (Exception e) { Log.w(TAG, "[FORK] setenv DM_DATA_ROOT: " + e); }
    }

    // SetupActivity.onCreate: in a fork build without all-files access, a screen that asks for it and nothing else. Returns true when it took over the activity.
    // Back from Android's settings, onResume re-runs the setup screen (gateResume).
    static boolean gate(Activity a) {
        if (!shared(a) || canWrite()) return false;
        float dp = a.getResources().getDisplayMetrics().density; int pad = (int) (24 * dp);
        LinearLayout l = new LinearLayout(a); l.setOrientation(LinearLayout.VERTICAL); l.setGravity(Gravity.CENTER); l.setPadding(pad, pad, pad, pad);
        TextView t = new TextView(a); t.setTextSize(17); t.setGravity(Gravity.CENTER);
        t.setText(T.t(a, "This app keeps the game, saves and settings in the folder %s on your phone's storage, so they stay when the app is reinstalled or replaced. Allow access to all files to continue.",
                      new File(Environment.getExternalStorageDirectory(), dataDirName(a)).getPath()));
        Button b = new Button(a); b.setText(T.t(a, "Allow access"));
        b.setOnClickListener(v -> {
            try { a.startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + a.getPackageName()))); }
            catch (android.content.ActivityNotFoundException e) { a.startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)); } });
        l.addView(t); l.addView(b); a.setContentView(l);
        gated = true;
        Log.i(TAG, "[FORK] waiting for all-files access for " + dataDirName(a));
        return true;
    }
    private static boolean gated;
    // SetupActivity.onResume: access granted while the gate showed, start the screen again
    static void gateResume(Activity a) {
        if (gated && canWrite()) { gated = false; Log.i(TAG, "[FORK] all-files access granted"); a.recreate(); }
    }
}
