// FORK: DLC added to a game that is already set up, without setting it up again. The unpacker's own DLC mode (nx_extract <prod.keys> --aoc <dlc.nsp> <dir>,
// dm/tools/nx_extract Program.cs Aoc) writes each DLC's files to <game folder>/aoc/<index>; the engine reads that folder at start (android_main.c DM_AOC_DIR).
// Only DLC of the game being set up is kept: its title ID is the game's + 0x1000 + index (Sword 0100ABF008968000 → 0100ABF008969001, ...9002).
// For Sword and Shield the DLC packages are small (the expansions' content is in the 1.3.2 update): this takes seconds and leaves the game and its shaders as they are.
package com.nxnative;

import android.content.Context;
import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

final class ForkDlc {
    private ForkDlc() { }

    // the DLC set up for this game: the folders in aoc/
    static List<String> installed(Context c) {
        List<String> out = new ArrayList<>(); File[] ds = new File(GameSettings.dir(c), "aoc").listFiles();
        if (ds != null) for (File d : ds) if (d.isDirectory()) out.add(d.getName());
        java.util.Collections.sort(out); return out;
    }

    // one DLC file (its path on the phone) into aoc/; returns null when done, else why not. Runs the unpacker: call off the UI thread
    static String install(Context c, String nsp, File bin, File keys, File py, File dm) {
        String game = GameSettings.game(c)[0]; long base = Long.parseUnsignedLong(game, 16);
        File aoc = new File(GameSettings.dir(c), "aoc"), tmp = new File(GameSettings.dir(c), "aoc.new");
        deleteTree(tmp); tmp.mkdirs();
        List<String> cmd = new ArrayList<>(); cmd.add(new File(bin, "nx_extract").getPath()); cmd.add(new File(keys, "prod.keys").getPath());
        cmd.add("--aoc"); cmd.add(nsp); cmd.add(tmp.getPath());
        StringBuilder log = new StringBuilder(); List<String[]> got = new ArrayList<>();
        Pattern line = Pattern.compile("add-on content ([0-9a-fA-F]{16}) \\S+ (.+)");
        try {
            ProcessBuilder pb = new ProcessBuilder(cmd).redirectErrorStream(true); SetupActivity.toolEnv(c, pb, py, dm, bin, 1);
            Process p = pb.start();
            try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()))) { String l; while ((l = r.readLine()) != null) {
                log.append(l).append('\n'); Matcher m = line.matcher(l); if (m.find()) got.add(new String[] { m.group(1), m.group(2).trim() }); } }
            int rc = p.waitFor();
            android.util.Log.i(Fork.TAG, "[FORK] DLC " + nsp + ": exit " + rc + "\n" + log);
            if (rc != 0) { deleteTree(tmp); String last = log.toString().trim(); int i = last.lastIndexOf('\n'); return T.t(c, "the DLC file could not be read (%s)", i >= 0 ? last.substring(i + 1) : last); }
        } catch (Exception e) { deleteTree(tmp); return e.toString(); }
        int kept = 0; aoc.mkdirs();
        for (String[] g : got) {
            long tid = Long.parseUnsignedLong(g[0], 16); File from = new File(g[1]);
            if ((tid & ~0x7ffL) != base + 0x1000) { android.util.Log.w(Fork.TAG, "[FORK] DLC " + g[0] + " is not " + game + "'s: left out"); continue; }
            File to = new File(aoc, from.getName()); deleteTree(to);
            if (!from.renameTo(to)) { deleteTree(tmp); return T.t(c, "could not move the DLC into %s", aoc.getPath()); }
            kept++;
        }
        deleteTree(tmp);
        if (kept == 0) return got.isEmpty() ? T.t(c, "no DLC found in that file") : T.t(c, "that DLC is for another game, not %s", GameSettings.game(c)[1]);
        return null;
    }
    static void deleteTree(File f) { File[] k = f.listFiles(); if (k != null) for (File x : k) deleteTree(x); f.delete(); }
}
