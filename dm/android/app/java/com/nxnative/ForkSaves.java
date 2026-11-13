// FORK: save slots (in-game menu → Saves). The active slot always lives where the engine reads it, <game>/save (android_main.c DM_SAVE; upstream's export and
// import work on it unchanged). Every other slot waits in <game>/saves/<id>/save. Choosing another slot sets DM_FORK_SLOT_NEXT; at the next start, before the
// engine runs (Fork.exportRoot), the folders are swapped by rename (same file system: instant, nothing copied). Then the active slot's save is copied to
// <game>/saves/<id>/backups/<time>/ and only the 3 newest backups are kept. A backup chosen for restore (DM_FORK_RESTORE) is copied in first.
// Slots are listed in <game>/saves/slots.txt, one "id<TAB>name" a line; "main" is the save that was there before slots (named "Main").
// Log lines: [FORK] saves ...
package com.nxnative;

import android.content.Context;
import android.util.Log;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.List;
import java.util.Locale;

final class ForkSaves {
    private static final String TAG = "[FORK]";
    static final int KEEP = 3;

    static File gameDir(Context c) { return GameSettings.dir(c); }
    static File slotsDir(Context c) { return new File(gameDir(c), "saves"); }
    static String active(Context c) { String s = GameSettings.get(c, "DM_FORK_SLOT"); return s.isEmpty() ? "main" : s; }
    static String next(Context c) { String s = GameSettings.get(c, "DM_FORK_SLOT_NEXT"); return s.isEmpty() ? active(c) : s; }
    /** where a slot's save is now: the active one in <game>/save, the others in saves/<id>/save */
    static File saveOf(Context c, String id) { return id.equals(active(c)) ? new File(gameDir(c), "save") : new File(new File(slotsDir(c), id), "save"); }
    static File backupsOf(Context c, String id) { return new File(new File(slotsDir(c), id), "backups"); }

    /** { id, name } in list order, "main" first */
    static List<String[]> list(Context c) {
        List<String[]> l = new ArrayList<>();
        File f = new File(slotsDir(c), "slots.txt");
        if (f.isFile()) try { for (String ln : new String(Files.readAllBytes(f.toPath()), "UTF-8").split("\n")) { int t = ln.indexOf('\t'); if (t > 0) l.add(new String[] { ln.substring(0, t), ln.substring(t + 1).trim() }); } } catch (IOException e) { }
        boolean main = false; for (String[] s : l) if (s[0].equals("main")) main = true;
        if (!main) l.add(0, new String[] { "main", "Main" });
        return l;
    }
    static void store(Context c, List<String[]> l) {
        slotsDir(c).mkdirs();
        try (FileWriter w = new FileWriter(new File(slotsDir(c), "slots.txt"))) { for (String[] s : l) w.write(s[0] + "\t" + s[1].replace('\t', ' ').replace('\n', ' ') + "\n"); } catch (IOException e) { Log.w(TAG, "[FORK] saves: slots.txt: " + e); }
    }
    static String nameOf(Context c, String id) { for (String[] s : list(c)) if (s[0].equals(id)) return s[1]; return id; }

    /** a new slot: empty (a new game) or a copy of the active slot's save as it is on storage now */
    static String create(Context c, String name, boolean copy) throws IOException {
        String id = "s" + Long.toString(System.currentTimeMillis(), 36);
        File d = new File(new File(slotsDir(c), id), "save"); d.mkdirs();
        if (copy) copyTree(new File(gameDir(c), "save"), d);
        List<String[]> l = list(c); l.add(new String[] { id, name.trim().isEmpty() ? "Slot " + l.size() : name.trim() }); store(c, l);
        Log.i(TAG, "[FORK] saves: slot " + id + " \"" + name + "\" made" + (copy ? ", a copy of " + active(c) : ", empty"));
        return id;
    }
    static void rename(Context c, String id, String name) { List<String[]> l = list(c); for (String[] s : l) if (s[0].equals(id)) s[1] = name.trim().isEmpty() ? s[1] : name.trim(); store(c, l); }
    /** not the active slot, nor the one chosen for the next start */
    static boolean delete(Context c, String id) {
        if (id.equals(active(c)) || id.equals(next(c)) || id.equals("main")) return false;
        deleteTree(new File(slotsDir(c), id));
        List<String[]> l = list(c); l.removeIf(s -> s[0].equals(id)); store(c, l);
        Log.i(TAG, "[FORK] saves: slot " + id + " deleted"); return true;
    }
    static void choose(Context c, String id) { GameSettings.set(c, "DM_FORK_SLOT_NEXT", id.equals(active(c)) ? "" : id); }
    /** newest first */
    static File[] backups(Context c, String id) {
        File[] b = backupsOf(c, id).listFiles(File::isDirectory); if (b == null) return new File[0];
        Arrays.sort(b, (x, y) -> y.getName().compareTo(x.getName())); return b;
    }
    /** the active slot's backup: copied in at the next start (the game holds its save while it runs); another slot's: copied in now */
    static void restore(Context c, String id, File backup) throws IOException {
        if (id.equals(active(c))) { GameSettings.set(c, "DM_FORK_RESTORE", backup.getPath()); return; }
        File d = saveOf(c, id); deleteTree(d); d.mkdirs(); copyTree(backup, d);
        Log.i(TAG, "[FORK] saves: slot " + id + " restored from " + backup.getName());
    }

