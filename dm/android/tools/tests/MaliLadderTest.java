// The app's side of the Mali white-screen ladder on a desktop JVM (no phone, no Android runtime: android.jar's stubs only on the class path): MaliLadder's state
//   file handling (fields / due / shown / gpuOr, read / write) and DriverPackages' choice of a PanVK build by GPU name (sourceFor, panvkBifrost), against state
//   files the engine's own code wrote (dm/gfx/tests/t_mali_white.c with MW_FIXTURE_DIR=<dir>: mali_white.h mw_state_save) and against hand-written ones.
//   bash dm/android/tools/tests/t_mali_ladder_java.sh [fixture dir]       (javac + java; ANDROID_JAR=<android.jar>)
package com.nxnative;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;

public final class MaliLadderTest {
    static int checks, fails;
    static void check(boolean ok, String what) { checks++; if (!ok) fails++; System.out.println((ok ? "PASS " : "FAIL ") + what); }
    static String src(String gpu) { String[] s = DriverPackages.sourceFor(gpu, "", 34); return s == null ? null : s[1]; }

    public static void main(String[] args) throws Exception {
        // a state file as the engine writes it (mw_state_format): both routes failed, the offer asked for
        List<String> offer = Arrays.asList("# Galar Native: the white-screen check on this phone's Mali driver (nvn_vk.c, mali_white.h). Started over when the driver or the app changes",
            "driver=9", "driver_version=0xc401000", "app=96", "gpu=Mali-G720-Immortalis MC12", "A=white", "B=crashed", "A_fine_starts=0", "B_fine_starts=0", "A_rechecks=0",
            "B_rechecks=0", "A_crashes=0", "B_crashes=2", "started=-", "early_ends=0", "next=A (both failed: unchecked)", "panvk=offer");
        Map<String, String> f = MaliLadder.fields(offer);
        check("96".equals(f.get("app")) && "offer".equals(f.get("panvk")) && "Mali-G720-Immortalis MC12".equals(f.get("gpu")) && "A (both failed: unchecked)".equals(f.get("next")) && !f.containsKey("#"),
              "fields: every key=value line, the comment left out, a value with spaces and ':' kept whole");
        check(MaliLadder.due(f, 96, "") && MaliLadder.due(f, 96, null), "the offer is due: asked for, this app version, the phone's own driver");
        check(!MaliLadder.due(f, 97, ""), "not due for another app version (until the engine has started on it: it writes app= at each start)");
        check(!MaliLadder.due(f, 96, "/data/user/0/com.nxnative.pokemonshield/files/drivers/panvk/libvulkan_panvk.so"), "not due once a driver package is in use");
        List<String> sh = MaliLadder.shown(offer); Map<String, String> fs = MaliLadder.fields(sh);
        check("shown".equals(fs.get("panvk")) && !MaliLadder.due(fs, 96, "") && sh.size() == offer.size() && sh.subList(0, sh.size() - 1).equals(offer.subList(0, offer.size() - 1)),
              "shown: panvk=shown, every other line as it was, never due again");
        check("Mali-G720-Immortalis MC12".equals(MaliLadder.gpuOr(f, "mali-g57 mc2")), "the GPU: the engine's gpu= line before the game's gpu.txt");
        { List<String> old = new ArrayList<>(offer); old.remove(4); check("Mali-G610 MC6".equals(MaliLadder.gpuOr(MaliLadder.fields(old), " Mali-G610 MC6\n".trim())) && "".equals(MaliLadder.gpuOr(MaliLadder.fields(old), null)),
              "a file without gpu= (older): the game's gpu.txt, else nothing"); }
        { Map<String, String> g = MaliLadder.fields(Arrays.asList("app=96", "panvk=offerX", "x", "=y", "A=white=really"));
          check(!MaliLadder.due(g, 96, "") && "white=really".equals(g.get("A")) && !g.containsKey(""), "odd lines: a value of another word is not an offer; '=' inside a value kept; a line without a key left out"); }
        { Map<String, String> g = MaliLadder.fields(Arrays.asList("app=96\r", "panvk=offer ")); check(MaliLadder.due(g, 96, ""), "a file edited on Windows (CR, a space): still read"); }
        // the engine's notes: "mali-env:<switches>" names the env.txt switches that hold the fix back (mali_white_vk.inc mw_moved), the menu's names for them
        { String[] k = MaliLadder.noteKey("mali-env:DM_VK_B10_AS_RGBA16,DM_VK_B10_NOCOMPRESS"), k2 = MaliLadder.noteKey("mali-try-b"), k3 = MaliLadder.noteKey(null);
          check("mali-env".equals(k[0]) && "DM_VK_B10_AS_RGBA16,DM_VK_B10_NOCOMPRESS".equals(k[1]) && "mali-try-b".equals(k2[0]) && "".equals(k2[1]) && "".equals(k3[0]) && "".equals(k3[1]),
                "a note's key and the switches after its ':' (none: empty; no key: empty)"); }
        check("Bright scenes in a wider format".equals(MaliLadder.switchLabel("DM_VK_B10_AS_RGBA16")) && "Bright scenes without compression".equals(MaliLadder.switchLabel("DM_VK_B10_NOCOMPRESS"))
              && MaliLadder.switchLabel("DM_VK_PROBE") == null && MaliLadder.switchLabel("") == null, "the menu's names of the two route switches (translated by T.t); any other: its own key");
        // the PanVK build for each GPU (deviceName in lower case, as the offer and the menu pass it)
        check(DriverPackages.PANVK_FAUX_RELEASES.equals(src("mali-g720-immortalis mc12")) && DriverPackages.PANVK_FAUX_RELEASES.equals(src("mali-g720 mc7"))
              && DriverPackages.PANVK_FAUX_RELEASES.equals(src("mali-g725 mc6")), "Mali-G720 / G725 (CSF): faux123's PanVK (the Bifrost G72 rule no longer takes them)");
        check(DriverPackages.PANVK_RELEASES.equals(src("mali-g72 mp3")) && DriverPackages.PANVK_RELEASES.equals(src("mali-g57 mc2")) && DriverPackages.PANVK_RELEASES.equals(src("mali-g52"))
              && DriverPackages.PANVK_RELEASES.equals(src("mali-g76 mc4")), "Mali-G72 / G57 / G52 / G76 (Bifrost, Valhall): FristOneRR's PanVK");
        check(DriverPackages.PANVK_FAUX_RELEASES.equals(src("mali-g615 mc6")) && DriverPackages.PANVK_FAUX_RELEASES.equals(src("immortalis-g925 mc12")) && DriverPackages.PANVK_FAUX_RELEASES.equals(src("mali-g610 mc6")),
              "Mali-G615 / G610, Immortalis-G925: faux123's PanVK");
        check(src("mali-g310") == null && src("mali-g510 mc2") == null && src("mali-g31 mp2") == null && src("mali-g51 mp4") == null && src("mali-g71 mp20") == null && src("mali-g1-ultra mc12") == null
              && src("mali") == null && src("") == null, "no PanVK build known: G310 / G510 / G31 / G51 / G71, the Mali-G1 family, an unknown name (the offer gives the setup screen's advice)");
        check(DriverPackages.panvkBifrost("mali-g72 mp18") && !DriverPackages.panvkBifrost("mali-g720 mc7") && !DriverPackages.panvkBifrost("mali-g725") && DriverPackages.panvkBifrost("mali-g57"),
              "the setup screen's advice keys on the same rule (G72 yes, G720 / G725 no)");
        // read / write through a file: whole lines, UTF-8, the offer marked shown in place
        File d = new File(System.getProperty("java.io.tmpdir"), "mali_ladder_test_" + System.nanoTime()); d.mkdirs(); File t = new File(d, "mali_ladder.txt");
        MaliLadder.write(t, offer); check(MaliLadder.read(t).equals(offer), "write then read: the same lines");
        MaliLadder.write(t, MaliLadder.shown(MaliLadder.read(t))); check(!MaliLadder.due(MaliLadder.fields(MaliLadder.read(t)), 96, "") && !new File(d, "mali_ladder.txt.tmp").exists(), "marked shown in the file, no .tmp left");
        t.delete(); d.delete();
        // the engine's own files (t_mali_white MW_FIXTURE_DIR)
        if (args.length > 0) {
            File fx = new File(args[0]); String[] want = { "offer.txt", "offer-g310.txt", "offer-g1.txt", "offer-nogpu.txt", "offer-g57.txt", "shown.txt", "none.txt", "oldapp.txt" };
            for (String n : want) check(new File(fx, n).exists(), "the engine's state file " + n + " is there");
            Map<String, String> o = MaliLadder.fields(MaliLadder.read(new File(fx, "offer.txt")));
            check(MaliLadder.due(o, 96, "") && "Mali-G720 MC7".equals(MaliLadder.gpuOr(o, "")) && DriverPackages.PANVK_FAUX_RELEASES.equals(src(MaliLadder.gpuOr(o, "").toLowerCase(java.util.Locale.ROOT))),
                  "the engine's offer.txt: due, the GPU from gpu=, faux123's PanVK for its Mali-G720");
            check(!MaliLadder.due(MaliLadder.fields(MaliLadder.shown(MaliLadder.read(new File(fx, "offer.txt")))), 96, ""), "... marked shown: not due");
            Map<String, String> g3 = MaliLadder.fields(MaliLadder.read(new File(fx, "offer-g310.txt")));
            check(MaliLadder.due(g3, 96, "") && src(MaliLadder.gpuOr(g3, "").toLowerCase(java.util.Locale.ROOT)) == null && "crashed".equals(g3.get("B")),
                  "offer-g310.txt (A white, B crashed): due, no PanVK build for a Mali-G310 (the advice instead)");
            check(src(MaliLadder.gpuOr(MaliLadder.fields(MaliLadder.read(new File(fx, "offer-g1.txt"))), "").toLowerCase(java.util.Locale.ROOT)) == null, "offer-g1.txt: no PanVK build for the Mali-G1 family");
            check(DriverPackages.PANVK_RELEASES.equals(src(MaliLadder.gpuOr(MaliLadder.fields(MaliLadder.read(new File(fx, "offer-g57.txt"))), "").toLowerCase(java.util.Locale.ROOT))), "offer-g57.txt: FristOneRR's PanVK");
            check("from gpu.txt".equals(MaliLadder.gpuOr(MaliLadder.fields(MaliLadder.read(new File(fx, "offer-nogpu.txt"))), "from gpu.txt")), "offer-nogpu.txt (an empty gpu=): the game's gpu.txt");
            check(!MaliLadder.due(MaliLadder.fields(MaliLadder.read(new File(fx, "shown.txt"))), 96, "") && !MaliLadder.due(MaliLadder.fields(MaliLadder.read(new File(fx, "none.txt"))), 96, ""),
                  "shown.txt / none.txt: not due");
            Map<String, String> oa = MaliLadder.fields(MaliLadder.read(new File(fx, "oldapp.txt")));
            check(!MaliLadder.due(oa, 96, "") && MaliLadder.due(oa, 95, ""), "oldapp.txt (made by versionCode 95): due for 95 only");
        } else System.out.println("NOTE no fixture folder given: the engine's own state files not read (t_mali_white MW_FIXTURE_DIR=<dir>)");
        System.out.println((fails == 0 ? "PASS" : "FAIL") + " MaliLadderTest: " + fails + " of " + checks + " checks failed");
        System.exit(fails == 0 ? 0 : 1);
    }
}
