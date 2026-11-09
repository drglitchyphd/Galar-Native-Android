// FORK: "Restart now" in the in-game menu (GameMenu: settings that apply from the next start). An app cannot start itself again from its own process, so this
// screen runs in a process of its own (":restart", build_apk.py, fork builds only): it waits half a second for the game to put its caches away (the game saves
// its pipeline cache when it is paused), ends the game's process, waits until it is gone, and opens the app again (a one-game build goes straight into the game).
package com.nxnative;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import java.io.File;

public final class ForkRestartActivity extends Activity {
    // GameMenu: from the game's process
    static void restart(Activity a) {
        Log.i(Fork.TAG, "[FORK] restart asked from the menu");
        a.startActivity(new Intent(a, ForkRestartActivity.class).putExtra("pid", android.os.Process.myPid()).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_NO_ANIMATION));
    }
    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        final int pid = getIntent().getIntExtra("pid", -1);
        final Handler h = new Handler(Looper.getMainLooper());
        h.postDelayed(() -> {
            if (pid > 0) android.os.Process.killProcess(pid);
            h.postDelayed(new Runnable() { int n;
                @Override public void run() {
                    if (pid > 0 && new File("/proc/" + pid).exists() && n++ < 30) { h.postDelayed(this, 100); return; }   // (up to 3 s for it to go)
                    startActivity(new Intent(ForkRestartActivity.this, SetupActivity.class).setAction(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                                  .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK));
                    finish();
                    h.postDelayed(() -> android.os.Process.killProcess(android.os.Process.myPid()), 300);
                } }, 200);
        }, 500);
    }
}
