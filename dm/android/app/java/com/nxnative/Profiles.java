// Player profiles: one list for the whole app (<files>/profiles.tsv, "id TAB name" a line) and the one chosen (<files>/current_profile), so every game of the app
//   (Sword and Shield, Scarlet and Violet) follows the same player. Each game keeps its own saves in its own folder:
//     Player 1 (id 1)  <game folder>/save                  (the save every player had before profiles: never moved or renamed)
//     any other        <game folder>/profiles/<id>/save
//   with everything that belongs to that save beside it (save.clock, save_place.txt, save_import, the backups). android_main.c choose_profile reads the same two
//   files at start and points DM_SAVE there. No Android classes here, so dm/tests/ProfilesTest.java runs it on a PC
package com.nxnative;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

final class Profiles {
    private Profiles() { }
    static final String FIRST = "1", FIRST_NAME = "Player 1", BAK = ".bak-";
    static final int MAX_NAME = 24;

    static File listFile(File root) { return new File(root, "profiles.tsv"); }
    static File currentFile(File root) { return new File(root, "current_profile"); }
    // (android_main.c takes the same: 1 to 16 of a-z 0-9; anything else is Player 1)
    static boolean validId(String id) { return id != null && id.matches("[a-z0-9]{1,16}"); }

    // { id, name } each; with no list yet, Player 1 alone
    static List<String[]> list(File root) {
        List<String[]> out = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(listFile(root)), StandardCharsets.UTF_8))) {
            String l; while ((l = r.readLine()) != null) { String[] a = l.split("\t", 2);
                if (a.length == 2 && validId(a[0]) && find(out, a[0]) == null && !clean(a[1]).isEmpty()) out.add(new String[] { a[0], clean(a[1]) }); }
        } catch (IOException e) { }
        if (out.isEmpty()) out.add(new String[] { FIRST, FIRST_NAME });
        return out;
    }
    static String[] find(List<String[]> ps, String id) { for (String[] p : ps) if (p[0].equals(id)) return p; return null; }
    static String current(File root) {
        String id = "";
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(currentFile(root)), StandardCharsets.UTF_8))) { String l = r.readLine(); if (l != null) id = l.trim(); } catch (IOException e) { }
        List<String[]> ps = list(root); return find(ps, id) != null ? id : ps.get(0)[0];
    }
    static String currentName(File root) { String[] p = find(list(root), current(root)); return p != null ? p[1] : FIRST_NAME; }
    static void choose(File root, String id) throws IOException {
        if (find(list(root), id) == null) throw new IOException("no such player");
        write(currentFile(root), id + "\n");
    }
    // the folder that holds this player's save in a game's folder
    static File saveRoot(File gameDir, String id) { return FIRST.equals(id) || !validId(id) ? gameDir : new File(new File(gameDir, "profiles"), id); }

    // a name as kept: one line, no tabs, spaces squeezed, at most MAX_NAME characters
    static String clean(String name) {
        String s = name == null ? "" : name.replaceAll("[\\t\\r\\n\\x00-\\x1f]", " ").replaceAll("\\s+", " ").trim();
        return s.codePointCount(0, s.length()) > MAX_NAME ? s.substring(0, s.offsetByCodePoints(0, MAX_NAME)).trim() : s;
    }
    // null when the name can be used, else why not (texts for T.t)
    static String nameProblem(List<String[]> ps, String name, String exceptId) {
        String n = clean(name); if (n.isEmpty()) return "Type a name.";
        for (String[] p : ps) if (!p[0].equals(exceptId) && p[1].equalsIgnoreCase(n)) return "That name is taken.";
        return null;
    }
    // a new player: the next free number as its id (never 1, never one in the list); returns the id
    static String add(File root, String name) throws IOException {
        List<String[]> ps = list(root); String why = nameProblem(ps, name, null); if (why != null) throw new IOException(why);
        int n = 2; for (String[] p : ps) try { n = Math.max(n, Integer.parseInt(p[0]) + 1); } catch (NumberFormatException e) { }
        String id = Integer.toString(n); ps.add(new String[] { id, clean(name) }); save(root, ps); return id;
    }
    static void rename(File root, String id, String name) throws IOException {
        List<String[]> ps = list(root); String[] p = find(ps, id); if (p == null) throw new IOException("no such player");
        String why = nameProblem(ps, name, id); if (why != null) throw new IOException(why);
        p[1] = clean(name); save(root, ps);
    }
    // Deletes a player and their saves in every game folder given (the app's folders only: a copy in Documents stays). Not the last player. The current player
    //   becomes the first one left when it was this one
    static void delete(File root, String id, List<File> gameDirs) throws IOException {
        List<String[]> ps = list(root); String[] p = find(ps, id); if (p == null) throw new IOException("no such player");
        if (ps.size() <= 1) throw new IOException("the last player cannot be deleted");
        String cur = current(root);
        ps.remove(p); save(root, ps);   // (first: a player half-deleted is gone from the list, not shown with half its saves)
        if (cur.equals(id)) write(currentFile(root), ps.get(0)[0] + "\n");
        for (File g : gameDirs) {
            if (FIRST.equals(id)) { File[] k = g.listFiles(); if (k != null) for (File f : k) if (ownedByFirst(f.getName())) deleteTree(f); }
            else deleteTree(saveRoot(g, id));
        }
    }
    // what belongs to Player 1's save in a game's folder: save, save.<anything> (its clock, backups, the engine's work copies), save_import, save_place.txt
    static boolean ownedByFirst(String n) { return n.equals("save") || n.startsWith("save.") || n.equals("save_import") || n.equals("save_place.txt"); }

    // ---------- Start over and Restore: a save folder put aside as <save>.bak-<date> next to it, only the newest kept ----------
    static File backup(File save) {
        File[] k = save.getAbsoluteFile().getParentFile().listFiles((d, n) -> n.startsWith(save.getName() + BAK)); if (k == null || k.length == 0) return null;
        Arrays.sort(k, (a, b) -> a.getName().compareTo(b.getName())); return k[k.length - 1];   // (yyyyMMdd-HHmmss: the name sorts by time)
    }
    static boolean hasBackup(List<File> saves) { for (File s : saves) if (backup(s) != null) return true; return false; }
    static boolean hasFiles(File d) { String[] k = d.list(); return k != null && k.length > 0; }
    // Start over: each save folder with something in it becomes <save>.bak-<stamp>; then the older backups go. Returns how many folders were put aside
    static int startOver(List<File> saves, String stamp) throws IOException {
        int n = 0;
        for (File s : saves) {
            if (!hasFiles(s)) continue;
            File to = fresh(s, stamp);
            if (!s.renameTo(to)) throw new IOException("could not move " + s.getName() + " aside");
            n++;
            File[] k = s.getAbsoluteFile().getParentFile().listFiles((d, nm) -> nm.startsWith(s.getName() + BAK));
            if (k != null) for (File f : k) if (!f.getName().equals(to.getName())) deleteTree(f);
        }
        return n;
    }
    // Restore: the backup becomes the save and the save becomes the backup (a swap: neither is lost, and the player can switch back). Returns how many folders
    static int restore(List<File> saves, String stamp) throws IOException {
        int n = 0;
        for (File s : saves) {
            File b = backup(s); if (b == null) continue;
            File aside = null;
            if (hasFiles(s)) { aside = fresh(s, stamp); if (!s.renameTo(aside)) throw new IOException("could not move " + s.getName() + " aside"); }
            else if (s.exists() && !s.delete()) throw new IOException("could not clear the empty " + s.getName());
            if (!b.renameTo(s)) { if (aside != null) aside.renameTo(s); throw new IOException("could not restore " + b.getName()); }
            n++;
        }
        return n;
    }
    private static File fresh(File s, String stamp) {   // <save>.bak-<stamp>, not one there already
        File to = new File(s.getAbsoluteFile().getParentFile(), s.getName() + BAK + stamp);
        for (int i = 2; to.exists(); i++) to = new File(s.getAbsoluteFile().getParentFile(), s.getName() + BAK + stamp + "-" + i);
        return to;
    }

    private static void save(File root, List<String[]> ps) throws IOException {
        StringBuilder b = new StringBuilder(); for (String[] p : ps) b.append(p[0]).append('\t').append(p[1]).append('\n'); write(listFile(root), b.toString());
    }
    private static void write(File f, String s) throws IOException {   // whole or not at all: a temporary file renamed over it
        File t = new File(f.getPath() + ".tmp");
        try (Writer w = new OutputStreamWriter(new FileOutputStream(t), StandardCharsets.UTF_8)) { w.write(s); }
        if (!t.renameTo(f)) { f.delete(); if (!t.renameTo(f)) throw new IOException("could not write " + f.getName()); }
    }
    static void deleteTree(File f) {   // (not into a link: only the link goes)
        try { File a = f.getAbsoluteFile(), p = a.getParentFile();
              if (f.isDirectory() && p != null && a.getCanonicalFile().equals(new File(p.getCanonicalFile(), a.getName()))) { File[] k = f.listFiles(); if (k != null) for (File c : k) deleteTree(c); } }
        catch (IOException e) { return; }
        f.delete();
    }
}
