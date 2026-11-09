// FORK: the touch-control editor's bar: Save, Reset and Cancel at the top of the screen while the editor is on (touch_ui.c), with the layout being edited named
// (each screen and orientation has its own: touch_layout_<profile>.txt). It closes itself when the editor ends any other way (holding − for 2 s).
package com.nxnative;

import android.app.Activity;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.PopupWindow;
import android.widget.TextView;

final class ForkTouch {
    private ForkTouch() { }
    private static native void nativeCmd(int cmd);   // 0 save and leave, 1 reset, 2 leave without saving
    private static native boolean nativeEditing();
    private static native String nativeProfile();
    private static PopupWindow pw;

    static String profileName(Activity a, String p) {
        switch (p) {
            case "outer_landscape": return T.t(a, "Outer screen");
            case "inner_landscape": return T.t(a, "Inner screen, landscape");
            case "inner_portrait": return T.t(a, "Inner screen, portrait");
            case "outer_portrait": return T.t(a, "Outer screen, portrait");
            default: return p;
        }
    }
    static void bar(Activity a) { a.runOnUiThread(() -> show(a)); }
    private static void show(Activity a) {
        if (pw != null && pw.isShowing()) return;
        float d = a.getResources().getDisplayMetrics().density;
        LinearLayout l = new LinearLayout(a); l.setOrientation(LinearLayout.HORIZONTAL); l.setGravity(Gravity.CENTER_VERTICAL);
        int p = (int) (10 * d); l.setPadding(p, p / 2, p, p / 2);
        GradientDrawable bg = new GradientDrawable(); bg.setColor(0xE0121419); bg.setCornerRadius(18 * d); l.setBackground(bg);
        TextView label = new TextView(a); label.setTextColor(0xFFF2F3F5); label.setTextSize(14); label.setPadding(p / 2, 0, p, 0);
        l.addView(label);
        String[] names = { T.t(a, "Save"), T.t(a, "Reset"), T.t(a, "Cancel") }; int[] colors = { 0xFF2E9BFF, 0xFF3A3F4A, 0xFF3A3F4A };
        for (int i = 0; i < 3; i++) { final int cmd = i; Button b = new Button(a); b.setText(names[i]); b.setAllCaps(false); b.setTextColor(0xFFFFFFFF);
            GradientDrawable bb = new GradientDrawable(); bb.setColor(colors[i]); bb.setCornerRadius(14 * d); b.setBackground(bb);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, (int) (40 * d)); lp.leftMargin = p / 2; b.setLayoutParams(lp);
            b.setPadding(p * 2, 0, p * 2, 0);
            b.setOnClickListener(v -> { try { nativeCmd(cmd); } catch (UnsatisfiedLinkError e) { } if (cmd != 1) close(); });
            l.addView(b); }
        pw = new PopupWindow(l, LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT, false);
        pw.setTouchable(true); pw.setOutsideTouchable(false); pw.setFocusable(false);   // touches outside go on to the editor underneath
        View root = a.getWindow().getDecorView();
        root.post(() -> { try { pw.showAtLocation(root, Gravity.TOP | Gravity.CENTER_HORIZONTAL, 0, (int) (12 * d)); } catch (RuntimeException e) { pw = null; } });
        Handler h = new Handler(Looper.getMainLooper());
        h.post(new Runnable() { @Override public void run() {
            if (pw == null) return;
            boolean on; String prof; try { on = nativeEditing(); prof = nativeProfile(); } catch (UnsatisfiedLinkError e) { on = false; prof = ""; }
            if (!on) { close(); return; }
            label.setText(T.t(a, "Layout: %s", profileName(a, prof)));
            h.postDelayed(this, 300); } });
    }
    private static void close() { if (pw != null) { try { pw.dismiss(); } catch (RuntimeException e) { } pw = null; } }
}
