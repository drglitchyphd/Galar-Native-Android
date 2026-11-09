// FORK: the touch-control editor's bar: Save, Reset and Cancel at the top of the screen while the editor is on (touch_ui.c), with the layout being edited named
// (each screen and orientation has its own: touch_layout_<profile>.txt). It closes itself when the editor ends any other way (holding − for 2 s).
// Dragged by its label (⠿) out of the way of the controls under it; where it was left is kept per profile (SharedPreferences "fork", bar_<profile>).
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
        TextView label = new TextView(a); label.setTextColor(0xFFF2F3F5); label.setTextSize(14); label.setPadding(p / 2, p / 2, p, p / 2);
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
        final android.content.SharedPreferences sp = a.getSharedPreferences("fork", 0);
        final String[] prof = { profileNow() };
        final int[] pos = new int[2];
        root.post(() -> { try {
            l.measure(View.MeasureSpec.UNSPECIFIED, View.MeasureSpec.UNSPECIFIED);
            int dx = (root.getWidth() - l.getMeasuredWidth()) / 2, dy = (int) (12 * d);
            pos[0] = sp.getInt("bar_" + prof[0] + "_x", dx); pos[1] = sp.getInt("bar_" + prof[0] + "_y", dy);
            pw.showAtLocation(root, Gravity.TOP | Gravity.START, pos[0], pos[1]); } catch (RuntimeException e) { pw = null; } });
        final float[] grab = new float[4];
        label.setOnTouchListener((v, ev) -> {   // drag the bar by its label
            if (pw == null) return false;
            switch (ev.getActionMasked()) {
                case android.view.MotionEvent.ACTION_DOWN: grab[0] = ev.getRawX(); grab[1] = ev.getRawY(); grab[2] = pos[0]; grab[3] = pos[1]; return true;
                case android.view.MotionEvent.ACTION_MOVE:
                    pos[0] = Math.max(0, Math.min(root.getWidth() - l.getWidth(), (int) (grab[2] + ev.getRawX() - grab[0])));
                    pos[1] = Math.max(0, Math.min(root.getHeight() - l.getHeight(), (int) (grab[3] + ev.getRawY() - grab[1])));
                    pw.update(pos[0], pos[1], -1, -1); return true;
                case android.view.MotionEvent.ACTION_UP: case android.view.MotionEvent.ACTION_CANCEL:
                    sp.edit().putInt("bar_" + prof[0] + "_x", pos[0]).putInt("bar_" + prof[0] + "_y", pos[1]).apply(); return true;
                default: return true;
            }
        });
        Handler h = new Handler(Looper.getMainLooper());
        h.post(new Runnable() { @Override public void run() {
            if (pw == null) return;
            boolean on; String pf; try { on = nativeEditing(); pf = nativeProfile(); } catch (UnsatisfiedLinkError e) { on = false; pf = ""; }
            if (!on) { close(); return; }
            label.setText("⠿  " + T.t(a, "Layout: %s", profileName(a, pf)));   // (⠿: drag here)
            h.postDelayed(this, 300); } });
    }
    private static String profileNow() { try { return nativeProfile(); } catch (UnsatisfiedLinkError e) { return ""; } }
    private static void close() { if (pw != null) { try { pw.dismiss(); } catch (RuntimeException e) { } pw = null; } }
}
