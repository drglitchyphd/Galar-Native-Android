// The app updates itself from its own official releases (assets/app: the profile's app_repo on GitHub; ☰ → About → Updates).
//   What is asked: GitHub's "latest release" of that repository (pre-releases and drafts are never it), at most once a day when the app starts (a switch in About turns
//   that off) and whenever the player taps "Check for updates". Nothing about the phone or the player is sent: one anonymous GET.
//   What is installed: the release's .apk, downloaded into the app's own cache, and only when it is this app (same package), a higher version, and signed with the
//   release key (BuildSigner); the system's installer then asks the player to confirm, and checks the signature once more itself. The update keeps setup, saves and
//   settings, as installing the APK by hand does.
//   An unofficial build (any other signer) cannot take an official APK over itself, so it is never offered one: About says so instead.
//   The game closes when the update installs: the status text tells the player to save first.
package com.nxnative;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.os.Handler;
import android.os.Looper;
import android.widget.Toast;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import org.json.JSONArray;
import org.json.JSONObject;

final class AppUpdater {
    private AppUpdater() { }
    private static final long DAY = 24L * 3600 * 1000;
    private static final long MAX_APK = 200L << 20;   // (a release APK is about 24 MB: anything far beyond is not one)
    private static final String ACTION = "com.nxnative.UPDATE_STATUS";
    private static final int IDLE = 0, CHECKING = 1, LATEST = 2, AVAILABLE = 3, DOWNLOADING = 4, CONFIRM = 5, FAILED = 6;
    private static volatile int state = IDLE; private static volatile int percent; private static volatile String error = "";
    private static final Handler UI = new Handler(Looper.getMainLooper());
    static volatile Runnable listener;   // GameMenu: draw the About page again (called on the UI thread)

    private static SharedPreferences prefs(Context c) { return c.getApplicationContext().getSharedPreferences("updater", Context.MODE_PRIVATE); }
    static boolean auto(Context c) { return prefs(c).getBoolean("auto", true); }
    static void setAuto(Context c, boolean on) { prefs(c).edit().putBoolean("auto", on).apply(); }
    static boolean busy() { return state == CHECKING || state == DOWNLOADING || state == CONFIRM; }

    // "v0.96", "0.96", "v1.0.2" → {0, 96, 0}; anything else (v0.95-pre, mali-test-1) is no version to update to
    private static final Pattern VER = Pattern.compile("v?(\\d{1,5})\\.(\\d{1,5})(?:\\.(\\d{1,5}))?");
    static int[] version(String s) {
        Matcher m = VER.matcher(s == null ? "" : s.trim()); if (!m.matches()) return null;
        return new int[] { Integer.parseInt(m.group(1)), Integer.parseInt(m.group(2)), m.group(3) == null ? 0 : Integer.parseInt(m.group(3)) };
    }
    static boolean newer(String tag, String installed) {
        int[] t = version(tag), i = version(installed); if (t == null || i == null) return false;
        for (int k = 0; k < 3; k++) if (t[k] != i[k]) return t[k] > i[k];
        return false;
    }
    private static String installed(Context c) { try { return c.getPackageManager().getPackageInfo(c.getPackageName(), 0).versionName; } catch (Exception e) { return ""; } }
    // the release found by the last check, when it is newer than what is installed (else "")
    static String available(Context c) { String tag = prefs(c).getString("tag", ""); return newer(tag, installed(c)) ? tag : ""; }
    private static String repo(Context c) { String r = GameSettings.app(c)[1].trim(); return r.startsWith("github.com/") ? r.substring(11) : ""; }

    // ---------- About → Updates ----------
    static String status(Context c) {
        String tag = available(c);
        switch (state) {
            case CHECKING: return T.t(c, "Checking…");
            case DOWNLOADING: return T.t(c, "Downloading the update: %d%%", percent);
            case CONFIRM: return T.t(c, "The update is ready: confirm the installation on the screen that opens.");
            case FAILED: return error;
            case LATEST: return T.t(c, "This is the latest version.");
            default: return tag.isEmpty() ? "" : T.t(c, "Version %s is available. The game closes when the update installs, so save first.", tag);
        }
    }
    static boolean canInstall(Context c) { return !busy() && !available(c).isEmpty(); }
    // the release's own notes (as written on its page, in English), shortened
    static String notes(Context c) {
        if (available(c).isEmpty() || busy()) return "";
        String n = prefs(c).getString("notes", "").replace("\r", "").trim();
        return n.length() > 600 ? n.substring(0, 600).trim() + "…" : n;
    }

