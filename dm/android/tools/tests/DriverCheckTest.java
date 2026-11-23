// The app's side of the driver check on a desktop JVM (no phone, no Android runtime: android.jar's stubs only on the class path): DriverCheck's one-check-at-a-time
//   guard (claim / release: a second Play, or the setup screen made again, while a check runs) and how an unfinished check is resumed (unfinished / resume: the
//   journal of a check whose app closed during it, as the self-tests and DriverCheck.check write it).
//   bash dm/android/tools/tests/t_driver_check_java.sh       (javac + java; ANDROID_JAR=<android.jar>)
package com.nxnative;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public final class DriverCheckTest {
    static int checks, fails;
    static void check(boolean ok, String what) { checks++; if (!ok) fails++; System.out.println((ok ? "PASS " : "FAIL ") + what); }

    public static void main(String[] args) {
        // claim / release: one check; while it runs a second call only takes over what runs after it
        final int[] ran = new int[3];
        Runnable g1 = () -> ran[1]++, g2 = () -> ran[2]++;
        check(DriverCheck.claim(g1), "the first Play starts a check");
        check(!DriverCheck.claim(g2), "a second Play while it runs starts none (its go waits for the running check)");
        Runnable after = DriverCheck.release();
        check(after == g2, "the check's end runs the latest go only (the setup screen made again: its own), once");
        check(DriverCheck.release() == null, "nothing more runs after it");
        check(DriverCheck.claim(g1) && DriverCheck.release() == g1, "after the end a new check can start");

        // unfinished: driver-check.run's key and start count
        check(DriverCheck.unfinished("", "3|fp|-|96") == 0, "no driver-check.run: no unfinished check");
        check(DriverCheck.unfinished("3|fp|-|96\n1\n", "3|fp|-|96") == 1, "one unfinished start of this key");
        check(DriverCheck.unfinished("3|fp|-|95\n1\n", "3|fp|-|96") == 0, "another key's (the app or the driver changed since): none");
        check(DriverCheck.unfinished("3|fp|-|96\nx\n", "3|fp|-|96") == 0 && DriverCheck.unfinished("3|fp|-|96\n-4\n", "3|fp|-|96") == 0, "a damaged count: none");

        // resume: a crash of the check's process (its setting), a hang (no setting), and the app itself closed during a test (left out, no setting)
        String j = "attempt 0 skip \nprobe 1\ngpu 0x13b5 0x1 0x1 1.3.0 Mali\nbegin engine-requirements\nresult engine-requirements PASS ok\n"
                 + "begin int-view DM_VK_INT_TEX=keep\nresult int-view FAIL wrong\nfix int-view DM_VK_INT_TEX=keep\n"
                 + "begin dmabuf-heap DM_VK_DMAHEAP=0\nlost dmabuf-heap crashed DM_VK_DMAHEAP=0\n"
                 + "attempt 1 skip dmabuf-heap\nprobe 1\nbegin bc-textures DM_VK_BC_GPU=0\nlost bc-textures hung\n"
                 + "attempt 2 skip dmabuf-heap,bc-textures\nprobe 1\nbegin depth-copy DM_VK_ZCOPY=0\nresult depth-copy PASS ok\nbegin 3d-layered DM_VK_NO_LAYERED_3D=1\n"
                 + "begin 3d-layered DM_VK_CLEAR_BY_PASS=1\n";
        List<String> skip = new ArrayList<>(), crashed = new ArrayList<>(); Map<String, String> fixes = new LinkedHashMap<>();
        DriverCheck.resume(j, skip, fixes, crashed);
        check(skip.equals(java.util.Arrays.asList("dmabuf-heap", "bc-textures", "3d-layered")), "the tests lost and the one running when the app closed are left out: " + skip);
        check("0".equals(fixes.get("DM_VK_DMAHEAP")) && "keep".equals(fixes.get("DM_VK_INT_TEX")) && !fixes.containsKey("DM_VK_BC_GPU") && !fixes.containsKey("DM_VK_CLEAR_BY_PASS")
              && !fixes.containsKey("DM_VK_NO_LAYERED_3D") && fixes.size() == 2,
              "a crash's setting and a failed test's are kept; a hang's and the test the app closed in get none: " + fixes);
        check(crashed.size() == 3 && crashed.get(1).contains("hung") && crashed.get(2).contains("the app closed"), "each one named for the report: " + crashed);
        // a journal whose last attempt ended (done): nothing open
        skip.clear(); crashed.clear(); fixes.clear();
        DriverCheck.resume("attempt 0 skip \nbegin 3d-layered DM_VK_NO_LAYERED_3D=1\nresult 3d-layered PASS ok\ndone 12 0 0 1\n", skip, fixes, crashed);
        check(skip.isEmpty() && fixes.isEmpty() && crashed.isEmpty(), "a finished attempt: nothing left out");
        // a process that ended outside any test, and a test that ended in the process (its own result line), before the app closed
        skip.clear(); crashed.clear(); fixes.clear();
        DriverCheck.resume("attempt 0 skip \nbegin float-controls\nresult float-controls PASS ok\nlost - crashed\nattempt 1 skip \nprobe 1\n", skip, fixes, crashed);
        check(skip.isEmpty() && fixes.isEmpty(), "a lost process outside any test, then the app closed before a test began: nothing left out");
        System.out.println(checks + " checks, " + fails + " failed");
        System.exit(fails == 0 ? 0 : 1);
    }
}
