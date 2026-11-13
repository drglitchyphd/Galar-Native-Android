// FORK v50: a power readout. Every 5 s the battery's current (BatteryManager CURRENT_NOW) and voltage are sampled; each minute the average goes into the game's log
// ([FORK] power: watts, mA, V, battery temperature, level, charging, frame generation paused or not), and the latest watts show in the performance overlay. Discharging
// only is meaningful (on the charger the current is the charger's). Read-only: changes nothing. For A / B tests of settings against battery drain and heat.
package com.nxnative;

import android.app.Activity;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.Handler;
import android.os.Looper;

final class ForkPower {
    private static native void nativeForkLog(String s);   // android_main.c: a line into the game's log
    private static native int nativeFgPaused();             // android_main.c: 1 while frame generation is paused on a still screen
    private static Handler h;
    private static double sumW, sumMa, sumV; private static int n;
    private static volatile double lastW = -1;
    static double watts() { return lastW; }

    static void start(Activity a) {
        if (h != null) return;
        h = new Handler(Looper.getMainLooper());
        h.post(new Runnable() { public void run() { sample(a); h.postDelayed(this, 5000); } });
    }
    private static void sample(Activity a) {
        try {
            BatteryManager bm = a.getSystemService(BatteryManager.class); if (bm == null) return;
            Intent b = a.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED)); if (b == null) return;
            long cur = bm.getIntProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW);   // microamperes on most phones; some report milliamperes
            if (cur == Integer.MIN_VALUE || cur == 0) return;
            double ma = Math.abs(cur) > 20000 ? Math.abs(cur) / 1000.0 : Math.abs(cur);
            double v = b.getIntExtra(BatteryManager.EXTRA_VOLTAGE, 0) / 1000.0; if (v <= 0) v = 3.9;
            double w = ma * v / 1000.0; lastW = w;
            sumW += w; sumMa += ma; sumV += v; n++;
            if (n >= 12) {
                float temp = b.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) / 10f;
                int level = b.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) * 100 / Math.max(1, b.getIntExtra(BatteryManager.EXTRA_SCALE, 100));
                int st = b.getIntExtra(BatteryManager.EXTRA_STATUS, -1); boolean chg = st == BatteryManager.BATTERY_STATUS_CHARGING || st == BatteryManager.BATTERY_STATUS_FULL;
                int fp = 0; try { fp = nativeFgPaused(); } catch (UnsatisfiedLinkError e) { }
                String s = String.format(java.util.Locale.ROOT, "[FORK] power: %.2f W (%.0f mA at %.2f V) over the last minute, battery %.1f °C, %d %%%s%s",
                    sumW / n, sumMa / n, sumV / n, temp, level, chg ? ", CHARGING (not the game's draw)" : ", discharging", fp == 1 ? ", frame generation paused (still screen)" : "");
                try { nativeForkLog(s); } catch (UnsatisfiedLinkError e) { }
                sumW = sumMa = sumV = 0; n = 0;
            }
        } catch (RuntimeException e) { }
    }
}