    // ---------- checking ----------
    // The app starts (SetupActivity): say that an update found earlier is waiting, and look again when the last look is a day old. The start itself never waits for it
    static void atStart(Activity a, boolean official) {
        File old = apk(a); if (old.exists() && available(a).isEmpty()) old.delete();   // (the update it held is installed)
        if (!official || !auto(a) || repo(a).isEmpty()) return;
        SharedPreferences p = prefs(a); long now = System.currentTimeMillis();
        announce(a);
        if (Math.abs(now - p.getLong("checked", 0)) >= DAY) check(a, false);
    }
    // once a day at most: a player who chose "later" is not told at every start
    private static void announce(Context c) {
        String tag = available(c); if (tag.isEmpty()) return;
        SharedPreferences p = prefs(c); long now = System.currentTimeMillis();
        if (Math.abs(now - p.getLong("told", 0)) < DAY) return;
        p.edit().putLong("told", now).apply();
        Toast.makeText(c.getApplicationContext(), T.t(c, "%s %s is available. Open the menu, then About, to install it.", GameSettings.app(c)[0], tag), Toast.LENGTH_LONG).show();
    }
    static void check(Activity a, boolean asked) {
        if (busy()) return;
        final Context c = a.getApplicationContext(); final String repo = repo(c);
        if (repo.isEmpty()) return;
        if (asked) set(CHECKING, "");
        new Thread(() -> {
            try {
                JSONObject r = new JSONObject(new String(get("https://api.github.com/repos/" + repo + "/releases/latest", 1 << 20), "UTF-8"));
                String tag = r.optString("tag_name", ""), url = "", digest = ""; long size = 0;
                JSONArray as = r.optJSONArray("assets");
                for (int i = 0; as != null && i < as.length(); i++) {   // the release's APK: the one named after the tag when there are several
                    JSONObject o = as.getJSONObject(i); String n = o.optString("name", ""), u = o.optString("browser_download_url", "");
                    if (!n.toLowerCase(Locale.ROOT).endsWith(".apk") || !u.startsWith("https://github.com/" + repo + "/releases/download/")) continue;
                    if (url.isEmpty() || n.contains(tag)) { url = u; size = o.optLong("size", 0); digest = o.optString("digest", ""); }
                }
                if (version(tag) == null || url.isEmpty()) throw new Exception("no release");
                prefs(c).edit().putLong("checked", System.currentTimeMillis()).putString("tag", tag).putString("url", url).putLong("size", size)
                        .putString("digest", digest).putString("notes", r.optString("body", "")).apply();
                if (available(c).isEmpty()) { if (asked) set(LATEST, ""); return; }
                if (asked) set(AVAILABLE, ""); else UI.post(() -> announce(c));
            } catch (Exception e) {
                // (a start's own look says nothing when it fails: no network is no news, and the next start looks again)
                if (asked) set(FAILED, T.t(c, "Could not check for updates. Check the internet connection."));
            }
        }, "updater").start();
    }

