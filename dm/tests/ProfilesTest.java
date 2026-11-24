package com.nxnative;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.List;

// Profiles.java: the player list, each player's save folder, deleting a player, Start over and Restore. Run: javac -d out Profiles.java ProfilesTest.java; java -cp out com.nxnative.ProfilesTest
public final class ProfilesTest {
    private static void check(boolean ok, String what) { if (!ok) throw new AssertionError(what); }
    private static void put(File f, String s) throws IOException { f.getParentFile().mkdirs(); Files.write(f.toPath(), s.getBytes(StandardCharsets.UTF_8)); }
    private static String get(File f) throws IOException { return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8); }

    public static void main(String[] args) throws Exception {
        File root = Files.createTempDirectory("profiles-test").toFile();
        try {
            File sword = new File(root, "0100ABF008968000"); sword.mkdirs();
            List<File> games = Arrays.asList(root, sword);
            // an existing player, before profiles: the save stays where it is and is Player 1's
            put(new File(root, "save/main"), "shield-1"); put(new File(sword, "save/main"), "sword-1"); put(new File(root, "save.clock"), "c"); put(new File(root, "env.txt"), "K=V\n");
            check(Profiles.list(root).size() == 1 && Profiles.current(root).equals("1") && Profiles.currentName(root).equals("Player 1"), "no list: Player 1 alone, chosen");
            check(Profiles.saveRoot(root, "1").equals(root) && Profiles.saveRoot(sword, "1").equals(sword), "Player 1's save is the game folder's save");
            check(Profiles.saveRoot(root, "../x").equals(root), "a bad id falls back to Player 1, never outside the folder");

            String b = Profiles.add(root, "  Ana\tMaria \n");
            check(b.equals("2") && Profiles.find(Profiles.list(root), "2")[1].equals("Ana Maria"), "add: id 2, name cleaned to one line");
            check(Profiles.current(root).equals("1"), "adding does not choose by itself");
            Profiles.choose(root, b);
            check(Profiles.current(root).equals("2") && Profiles.saveRoot(sword, "2").equals(new File(new File(sword, "profiles"), "2")), "Player 2's save under profiles/2");
            check(get(new File(root, "current_profile")).trim().equals("2"), "current_profile holds the id (android_main.c reads it)");
            check(get(new File(root, "profiles.tsv")).equals("1\tPlayer 1\n2\tAna Maria\n"), "profiles.tsv: id TAB name lines");
            try { Profiles.add(root, "ana maria"); check(false, "a taken name is refused"); } catch (IOException e) { check(e.getMessage().equals("That name is taken."), "taken: " + e.getMessage()); }
            try { Profiles.add(root, "   "); check(false, "an empty name is refused"); } catch (IOException e) { check(e.getMessage().equals("Type a name."), "empty: " + e.getMessage()); }
            check(Profiles.clean("x".repeat(40)).length() == Profiles.MAX_NAME, "names are cut to MAX_NAME");
            Profiles.rename(root, "1", "Dan"); check(Profiles.find(Profiles.list(root), "1")[1].equals("Dan"), "rename");
            check(new File(root, "save/main").isFile(), "the existing save is never moved");

            // Start over and Restore on Player 2 in Sword
            File s2 = new File(Profiles.saveRoot(sword, "2"), "save"); put(new File(s2, "main"), "ana-old");
            List<File> dirs = Arrays.asList(s2);
            check(!Profiles.hasBackup(dirs), "no backup yet");
            check(Profiles.startOver(dirs, "20261123-100000") == 1 && !s2.exists() && new File(s2.getParentFile(), "save.bak-20261123-100000/main").isFile(), "start over: the save becomes save.bak-<date>");
            check(Profiles.hasBackup(dirs), "a backup now");
            check(Profiles.startOver(dirs, "20261123-100001") == 0, "start over with nothing saved moves nothing (and the backup stays)");
            check(new File(s2.getParentFile(), "save.bak-20261123-100000").isDirectory(), "the backup was kept when nothing new was put aside");
            put(new File(s2, "main"), "ana-new");
            check(Profiles.restore(dirs, "20261123-110000") == 1 && get(new File(s2, "main")).equals("ana-old"), "restore: the old save is back");
            check(get(new File(Profiles.backup(s2), "main")).equals("ana-new") && s2.getParentFile().list((d, n) -> n.startsWith("save.bak-")).length == 1, "restore: the newer save is the one backup now");
            check(Profiles.restore(dirs, "20261123-120000") == 1 && get(new File(s2, "main")).equals("ana-new"), "restore again switches back");
            check(Profiles.startOver(dirs, "20261123-130000") == 1 && s2.getParentFile().list((d, n) -> n.startsWith("save.bak-")).length == 1
                  && get(new File(Profiles.backup(s2), "main")).equals("ana-new"), "start over keeps only the newest backup");
            Files.delete(new File(s2.getParentFile(), "save.bak-20261123-130000/main").toPath()); s2.mkdirs();
            // the Player 1 save's neighbours are not taken for backups
            put(new File(root, "save.backup-20260101-000000/x"), "1"); check(Profiles.backup(new File(root, "save")) == null, "save.backup-* (the engine's) is not a Start over backup");

            // delete: not the last; Player 2's folders go in every game, Player 1's untouched
            put(new File(Profiles.saveRoot(root, "2"), "save/main"), "ana-shield");
            Profiles.delete(root, "2", games);
            check(!Profiles.saveRoot(root, "2").exists() && !Profiles.saveRoot(sword, "2").exists(), "delete: Player 2's saves gone in both games");
            check(Profiles.current(root).equals("1") && Profiles.list(root).size() == 1, "delete: the current player falls back to the first");
            check(get(new File(root, "save/main")).equals("shield-1") && get(new File(sword, "save/main")).equals("sword-1"), "delete: Player 1's saves untouched");
            try { Profiles.delete(root, "1", games); check(false, "the last player cannot be deleted"); } catch (IOException e) { }
            check(new File(root, "save/main").isFile(), "refused delete touched nothing");

            // deleting Player 1: only its own files in the game folders, nothing else of the app's
            String c = Profiles.add(root, "Cleo"); check(c.equals("2"), "ids are the next number above the list's: " + c);
            Profiles.delete(root, "1", games);
            check(!new File(root, "save").exists() && !new File(root, "save.clock").exists() && !new File(sword, "save").exists(), "delete Player 1: its save and save.* gone");
            check(new File(root, "env.txt").isFile() && sword.isDirectory() && new File(root, "profiles.tsv").isFile(), "delete Player 1: the app's other files stay");
            check(Profiles.current(root).equals("2") && Profiles.list(root).size() == 1, "Cleo is the one player");
            check(Profiles.add(root, "Dee").equals("3"), "a new id never reuses 1");
            // a current_profile naming no listed player: the list's first (android_main.c choose_profile does the same)
            put(new File(root, "current_profile"), "9\n"); check(Profiles.current(root).equals("2"), "unknown current → the first in the list");
            System.out.println("ProfilesTest: all checks passed");
        } finally { Profiles.deleteTree(root); }
    }
}