    /** Fork.exportRoot, before the engine starts: the slot switch, a restore, then this start's backup */
    static void atStart(Context c) {
        try {
            File game = gameDir(c), live = new File(game, "save");
            String cur = active(c), want = GameSettings.get(c, "DM_FORK_SLOT_NEXT");
            if (!want.isEmpty() && !want.equals(cur)) {
                File park = new File(new File(slotsDir(c), cur), "save"), in = new File(new File(slotsDir(c), want), "save");
                park.getParentFile().mkdirs();
                if (park.exists()) { Log.w(TAG, "[FORK] saves: " + park + " is in the way: the switch waits"); }
                else if (live.exists() && !live.renameTo(park)) Log.w(TAG, "[FORK] saves: could not park slot " + cur);
                else {
                    File w = new File(game, "save.work"); if (w.exists()) deleteTree(w);   // (the engine's working copy of the old slot: made again from the save at mount)
                    if (in.exists() && !in.renameTo(live)) { park.renameTo(live); Log.w(TAG, "[FORK] saves: could not bring in slot " + want + ": still on " + cur); }
                    else { if (!live.exists()) live.mkdirs(); GameSettings.set(c, "DM_FORK_SLOT", want.equals("main") ? "" : want); Log.i(TAG, "[FORK] saves: slot " + cur + " → " + want); }
                }
                GameSettings.set(c, "DM_FORK_SLOT_NEXT", "");
            }
            String rs = GameSettings.get(c, "DM_FORK_RESTORE");
            if (!rs.isEmpty()) {
                GameSettings.set(c, "DM_FORK_RESTORE", "");
                File b = new File(rs);
                if (b.isDirectory()) { File w = new File(game, "save.work"); if (w.exists()) deleteTree(w); deleteTree(live); live.mkdirs(); copyTree(b, live); Log.i(TAG, "[FORK] saves: restored " + b.getName() + " into " + active(c)); }
            }
            if (live.isDirectory() && hasFiles(live)) {
                File bd = new File(backupsOf(c, active(c)), new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date()));
                bd.mkdirs(); copyTree(live, bd);
                File[] all = backups(c, active(c)); for (int i = KEEP; i < all.length; i++) deleteTree(all[i]);
                Log.i(TAG, "[FORK] saves: slot " + active(c) + " backed up (" + Math.min(all.length, KEEP) + " kept)");
            }
        } catch (Exception e) { Log.w(TAG, "[FORK] saves: " + e); }
    }

    private static boolean hasFiles(File d) { File[] f = d.listFiles(); if (f == null) return false; for (File x : f) if (x.isFile() || (x.isDirectory() && hasFiles(x))) return true; return false; }
    static void copyTree(File from, File to) throws IOException {
        File[] f = from.listFiles(); if (f == null) return; to.mkdirs();
        for (File x : f) {
            File y = new File(to, x.getName());
            if (x.isDirectory()) copyTree(x, y);
            else try (FileInputStream i = new FileInputStream(x); FileOutputStream o = new FileOutputStream(y)) { byte[] b = new byte[1 << 16]; int n; while ((n = i.read(b)) > 0) o.write(b, 0, n); }
        }
    }
    static void deleteTree(File f) { File[] k = f.listFiles(); if (k != null) for (File x : k) deleteTree(x); f.delete(); }
}