    // ---------- installing ----------
    private static File apk(Context c) { return new File(c.getCacheDir(), "update.apk"); }
    static void install(Activity a) {
        if (busy()) return;
        final Context c = a.getApplicationContext(); final SharedPreferences p = prefs(c);
        final String url = p.getString("url", ""), digest = p.getString("digest", ""); final long size = p.getLong("size", 0);
        if (available(c).isEmpty() || url.isEmpty()) return;
        percent = 0; set(DOWNLOADING, "");
        new Thread(() -> {
            File f = apk(c);
            try {
                String got = download(url, f, size);
                if (digest.startsWith("sha256:") && !digest.substring(7).equalsIgnoreCase(got)) throw new Exception("checksum");   // (GitHub's own digest of the uploaded file)
            } catch (Exception e) { f.delete(); set(FAILED, T.t(c, "The update could not be downloaded: %s", String.valueOf(e.getMessage()))); return; }
            if (!ours(c, f)) { f.delete(); set(FAILED, T.t(c, "The downloaded file is not an official %s release, so it was not installed.", GameSettings.app(c)[0])); return; }
            try { commit(a, f); set(CONFIRM, ""); }
            catch (Exception e) { set(FAILED, T.t(c, "The update could not be installed: %s", String.valueOf(e.getMessage()))); }
        }, "updater").start();
    }
    // this app, a higher version, the release key. (A file the package manager reads no signer from is left to the system's installer, which refuses another key itself)
    private static boolean ours(Context c, File f) {
        try {
            PackageManager pm = c.getPackageManager();
            PackageInfo pi = pm.getPackageArchiveInfo(f.getPath(), PackageManager.GET_SIGNING_CERTIFICATES);
            if (pi == null || !c.getPackageName().equals(pi.packageName)) return false;
            if (pi.getLongVersionCode() <= pm.getPackageInfo(c.getPackageName(), 0).getLongVersionCode()) return false;
            Signature[] sg = pi.signingInfo == null ? null : pi.signingInfo.getApkContentsSigners();
            if (sg == null || sg.length == 0) return true;
            String[] d = new String[sg.length]; for (int i = 0; i < sg.length; i++) d[i] = BuildSigner.sha256(sg[i].toByteArray());
            return BuildSigner.official(d);
        } catch (Exception e) { return false; }
    }
    // Hand the file to the system's installer. It answers through the broadcast below: first "the player must confirm" with the screen to show, then the outcome
    //   (on success the app is closed by the install and nothing arrives)
    private static BroadcastReceiver receiver;
    private static void commit(Activity a, File f) throws Exception {
        final Context c = a.getApplicationContext();
        PackageInstaller pi = c.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams sp = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        sp.setAppPackageName(c.getPackageName()); sp.setSize(f.length());
        int id = pi.createSession(sp);
        try (PackageInstaller.Session s = pi.openSession(id)) {
            try (InputStream in = new FileInputStream(f); OutputStream out = s.openWrite("base.apk", 0, f.length())) {
                byte[] b = new byte[1 << 16]; int n; while ((n = in.read(b)) > 0) out.write(b, 0, n);
                s.fsync(out);
            }
            synchronized (AppUpdater.class) {
                if (receiver == null) {
                    receiver = new BroadcastReceiver() { @Override public void onReceive(Context x, Intent i) { outcome(x, i); } };
                    c.registerReceiver(receiver, new IntentFilter(ACTION), android.os.Build.VERSION.SDK_INT >= 33 ? Context.RECEIVER_NOT_EXPORTED : 0);
                }
            }
            // (mutable: the installer adds its status to the intent; the package is named, so only this app receives it)
            PendingIntent done = PendingIntent.getBroadcast(c, id, new Intent(ACTION).setPackage(c.getPackageName()), PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
            s.commit(done.getIntentSender());
        } catch (Exception e) { try { pi.abandonSession(id); } catch (Exception e2) { } throw e; }
    }
    private static void outcome(Context c, Intent i) {
        int st = i.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        if (st == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Intent ask = i.getParcelableExtra(Intent.EXTRA_INTENT);
            try { if (ask != null) c.startActivity(ask.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)); } catch (Exception e) { set(FAILED, T.t(c, "The update could not be installed: %s", String.valueOf(e.getMessage()))); }
        } else if (st == PackageInstaller.STATUS_FAILURE_ABORTED) set(AVAILABLE, "");   // (the player said no: the update stays on offer)
        else if (st != PackageInstaller.STATUS_SUCCESS) set(FAILED, T.t(c, "The update could not be installed: %s", String.valueOf(i.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE))));
    }

    // ---------- network ----------
    private static HttpURLConnection open(String url) throws Exception {
        HttpURLConnection h = (HttpURLConnection) new URL(url).openConnection();
        h.setConnectTimeout(10000); h.setReadTimeout(20000); h.setRequestProperty("User-Agent", "nxnative-updater"); h.setRequestProperty("Accept", "application/vnd.github+json, application/octet-stream");
        if (!"https".equals(h.getURL().getProtocol())) throw new Exception("not https");
        int code = h.getResponseCode(); if (code != 200) { h.disconnect(); throw new Exception("HTTP " + code); }
        return h;
    }
    private static byte[] get(String url, int max) throws Exception {
        HttpURLConnection h = open(url);
        try (InputStream in = h.getInputStream()) {
            ByteArrayOutputStream o = new ByteArrayOutputStream(); byte[] b = new byte[8192]; int n;
            while ((n = in.read(b)) > 0) { o.write(b, 0, n); if (o.size() > max) throw new Exception("too large"); }
            return o.toByteArray();
        } finally { h.disconnect(); }
    }
    // to the file; the SHA-256 of what arrived, in hex. The length must be the one the release lists
    private static String download(String url, File f, long size) throws Exception {
        HttpURLConnection h = open(url);
        try (InputStream in = h.getInputStream(); OutputStream out = new FileOutputStream(f)) {
            MessageDigest md = MessageDigest.getInstance("SHA-256"); long total = size > 0 ? size : h.getContentLengthLong(), got = 0; int shown = -1;
            byte[] b = new byte[1 << 16]; int n;
            while ((n = in.read(b)) > 0) {
                out.write(b, 0, n); md.update(b, 0, n); got += n;
                if (got > MAX_APK) throw new Exception("too large");
                int pc = total > 0 ? (int) Math.min(100, got * 100 / total) : 0;
                if (pc != shown && pc % 5 == 0) { shown = pc; percent = pc; changed(); }
            }
            if (size > 0 && got != size) throw new Exception(got + " / " + size);
            StringBuilder s = new StringBuilder(64); for (byte x : md.digest()) s.append(String.format(Locale.ROOT, "%02x", x & 0xff));
            return s.toString();
        } finally { h.disconnect(); }
    }

    private static void set(int s, String err) { state = s; error = err; changed(); }
    private static void changed() { UI.post(() -> { Runnable r = listener; if (r != null) r.run(); }); }
}
