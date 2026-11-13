// The in-game settings: one panel that slides in from the right over the dimmed game (opened by the touch controls' ☰ button, touch_ui.c; by holding − and + together on
// a controller for 1 s; or by holding − for 3 s, which opens it on the controller buttons: android_main.c front_open_menu). Tabs down its left side, each setting a card with
// its choices as chips and what the chosen one does under them; mods, the graphics driver, the controller buttons and the online room are pages inside the panel. Nothing
// pops up over the game: a setting that applies from the next start says so on its card, other news goes to the panel's status line. Every change is written to env.txt
// (GameSettings); the picture, the touch controls, the controller buttons, rumble and the performance overlay change at once (nativeSetting). Every text goes through T.t
package com.nxnative;

import android.app.Activity;
import android.app.Dialog;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.text.InputType;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.EditText;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

public final class GameMenu {
    private GameMenu() { }
    private static native boolean nativeSetting(String key, String value);
    private static native void nativeEditTouch();
    private static native String nativeLsfgImport(byte[] dll, String dir);   // lsfg_import.cpp: null when done, else why not

    // ---------- look ----------
    private static final int BG = 0xF0121419, CARD = 0xFF1C1F26, CARD_LINE = 0xFF2A2E37, TEXT = 0xFFF2F3F5, SUB = 0xFF9AA0AB, CHIP = 0xFF262A33, WARN = 0xFFFFB74D;
    private static int accent(Activity a) {   // the game's colour: Sword blue, Shield magenta, Violet purple, Scarlet red
        String id = GameSettings.game(a)[0].toUpperCase(java.util.Locale.ROOT);
        return id.startsWith("0100ABF") ? 0xFF2E9BFF : id.startsWith("01008DB") ? 0xFFE0306E : id.startsWith("01008F6") ? 0xFF9B5CFF : id.startsWith("0100A3D") ? 0xFFFF4D3D : 0xFF3DDC97;
    }
    private static int dp(Activity a, float v) { return Math.round(v * a.getResources().getDisplayMetrics().density); }
    private static GradientDrawable round(int color, float r, int stroke, int strokeColor) {
        GradientDrawable g = new GradientDrawable(); g.setColor(color); g.setCornerRadius(r); if (stroke > 0) g.setStroke(stroke, strokeColor); return g;
    }
    private static TextView text(Activity a, String s, float sp, int color, boolean bold) {
        TextView t = new TextView(a); t.setText(s); t.setTextSize(sp); t.setTextColor(color); if (bold) t.setTypeface(Typeface.DEFAULT_BOLD); return t;
    }
    private static String t(Activity a, String en, Object... args) { return T.t(a, en, args); }

    // ---------- the panel ----------
    private static Dialog dlg; private static Activity act; private static LinearLayout rail, body; private static ScrollView scroll; private static TextView status;
    private static String page = "display"; private static final Set<String> RESTART = new HashSet<>();   // settings changed that apply from the next start
    private static final String[][] TABS = { { "display", "Display" }, { "perf", "Performance" }, { "controls", "Controls" }, { "mods", "Mods" }, { "saves", "Saves" },
                                             { "online", "Online" }, { "system", "System" }, { "about", "About" } };

    // android_main.c: what 0 = the menu, 1 = the controller buttons
    public static void show(Activity a, int what) { if (what == 2) { ForkTouch.bar(a); return; }   // FORK: 2 = the touch editor's bar (touch_ui.c)
        a.runOnUiThread(() -> open(a, what == 1 ? "remap" : null)); }

    private static void open(Activity a, String start) {
        if (dlg != null && dlg.isShowing()) { if (start != null) go(start); return; }
        act = a; if (start != null) page = start; else if (page.equals("remap") || page.equals("driver") || page.equals("order")) page = "display";
        Dialog d = new Dialog(a); d.requestWindowFeature(Window.FEATURE_NO_TITLE);
        int sw = a.getResources().getDisplayMetrics().widthPixels, sh = a.getResources().getDisplayMetrics().heightPixels;
        LinearLayout panel = new LinearLayout(a); panel.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable pbg = new GradientDrawable(); pbg.setColor(BG); pbg.setCornerRadii(new float[] { dp(a, 22), dp(a, 22), 0, 0, 0, 0, dp(a, 22), dp(a, 22) }); panel.setBackground(pbg);
        // header: the game, a close button
        LinearLayout head = new LinearLayout(a); head.setGravity(Gravity.CENTER_VERTICAL); head.setPadding(dp(a, 20), dp(a, 14), dp(a, 12), dp(a, 8));
        View dot = new View(a); dot.setBackground(round(accent(a), dp(a, 6), 0, 0)); head.addView(dot, new LinearLayout.LayoutParams(dp(a, 10), dp(a, 10)));
        TextView title = text(a, GameSettings.game(a)[1], 19, TEXT, true); title.setPadding(dp(a, 10), 0, 0, 0); head.addView(title, new LinearLayout.LayoutParams(0, -2, 1));
        TextView close = text(a, "✕", 20, SUB, false); close.setPadding(dp(a, 14), dp(a, 6), dp(a, 14), dp(a, 6)); close.setOnClickListener(v -> d.dismiss()); head.addView(close);
        panel.addView(head);
        LinearLayout mid = new LinearLayout(a); mid.setOrientation(LinearLayout.HORIZONTAL);
        rail = new LinearLayout(a); rail.setOrientation(LinearLayout.VERTICAL); rail.setPadding(dp(a, 10), dp(a, 4), dp(a, 6), dp(a, 10));
        ScrollView rs = new ScrollView(a); rs.addView(rail); mid.addView(rs, new LinearLayout.LayoutParams(dp(a, 150), -1));
        scroll = new ScrollView(a); body = new LinearLayout(a); body.setOrientation(LinearLayout.VERTICAL); body.setPadding(dp(a, 8), dp(a, 4), dp(a, 18), dp(a, 18));
        scroll.addView(body); mid.addView(scroll, new LinearLayout.LayoutParams(0, -1, 1));
        panel.addView(mid, new LinearLayout.LayoutParams(-1, 0, 1));
        status = text(a, "", 13, SUB, false); status.setPadding(dp(a, 20), dp(a, 6), dp(a, 20), dp(a, 12)); status.setVisibility(View.GONE); panel.addView(status);
        d.setContentView(panel);
        Window w = d.getWindow();
        w.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        w.setLayout(Math.min(sw, Math.max(dp(a, 560), (int) (sw * 0.6f))), sh); w.setGravity(Gravity.END | Gravity.FILL_VERTICAL);
        w.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND); w.setDimAmount(0.45f);
        w.setWindowAnimations(android.R.style.Animation_InputMethod);   // (slides in from the edge)
        hideBars(w);
        d.setOnKeyListener((x, kc, e) -> remapKey(kc, e));
        d.setOnDismissListener(x -> { dlg = null; capture = null; });   // FORK: RESTART kept until the app restarts (the banner on top of every page, Restart now)
        dlg = d; d.show(); hideBars(w);
        go(page);
    }
    @SuppressWarnings("deprecation")
    private static void hideBars(Window w) {   // the game stays full screen behind the panel (Immersive.java's flags on the panel's own window)
        w.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN
            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }
    private static void say(String s) {   // the status line (in place of toasts over the game)
        if (status == null) return;
        status.setText(s); status.setVisibility(s.isEmpty() ? View.GONE : View.VISIBLE);
    }
    private static void go(String p) {
        page = p; Activity a = act; if (a == null || body == null) return;
        rail.removeAllViews();
        String top = p.equals("remap") ? "controls" : p.equals("driver") ? "system" : p.equals("order") ? "mods" : p;
        for (String[] tb : TABS) {
            boolean on = tb[0].equals(top);
            TextView r = text(a, t(a, tb[1]), 15, on ? TEXT : SUB, on); r.setPadding(dp(a, 14), dp(a, 11), dp(a, 10), dp(a, 11));
            r.setBackground(on ? round((accent(a) & 0x00FFFFFF) | 0x38000000, dp(a, 12), 0, 0) : null);
            r.setOnClickListener(v -> { say(""); capture = null; go(tb[0]); });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2); lp.bottomMargin = dp(a, 4); rail.addView(r, lp);
        }
        body.removeAllViews();
        if (!RESTART.isEmpty() && Fork.shared(a)) {   // FORK: changes waiting for the next start: say so on top, with a way to restart now
            LinearLayout c = new LinearLayout(a); c.setOrientation(LinearLayout.HORIZONTAL); c.setGravity(android.view.Gravity.CENTER_VERTICAL);
            c.setPadding(dp(a, 14), dp(a, 10), dp(a, 10), dp(a, 10)); c.setBackground(round(0x33FFB74D, dp(a, 14), dp(a, 1), WARN));
            TextView tx = text(a, t(a, "Some changes apply after a restart."), 14, TEXT, false); c.addView(tx, new LinearLayout.LayoutParams(0, -2, 1));
            TextView bt = text(a, t(a, "Restart now"), 14, 0xFF000000, true); bt.setPadding(dp(a, 16), dp(a, 8), dp(a, 16), dp(a, 8)); bt.setBackground(round(WARN, dp(a, 12), 0, 0));
            bt.setOnClickListener(v -> confirm(a, t(a, "Restart the game now? Anything since your last save in the game is lost."), t(a, "OK"), () -> ForkRestartActivity.restart(a)));
            c.addView(bt);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2); lp.bottomMargin = dp(a, 12); body.addView(c, lp);
        }
        switch (p) {
            case "display": display(a); break;
            case "perf": perf(a); break;
            case "controls": controls(a); break;
            case "remap": remap(a); break;
            case "mods": mods(a); break;
            case "order": order(a); break;
            case "saves": saves(a); break;
            case "online": online(a); break;
            case "driver": driver(a); break;
            case "about": about(a); break;
            default: system(a);
        }
    }
    private static void redraw() { int y = scroll.getScrollY(); go(page); scroll.post(() -> scroll.scrollTo(0, y)); }   // (the same page, kept where it was scrolled)

    // ---------- building blocks ----------
    private static void heading(Activity a, String s) {
        TextView h = text(a, s, 13, SUB, true); h.setAllCaps(true); h.setLetterSpacing(0.08f); h.setPadding(dp(a, 6), dp(a, 14), 0, dp(a, 6)); body.addView(h);
    }
    private static LinearLayout card(Activity a) {
        LinearLayout c = new LinearLayout(a); c.setOrientation(LinearLayout.VERTICAL); c.setBackground(round(CARD, dp(a, 16), 1, CARD_LINE)); c.setPadding(dp(a, 16), dp(a, 12), dp(a, 16), dp(a, 14));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2); lp.bottomMargin = dp(a, 10); body.addView(c, lp); return c;
    }
    private static LinearLayout titleRow(Activity a, LinearLayout c, String title, String tag) {
        LinearLayout r = new LinearLayout(a); r.setGravity(Gravity.CENTER_VERTICAL);
        r.addView(text(a, title, 16, TEXT, true), new LinearLayout.LayoutParams(0, -2, 1));
        if (tag != null) { TextView tg = text(a, tag, 11, accent(a), true); tg.setBackground(round(0, dp(a, 10), dp(a, 1), accent(a))); tg.setPadding(dp(a, 8), dp(a, 2), dp(a, 8), dp(a, 2)); r.addView(tg); }
        c.addView(r); return r;
    }
    private static TextView pill(Activity a, String label, boolean on) {
        TextView p = text(a, label, 13, on ? Color.WHITE : SUB, true); p.setPadding(dp(a, 14), dp(a, 5), dp(a, 14), dp(a, 5));
        p.setBackground(on ? round(accent(a), dp(a, 14), 0, 0) : round(CHIP, dp(a, 14), 1, CARD_LINE)); return p;
    }
    // a choice: { value, chip label, what it does }; the chosen one's line under the chips. Applied at once when the engine can (nativeSetting), else tagged "Applies next start"
    // FORK v46: one choice that sets several settings at once (Smoothness, Leaving the game). keys: the settings; each option: { label, description, value per key }.
    //   The chip shown as chosen is the first option whose values all match; none matching: the first
    private static void combo(Activity a, String title, String[] keys, String[][] opts) {
        int cur = 0; for (int i = 0; i < opts.length && cur == 0; i++) { boolean m = true; for (int k = 0; k < keys.length; k++) if (!opts[i][2 + k].equals(GameSettings.get(a, keys[k]))) m = false; if (m) cur = i; }
        boolean restart = false; for (String k : keys) restart |= RESTART.contains(k);
        LinearLayout c = card(a); titleRow(a, c, title, restart ? t(a, "Applies next start") : null);
        HorizontalScrollView hs = new HorizontalScrollView(a); hs.setHorizontalScrollBarEnabled(false);
        LinearLayout chips = new LinearLayout(a); chips.setPadding(0, dp(a, 10), 0, dp(a, 6)); hs.addView(chips); c.addView(hs);
        TextView desc = text(a, "", 13, SUB, false); c.addView(desc);
        for (int i = 0; i < opts.length; i++) {
            String[] o = opts[i]; boolean on = i == cur;
            TextView ch = text(a, o[0], 14, on ? Color.WHITE : TEXT, on); ch.setPadding(dp(a, 16), dp(a, 8), dp(a, 16), dp(a, 8));
            ch.setBackground(on ? round(accent(a), dp(a, 18), 0, 0) : round(CHIP, dp(a, 18), 1, CARD_LINE));
            ch.setOnClickListener(v -> { for (int k = 0; k < keys.length; k++) if (!o[2 + k].equals(GameSettings.get(a, keys[k]))) set(a, keys[k], o[2 + k]); redraw(); });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, -2); lp.rightMargin = dp(a, 8); chips.addView(ch, lp);
            if (on) desc.setText(o[1]);
        }
    }
    private static void choice(Activity a, String key, String title, String[][] opts) {
        LinearLayout c = card(a); String cur = GameSettings.get(a, key);
        titleRow(a, c, title, RESTART.contains(key) ? t(a, "Applies next start") : null);
        HorizontalScrollView hs = new HorizontalScrollView(a); hs.setHorizontalScrollBarEnabled(false);
        LinearLayout chips = new LinearLayout(a); chips.setPadding(0, dp(a, 10), 0, dp(a, 6)); hs.addView(chips); c.addView(hs);
        TextView desc = text(a, "", 13, SUB, false); c.addView(desc);
        boolean found = false; for (String[] o : opts) if (o[0].equals(cur)) found = true;
        for (String[] o : opts) {
            boolean on = o[0].equals(cur) || (!found && o == opts[0]);
            TextView ch = text(a, o[1], 14, on ? Color.WHITE : TEXT, on); ch.setPadding(dp(a, 16), dp(a, 8), dp(a, 16), dp(a, 8));
            ch.setBackground(on ? round(accent(a), dp(a, 18), 0, 0) : round(CHIP, dp(a, 18), 1, CARD_LINE));
            ch.setOnClickListener(v -> { if (!o[0].equals(GameSettings.get(a, key))) set(a, key, o[0]); redraw(); });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, -2); lp.rightMargin = dp(a, 8); chips.addView(ch, lp);
            if (on) desc.setText(o.length > 2 ? o[2] : "");
        }
        if (desc.getText().length() == 0) desc.setVisibility(View.GONE);
    }
    private static void action(Activity a, String title, String sub, Runnable r) {
        LinearLayout c = card(a); LinearLayout row = titleRow(a, c, title, null);
        row.addView(text(a, "›", 22, SUB, false));
        if (sub != null && !sub.isEmpty()) { TextView s = text(a, sub, 13, SUB, false); s.setPadding(0, dp(a, 4), 0, 0); c.addView(s); }
        c.setOnClickListener(v -> r.run());
    }
    private static void slider(Activity a, String title, String key, int min, int max, int def, java.util.function.IntFunction<String> fmt) {
        LinearLayout c = card(a); LinearLayout row = titleRow(a, c, title, null);
        int v; try { v = Integer.parseInt(GameSettings.get(a, key)); } catch (NumberFormatException e) { v = def; }
        v = Math.max(min, Math.min(max, v));
        TextView val = text(a, fmt.apply(v), 14, 0xFFF2F2F5, true); row.addView(val);
        SeekBar sb = new SeekBar(a); sb.setMax(max - min); sb.setProgress(v - min); sb.setPadding(dp(a, 10), dp(a, 14), dp(a, 10), dp(a, 4));
        // neutral, not the game's accent all over (a purple bar and knob on Paldea were too much): a thin rounded grey track, the filled part near-white, a white knob with a soft ring
        android.graphics.drawable.GradientDrawable track = new android.graphics.drawable.GradientDrawable(); track.setColor(0x33FFFFFF); track.setCornerRadius(dp(a, 3)); track.setSize(0, dp(a, 4));
        android.graphics.drawable.GradientDrawable fill = new android.graphics.drawable.GradientDrawable(); fill.setColor(0xFFE6E6EB); fill.setCornerRadius(dp(a, 3)); fill.setSize(0, dp(a, 4));
        android.graphics.drawable.LayerDrawable bar = new android.graphics.drawable.LayerDrawable(new android.graphics.drawable.Drawable[] { track,
            new android.graphics.drawable.ClipDrawable(fill, android.view.Gravity.START, android.graphics.drawable.ClipDrawable.HORIZONTAL) });
        bar.setId(0, android.R.id.background); bar.setId(1, android.R.id.progress);
        bar.setLayerHeight(0, dp(a, 4)); bar.setLayerHeight(1, dp(a, 4)); bar.setLayerGravity(0, android.view.Gravity.CENTER_VERTICAL); bar.setLayerGravity(1, android.view.Gravity.CENTER_VERTICAL);
        sb.setProgressDrawable(bar); sb.setMinHeight(dp(a, 4)); sb.setMaxHeight(dp(a, 4));
        android.graphics.drawable.GradientDrawable knob = new android.graphics.drawable.GradientDrawable(); knob.setShape(android.graphics.drawable.GradientDrawable.OVAL);
        knob.setColor(0xFFFFFFFF); knob.setStroke(dp(a, 3), 0x40FFFFFF); knob.setSize(dp(a, 20), dp(a, 20));
        sb.setThumb(knob); sb.setThumbOffset(dp(a, 10)); sb.setSplitTrack(false); sb.setBackground(null);
        sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar b, int p, boolean user) { val.setText(fmt.apply(p + min)); if (user) set(a, key, String.valueOf(p + min)); }
            public void onStartTrackingTouch(SeekBar b) { }
            public void onStopTrackingTouch(SeekBar b) { }
        });
        c.addView(sb);
    }
    private static void note(Activity a, String s) { TextView n = text(a, s, 13, SUB, false); n.setPadding(dp(a, 6), dp(a, 2), dp(a, 6), dp(a, 10)); body.addView(n); }
    private static TextView button(Activity a, LinearLayout parent, String label, boolean primary, Runnable r) {
        TextView b = text(a, label, 14, primary ? Color.WHITE : TEXT, true); b.setGravity(Gravity.CENTER); b.setPadding(dp(a, 16), dp(a, 9), dp(a, 16), dp(a, 9));
        b.setBackground(primary ? round(accent(a), dp(a, 18), 0, 0) : round(CHIP, dp(a, 18), 1, CARD_LINE)); b.setOnClickListener(v -> r.run());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, -2); lp.rightMargin = dp(a, 8); lp.topMargin = dp(a, 8); parent.addView(b, lp); return b;
    }
    private static EditText field(Activity a, LinearLayout parent, String hint) {
        EditText e = new EditText(a); e.setHint(hint); e.setSingleLine(); e.setTextColor(TEXT); e.setHintTextColor(SUB); e.setTextSize(15);
        e.setBackground(round(CHIP, dp(a, 12), 1, CARD_LINE)); e.setPadding(dp(a, 12), dp(a, 10), dp(a, 12), dp(a, 10));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2); lp.topMargin = dp(a, 8); parent.addView(e, lp); return e;
    }

    private static void set(Activity a, String key, String value) {
        String was = GameSettings.get(a, key);
        GameSettings.set(a, key, value);
        boolean now;
        if (a instanceof GameActivity && (key.equals("DM_FRAMEGEN") || key.equals("DM_FPS_TARGET"))) ((GameActivity) a).displayMode();   // (the screen's rate follows the pictures shown)
        if (key.equals("DM_PERF_OVERLAY")) { if (a instanceof GameActivity) ((GameActivity) a).perfOverlay(value.equals("on")); now = true; }
        else if (key.equals("DM_SUSTAINED")) { if (a instanceof GameActivity) ((GameActivity) a).sustained(); now = true; }
        else if (key.equals("DM_LANG")) now = true;
        else if (key.equals("DM_RUMBLE")) {   // (controller / phone: at once; off and back on: the game's motors, from the next start)
            now = !value.equals("off") && !was.equals("off");
            try { nativeSetting(key, value); } catch (UnsatisfiedLinkError e) { }   // (the touch buttons' click follows Rumble unless set apart: touch_ui.c)
        }
        else { try { now = nativeSetting(key, value); } catch (UnsatisfiedLinkError e) { now = false; } }
        if (!now) RESTART.add(key);
    }

    // ---------- the pages ----------
    private static void display(Activity a) {
        heading(a, t(a, "Screen"));
        choice(a, "DM_ASPECT", t(a, "Picture"), new String[][] { { "", "16:9", t(a, "16:9, as the game is made") }, { "fill", t(a, "Fill"), t(a, "Fill the screen (stretched)") },
                                                                    { "20:9", "20:9", t(a, "20:9, for a widescreen mod") },
                                                                    { "fix", t(a, "Fill, corrected"), t(a, "Fill the screen with the 3D and the menus kept in shape (experimental; no mod needed)") } });   // FORK
        choice(a, "DM_FORK_PORTRAIT", t(a, "Portrait on the inner screen"), new String[][] { { "", t(a, "Off"), t(a, "Off: always landscape") },   // FORK: Fork.orientation
            { "on", t(a, "On"), t(a, "On: unfolded, the game turns with the phone; upright, the picture is at the top and the controls below it (their own layout)") } });
        String rec = GameSettings.profile(a)[6].equals("sgsr") ? "SGSR" : t(a, "Sharp");
        choice(a, "DM_SCALER", t(a, "Upscaling"), new String[][] { { "", t(a, "Auto"), t(a, "Auto: this phone gets %s", rec) },
            { "sharp", t(a, "Sharp"), t(a, "Sharp: the picture upscaled with sharpening (lighter settings keep their detail)") },
            { "sgsr", "SGSR", t(a, "Snapdragon Game Super Resolution (Qualcomm's upscaler)") }, { "smooth", t(a, "Smooth"), t(a, "Smooth: a plain stretch") } });
        // FORK v46: frame generation moved into Speed → Smoothness (with the frame rate); upstream's own frame generation and its Lossless.dll import are not offered
        // HD textures: the player's own retexture mods (Mods), on Auto only on phones with the memory for them (GameSettings.profile, android_main.c mod_root);
        //   offered once such a mod is in the mods folder
        if (GameSettings.hasTexturePack(a)) {
            boolean hdAuto = GameSettings.profile(a)[8].equals("on");
            choice(a, "DM_HD_TEXTURES", t(a, "HD textures"), new String[][] {
                { "", t(a, "Auto"), hdAuto ? t(a, "Auto: on for this phone") : t(a, "Auto: off for this phone (HD texture mods need 12 GB of memory)") },
                { "on", t(a, "On"), t(a, "On: your HD texture mods load (more memory, slower area loads)") },
                { "off", t(a, "Off"), t(a, "Off: the game's own textures (your HD texture mods stay installed)") } });
        }
        heading(a, t(a, "Overlay"));
        choice(a, "DM_PERF_OVERLAY", t(a, "Performance overlay"), new String[][] { { "", t(a, "Off"), "" }, { "on", t(a, "On"), t(a, "On: fps, temperature, GPU, memory, battery") } });
    }
    private static void perf(Activity a) {
        String[] p = GameSettings.profile(a);
        note(a, t(a, "This phone: %s", p[1]));
        heading(a, t(a, "Speed"));
        String g = t(a, p[2].equals("high") ? "High" : p[2].equals("auto") ? "Adaptive" : "Low");
        String[][] gfx = { { "", t(a, "Auto"), t(a, "Auto: this phone gets %s", g) }, { "high", t(a, "High"), t(a, "High: TV mode, 1080p, full effects") },
            { "low", t(a, "Low"), t(a, "Low: handheld mode, 720p, lighter") }, { "auto", t(a, "Adaptive"), t(a, "Adaptive: High, a lighter picture while the phone is hot or slow") },
            // Lowest: handheld mode with the game's 3D render scale capped (Pokémon Sword / Shield: the engine's hle_resscale.c; Scarlet / Violet have no such scale)
            { "lowest", t(a, "Lowest"), t(a, "Lowest: handheld mode, 3D at 960x540, coolest") } };
        choice(a, "DM_GRAPHICS", t(a, "Graphics"), GameSettings.paldea(a) ? java.util.Arrays.copyOf(gfx, 4) : gfx);
        // FORK v46: Smoothness: the frame rate and frame generation as one choice (60 fps mode and frame generation do not go together: with both the game held 30).
        //   Writes DM_FPS_TARGET, DM_FORK_FG (gfx/nvn/fork_fg.h, the player's Lossless.dll) and clears upstream's DM_FRAMEGEN
        boolean dll = Fork.lsfgDll(a).isFile();
        java.util.List<String[]> sm = new java.util.ArrayList<>();
        // v47 (Steven Nov 13): 60 fps scrapped (the Fold held 30 for 1569 of 1800 frames at 60), Lossless 3x scrapped ("runs weird"): 30, with or without Lossless 2x
        sm.add(new String[] { "30 fps", t(a, "30 fps, as the game is made"), "30", "", "" });
        if (dll) sm.add(new String[] { t(a, "60 on screen (Lossless 2x)"), t(a, "The game at 30, a generated frame between every two: 60 on screen. Animations keep their speed. From the next start"), "30", "2x", "" });
        combo(a, t(a, "Smoothness"), new String[] { "DM_FPS_TARGET", "DM_FORK_FG", "DM_FRAMEGEN" }, sm.toArray(new String[0][]));
        if (!dll) note(a, t(a, "Lossless frame generation: choose your Lossless.dll on the setup screen (System, Setup screen, Frame generation)."));
        // FORK: frame pacing slack (nvn_vk.c pace_time, DM_FORK_PACE_MARGIN); the pacer counts in the screen's own top rate (v46: automatic, nvn_vk.c fork_refresh)
        choice(a, "DM_FORK_PACE_MARGIN", t(a, "Frame pacing"), new String[][] { { "", t(a, "Stock"), t(a, "Stock: each frame aimed one refresh ahead (least delay)") },
            { "2", t(a, "Smooth"), t(a, "Smooth: two refreshes ahead, frames a little late still show on time (about 4 ms more delay)") },
            { "3", t(a, "Smoothest"), t(a, "Smoothest: three refreshes ahead (about 8 ms more delay)") } });
        String fgv = GameSettings.get(a, "DM_FORK_FG");
        if (dll && fgv.equals("2x")) {   // the fork's frame generation's own settings
            slider(a, t(a, "Frame generation: flow scale"), "DM_FORK_FG_FLOW", 25, 100, 75, v -> v + " %");
            choice(a, "DM_FORK_FG_PERF", t(a, "Frame generation: model"), new String[][] { { "", t(a, "Performance"), t(a, "Performance: lighter on the GPU (recommended on phones)") },
                { "off", t(a, "Quality"), t(a, "Quality: the full model, heavier") } });
        }
        if (GameActivity.sustainedSupported(a)) choice(a, "DM_SUSTAINED", t(a, "Sustained performance"), new String[][] {
            { "", t(a, "Off"), t(a, "Off: the phone's full speed, which drops when it gets hot") }, { "on", t(a, "On"), t(a, "On: a speed the phone can hold for hours, fewer slowdowns when hot") } });
        heading(a, t(a, "Memory"));
        choice(a, "DM_VK_TEX_QUALITY", t(a, "Textures"), new String[][] { { "", t(a, "Auto"), t(a, "Auto: this phone gets %s", t(a, p[3].equals("half") ? "Half" : "Full")) },
            { "full", t(a, "Full"), t(a, "Full: the game's textures as they are") }, { "half", t(a, "Half"), t(a, "Half: large textures at half size, less memory and GPU work") } });
        choice(a, "DM_VK_BC_TRANSCODE", t(a, "Texture format"), new String[][] { { "", t(a, "Auto"), t(a, "Auto: compact on GPUs without BC (Mali, Xclipse, PowerVR), converted once and kept") },
            { "off", t(a, "Full decode"), t(a, "Full decode: more memory, no conversion (Snapdragon is not affected either way)") } });
    }
    private static void controls(Activity a) {
        heading(a, t(a, "Controller"));
        choice(a, "DM_PAD_LAYOUT", t(a, "Controller buttons"), new String[][] { { "", t(a, "By position"), t(a, "By position, as on a Switch pad") },
                                                                                 { "labels", t(a, "By label"), t(a, "By label (the button marked A is A)") } });
        action(a, t(a, "Remap controller buttons"), t(a, "Or hold − for 3 s on a controller. Hold − and + together for 1 s to open this menu."), () -> go("remap"));
        choice(a, "DM_RUMBLE", t(a, "Rumble"), new String[][] { { "", t(a, "On"), t(a, "On: the controller's motor, else the phone's") }, { "pad", t(a, "Controllers only"), "" },
                                                                 { "off", t(a, "Off"), "" } });
        // the right stick / camera, touch and controller alike (hle_hid.c rsens)
        choice(a, "DM_RSTICK_SENS", t(a, "Camera sensitivity"), new String[][] { { "50", "50 %", t(a, "Slower camera: the right stick and the touch camera turn less") }, { "75", "75 %", "" },
            { "", "100 %", t(a, "As on the Switch") }, { "125", "125 %", "" }, { "150", "150 %", t(a, "Faster camera: full speed with less of the stick") }, { "200", "200 %", "" } });
        // FORK v47 (from Arceus Native): movement stick sensitivity (hle_hid.c lsens) and the touch camera's own settings (touch_ui.c cam_pct / cam_stick / rs_off)
        slider(a, t(a, "Movement stick sensitivity"), "DM_LSTICK_SENS", 100, 200, 100, v -> v == 100 ? t(a, "100 % (as on the Switch)") : v + " %");
        slider(a, t(a, "Touch camera sensitivity"), "DM_TOUCH_CAM_SENS", 25, 300, 100, v -> v + " %");
        if (!"float".equals(GameSettings.get(a, "DM_FORK_TOUCH")))
            choice(a, "DM_TOUCH_CAMERA", t(a, "Touch camera"), new String[][] { { "", t(a, "Drag"), t(a, "Drag: the camera follows the finger's movement and stops when it stops") },
                { "stick", t(a, "Stick"), t(a, "Stick: where the finger lands is the centre; hold it away to keep turning, faster the further (hold-to-lock-on is off)") } });
        choice(a, "DM_TOUCH_RSTICK", t(a, "Right stick on screen"), new String[][] { { "", t(a, "Show"), "" }, { "off", t(a, "Hide"), t(a, "Hide: the camera is turned with a finger only") } });
        heading(a, t(a, "Touch controls"));
        // L3 / R3 (pressing the sticks, touch_ui.c): off by default, Sword and Shield hardly use them (the left stick whistles in the Wild Area)
        choice(a, "DM_TOUCH_L3R3", t(a, "L3 / R3 buttons"), new String[][] { { "", t(a, "Off"), t(a, "Off: no L3 / R3 on screen (a controller's stick buttons still work)") },
                                                                             { "on", t(a, "On"), t(a, "On: L3 and R3 above the sticks (move them with Move the touch controls)") } });
        // the short click when a touch button is pressed (touch_ui.c): by default it goes with Rumble, so Rumble Off silences the phone entirely (#114)
        choice(a, "DM_TOUCH_HAPTIC", t(a, "Vibrate on press"), new String[][] { { "", t(a, "Like rumble"), t(a, "Like rumble: off when Rumble is Off") },
                                                                                { "on", t(a, "On"), "" }, { "off", t(a, "Off"), "" } });
        slider(a, t(a, "Opacity"), "DM_TOUCH_OPACITY", 5, 100, 100, v -> v + " %");   // FORK: from 5 % (stock 20)
        slider(a, t(a, "Fade after"), "DM_TOUCH_FADE", 0, 10, 3, v -> v == 0 ? t(a, "never fade") : t(a, "%d s without a touch", v));
        // FORK v46: the fork's touch extras together (touch_ui.c, host/fork_ff.c)
        heading(a, t(a, "Fork extras"));
        // FORK: the control scheme (touch_ui.c g_float) and the sticks' dead zone
        choice(a, "DM_FORK_TOUCH", t(a, "Control scheme"), new String[][] { { "", t(a, "Stock"), t(a, "Stock: the stick on the left, drag on the right turns the camera") },
            { "float", t(a, "Floating sticks"), t(a, "Floating sticks: the left half is the movement stick and the right half the camera stick, each where your thumb lands; a quick tap anywhere that is not a button presses A, two fingers B") } });
        slider(a, t(a, "Stick dead zone"), "DM_FORK_DEADZONE", 0, 40, 12, v -> v + " %");
        // FORK: the controls' colour and overall size (touch_ui.c tint, g_tsize)
        choice(a, "DM_FORK_TOUCH_COLOR", t(a, "Colour"), new String[][] { { "", t(a, "Blue"), "" }, { "grey", t(a, "Grey"), "" }, { "white", t(a, "White"), "" },
            { "dark", t(a, "Dark"), "" }, { "red", t(a, "Red"), "" }, { "green", t(a, "Green"), "" }, { "purple", t(a, "Purple"), "" } });
        slider(a, t(a, "Size"), "DM_FORK_TOUCH_SIZE", 50, 150, 100, v -> v + " %");
        choice(a, "DM_FORK_FF", t(a, "Fast-forward"), new String[][] { { "", t(a, "Off"), "" },
            { "battle", t(a, "In battles"), t(a, "In battles: the ⏩ button shows during battles. Tap: on / off. Hold: fast while held") },
            { "all", t(a, "Anywhere"), t(a, "Anywhere: the ⏩ button is always there (walking, hatching, battles)") } });
        choice(a, "DM_FORK_FF_SPEED", t(a, "Fast-forward speed"), new String[][] { { "125", "1.25x", "" }, { "", "1.5x", "" }, { "200", "2x", "" }, { "300", "3x", "" } });
        choice(a, "DM_FORK_TURBO", t(a, "Turbo A"), new String[][] { { "", t(a, "Off"), "" },
            { "on", t(a, "On"), t(a, "On: hold the ⚡ button to press A again and again (text, buying, hatching)") } });
        slider(a, t(a, "Turbo speed"), "DM_FORK_TURBO_HZ", 3, 20, 10, v -> t(a, "%d presses a second", v));
        // FORK v46: battle controls as one switch (touch_ui.c battle_mode: unused buttons hidden, battle icons, the battle's own layout) and its two sliders.
        //   Set before v46 with the old separate settings: carried over once (the old keys cleared, so Same as elsewhere is off for good)
        if (GameSettings.get(a, "DM_FORK_BATTLE").isEmpty() && ("on".equals(GameSettings.get(a, "DM_FORK_BATTLE_HIDE")) || "on".equals(GameSettings.get(a, "DM_FORK_BATTLE_LAYOUT")))) {
            GameSettings.set(a, "DM_FORK_BATTLE", "on"); GameSettings.set(a, "DM_FORK_BATTLE_HIDE", ""); GameSettings.set(a, "DM_FORK_BATTLE_LAYOUT", ""); GameSettings.set(a, "DM_FORK_BATTLE_ICONS", ""); }
        // FORK v47: context controls (touch_ui.c ctx_hidden): each screen kind hides what it does not use; dialogue and menus keep a layout of their own.
        //   The ⊞ button brings every button back until the screen changes (the screen is a guess from what the game draws)
        choice(a, "DM_FORK_GESTURES", t(a, "Gesture shortcuts"), new String[][] { { "", t(a, "Off"), "" },   // FORK v47 (touch_ui.c gestures_event)
            { "on", t(a, "On"), t(a, "A quick three-finger tap opens this menu; two fingers swiped down together press X (the game's menu)") } });
        // FORK v48: the encounter panel (touch_ui.c enc_draw, host/fork_enc.c)
        choice(a, "DM_FORK_ENC", t(a, "Encounter info"), new String[][] { { "", t(a, "Off"), "" },
            { "on", t(a, "On"), t(a, "In wild battles: a panel with the Pokémon's species, shiny, nature, ability, IVs and EVs. Drag it to move it; tap it to fold it to a Poké Ball (gold star = shiny)") } });
        heading(a, t(a, "Context controls"));
        note(a, t(a, "The fork guesses the screen from what the game draws. If buttons you need are hidden, tap ⊞ (top centre) for every button until the screen changes. Arrange a screen's own layout with Move the touch controls while on it."));
        choice(a, "DM_FORK_CTX_DIALOGUE", t(a, "Dialogue"), new String[][] { { "", t(a, "Same as elsewhere"), "" },
            { "on", t(a, "Text mode"), t(a, "While someone talks: A, B, the D-pad (for choices), turbo and fast-forward only, in a layout of their own") } });
        choice(a, "DM_FORK_CTX_MENU", t(a, "Full-screen menus"), new String[][] { { "", t(a, "Same as elsewhere"), "" },
            { "on", t(a, "Menu mode"), t(a, "Bag, Box, Pokédex, the X menu: the sticks, ZL / ZR and turbo hide, in a layout of their own") } });
        choice(a, "DM_FORK_CTX_WORLD", t(a, "Overworld"), new String[][] { { "", t(a, "Same as elsewhere"), "" },
            { "on", t(a, "Hide extras"), t(a, "Walking around: ZL / ZR, L3 / R3 and the D-pad hide (they do nothing there)") } });
        heading(a, t(a, "Battle"));
        choice(a, "DM_FORK_BATTLE", t(a, "Battle controls"), new String[][] { { "", t(a, "Same as elsewhere"), "" },
            { "on", t(a, "Battle mode"), t(a, "Battle mode: in battles the unused buttons hide (L, R, ZL, ZR, +, −, right stick), X shows a Poké Ball, B a back arrow, Y info, and battles keep a layout of their own on each screen (arrange it with Move the touch controls during a battle)") } });
        if ("on".equals(GameSettings.get(a, "DM_FORK_BATTLE"))) {
            slider(a, t(a, "Battle opacity"), "DM_FORK_BATTLE_OPACITY", 0, 100, 0, v -> v == 0 ? t(a, "same as elsewhere") : v + " %");
            slider(a, t(a, "Battle: fade after"), "DM_FORK_BATTLE_FADE", -1, 10, -1, v -> v < 0 ? t(a, "same as elsewhere") : v == 0 ? t(a, "never fade") : t(a, "%d s without a touch", v)); }
        action(a, t(a, "Move the touch controls"), t(a, "Drag a control to move it, pinch to resize, double-tap to hide. Save, Reset or Cancel on the bar at the top. Each screen and orientation keeps its own layout."), () -> {
            if (dlg != null) dlg.dismiss(); try { nativeEditTouch(); ForkTouch.bar(a); } catch (UnsatisfiedLinkError e) { } });   // FORK: ForkTouch.bar
    }
    // Remapping (PadMapping.java, DM_PAD_MAPPING): tap a Switch button, then press the controller key for it (the panel takes the key: remapKey)
    private static String capture;
    private static void remap(Activity a) {
        Map<String, Integer> map = PadMapping.parse(GameSettings.get(a, "DM_PAD_MAPPING"));
        heading(a, t(a, "Remap controller buttons"));
        note(a, t(a, "Tap a button, then press the controller key you want for it."));
        for (String[] btn : PadMapping.BUTTONS) {
            LinearLayout c = card(a); LinearLayout row = titleRow(a, c, btn[1], null);
            Integer kc = map.get(btn[0]); boolean wait = btn[0].equals(capture);
            row.addView(text(a, wait ? t(a, "Press a key…") : kc != null ? PadMapping.keyName(kc) : t(a, "default"), 14, wait ? accent(a) : SUB, wait));
            if (wait) c.setBackground(round(CARD, dp(a, 16), dp(a, 2), accent(a)));
            c.setOnClickListener(x -> { capture = btn[0]; redraw(); });
        }
        LinearLayout r = new LinearLayout(a); body.addView(r);
        button(a, r, t(a, "Reset all"), false, () -> { set(a, "DM_PAD_MAPPING", ""); capture = null; say(t(a, "Controller buttons reset.")); redraw(); });
        button(a, r, t(a, "Done"), true, () -> { capture = null; go("controls"); });
    }
    private static boolean remapKey(int kc, KeyEvent e) {
        if (capture == null || e.getAction() != KeyEvent.ACTION_DOWN || !PadMapping.isPadKey(e)) return false;
        Map<String, Integer> map = PadMapping.parse(GameSettings.get(act, "DM_PAD_MAPPING"));
        map.values().removeIf(v -> v == kc);   // a key drives one Switch button
        map.put(capture, kc); set(act, "DM_PAD_MAPPING", PadMapping.serialize(map)); capture = null;
        redraw(); return true;
    }

    // ---------- mods ----------
    // <files>/mods/<mod>/romfs (files that replace the game's) and <mod>/exefs/*.pchtxt (code patches for this game version), as the Switch emulators lay them out;
    // loose .pchtxt files in mods/ work too. Switching one on or off renames its folder (a leading "-" switches it off, android_main.c / loader.c); applies at the next start.
    // Load order: mods/order.txt, one name a line (without the "-"), first = wins where two mods replace the same file (android_main.c mods_romfs); mods it does not list
    // come after, by name. Notes: <mod>/notes.txt (the mod's own readme when the zip has one, or what the player writes)
    private static File modsDir(Activity a) { return new File(GameSettings.dir(a), "mods"); }
    private static String plain(File f) { String n = f.getName(); return n.startsWith("-") ? n.substring(1) : n; }
    private static List<String> order(File dir) {
        List<String> o = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(new File(dir, "order.txt")))) { String l; while ((l = r.readLine()) != null) if (!l.trim().isEmpty()) o.add(l.trim()); } catch (IOException e) { }
        return o;
    }
    private static void saveOrder(Activity a, List<File> ms) {
        try (FileWriter w = new FileWriter(new File(modsDir(a), "order.txt"))) { for (File f : ms) w.write(plain(f) + "\n"); } catch (IOException e) { }
    }
    private static List<File> modList(Activity a) {
        File[] fs = modsDir(a).listFiles(); List<File> all = new ArrayList<>(); if (fs == null) return all;
        Arrays.sort(fs); for (File f : fs) if (f.isDirectory() && !f.getName().startsWith(".") && !f.getName().equals("romfs")) all.add(f);
        List<File> out = new ArrayList<>();
        for (String n : order(modsDir(a))) for (File f : all) if (plain(f).equals(n) && !out.contains(f)) out.add(f);
        for (File f : all) if (!out.contains(f)) out.add(f);
        return out;
    }
    private static void files(File root, File d, Set<String> out) {
        File[] fs = d.listFiles(); if (fs == null) return;
        for (File f : fs) { if (f.isDirectory()) files(root, f, out); else out.add(root.toURI().relativize(f.toURI()).getPath().toLowerCase(java.util.Locale.ROOT)); }
    }
    // for each mod that is on: the other mods that are on and replace some of the same game files, with how many
    private static Map<File, Map<File, Integer>> conflicts(List<File> ms) {
        Map<File, Set<String>> fs = new HashMap<>();
        for (File m : ms) if (!m.getName().startsWith("-")) { Set<String> s = new HashSet<>(); File r = new File(m, "romfs"); files(r, r, s); fs.put(m, s); }
        Map<File, Map<File, Integer>> out = new HashMap<>();
        for (File x : fs.keySet()) for (File y : fs.keySet()) {
            if (x == y) continue; int n = 0; for (String p : fs.get(x)) if (fs.get(y).contains(p)) n++;
            if (n > 0) out.computeIfAbsent(x, k -> new HashMap<>()).put(y, n);
        }
        return out;
    }
    private static void mods(Activity a) {
        List<File> ms = modList(a); Map<File, Map<File, Integer>> cf = conflicts(ms);
        heading(a, t(a, "Mods (%d)", ms.size()));
        if (RESTART.contains("mods")) note(a, t(a, "Mods change from the next start."));
        LinearLayout top = new LinearLayout(a); body.addView(top);
        button(a, top, t(a, "Import a mod (.zip)"), true, () -> pick(a, Intent.ACTION_OPEN_DOCUMENT, null, REQ_MOD));
        if (ms.size() > 1) button(a, top, t(a, "Order and notes"), false, () -> go("order"));
        if (ms.isEmpty()) { note(a, t(a, "No mods yet.") + " " + where(a)); return; }
        for (File m : ms) {
            boolean on = !m.getName().startsWith("-");
            LinearLayout c = card(a); LinearLayout row = titleRow(a, c, plain(m), null); row.addView(pill(a, on ? t(a, "On") : t(a, "Off"), on));
            if (cf.containsKey(m)) { TextView w = text(a, "⚠ " + t(a, "shares files with %d other mod(s)", cf.get(m).size()), 13, WARN, false); w.setPadding(0, dp(a, 4), 0, 0); c.addView(w); }
            c.setOnClickListener(v -> { if (m.renameTo(new File(m.getParentFile(), on ? "-" + plain(m) : plain(m)))) RESTART.add("mods"); redraw(); });
        }
        note(a, where(a));
    }
    private static String where(Activity a) {
        String fm = Fork.shown(a, "mods");   // FORK: the shared folder
        if (fm != null) return t(a, "Put each mod in its own folder in %s/ — <mod>/romfs for files, <mod>/exefs for .pchtxt code patches (they must be made for %s %s).",
                                 fm, GameSettings.game(a)[1], GameSettings.version(a));
        return t(a, "Put each mod in its own folder in Android/data/%s/files/mods/ — <mod>/romfs for files, <mod>/exefs for .pchtxt code patches (they must be made for %s %s).",
                 a.getPackageName(), GameSettings.game(a)[1], GameSettings.version(a));
    }
    // the load order: higher wins; each mod's notes, the files it shares, moving it, deleting it
    private static void order(Activity a) {
        List<File> ms = modList(a); Map<File, Map<File, Integer>> cf = conflicts(ms);
        heading(a, t(a, "Load order: higher wins"));
        for (int i = 0; i < ms.size(); i++) {
            File m = ms.get(i); final int k = i; LinearLayout c = card(a);
            titleRow(a, c, (i + 1) + ". " + plain(m) + (m.getName().startsWith("-") ? " (" + t(a, "off") + ")" : ""), null);
            String n = ""; try { n = new String(java.nio.file.Files.readAllBytes(new File(m, "notes.txt").toPath()), java.nio.charset.StandardCharsets.UTF_8).trim(); } catch (IOException e) { }
            TextView nt = text(a, n.isEmpty() ? t(a, "No notes.") : n, 13, SUB, false); nt.setMaxLines(6); nt.setPadding(0, dp(a, 4), 0, 0); c.addView(nt);
            Map<File, Integer> sh = cf.get(m);
            if (sh != null) for (Map.Entry<File, Integer> e : sh.entrySet()) {
                boolean wins = ms.indexOf(m) < ms.indexOf(e.getKey());
                TextView w = text(a, "⚠ " + t(a, wins ? "Replaces %d of the same files as %s: this mod's are used (it is higher)." : "Replaces %d of the same files as %s: that mod's are used (it is higher).", e.getValue(), plain(e.getKey())), 13, WARN, false);
                w.setPadding(0, dp(a, 4), 0, 0); c.addView(w);
            }
            LinearLayout r = new LinearLayout(a); c.addView(r);
            if (i > 0) button(a, r, "↑ " + t(a, "Move up"), false, () -> { java.util.Collections.swap(ms, k, k - 1); saveOrder(a, ms); RESTART.add("mods"); redraw(); });
            if (i < ms.size() - 1) button(a, r, "↓ " + t(a, "Move down"), false, () -> { java.util.Collections.swap(ms, k, k + 1); saveOrder(a, ms); RESTART.add("mods"); redraw(); });
            button(a, r, t(a, "Edit notes"), false, () -> notes(a, m));
            button(a, r, t(a, "Delete"), false, () -> confirm(a, t(a, "Delete this mod from the phone?"), () -> { deleteTree(m); ms.remove(m); saveOrder(a, ms); RESTART.add("mods"); go("order"); }));
        }
        LinearLayout r = new LinearLayout(a); body.addView(r); button(a, r, t(a, "Done"), true, () -> go("mods"));
    }
    private static void notes(Activity a, File m) {
        body.removeAllViews(); heading(a, plain(m) + " · " + t(a, "Notes"));
        LinearLayout c = card(a); EditText e = field(a, c, ""); e.setSingleLine(false); e.setMinLines(5); e.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE);
        try { e.setText(new String(java.nio.file.Files.readAllBytes(new File(m, "notes.txt").toPath()), java.nio.charset.StandardCharsets.UTF_8)); } catch (IOException x) { }
        LinearLayout r = new LinearLayout(a); body.addView(r);
        button(a, r, t(a, "Save"), true, () -> { try (FileWriter fw = new FileWriter(new File(m, "notes.txt"))) { fw.write(e.getText().toString()); } catch (IOException x) { } go("order"); });
        button(a, r, t(a, "Cancel"), false, () -> go("order"));
    }
    private static void confirm(Activity a, String q, Runnable yes) { confirm(a, q, t(a, "Delete"), yes); }   // FORK v47: the button's label a parameter (restart: OK)
    private static void confirm(Activity a, String q, String label, Runnable yes) {   // asked in a card at the top of the page, not a pop-up
        LinearLayout c = new LinearLayout(a); c.setOrientation(LinearLayout.VERTICAL); c.setBackground(round(CARD, dp(a, 16), dp(a, 2), WARN)); c.setPadding(dp(a, 16), dp(a, 12), dp(a, 16), dp(a, 12));
        c.addView(text(a, q, 15, TEXT, true)); LinearLayout r = new LinearLayout(a); c.addView(r);
        button(a, r, label, true, yes); button(a, r, t(a, "Cancel"), false, () -> go(page));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2); lp.bottomMargin = dp(a, 10); body.addView(c, 0, lp); scroll.scrollTo(0, 0);
    }

    // ---------- saves, online, system ----------
    // FORK v47: save slots (ForkSaves.java): each slot a card with Use / Rename / Backups / Delete; a new slot, empty or a copy of the current one
    private static String slotOpen;   // the slot whose backups are listed (null: none)
    private static void slots(Activity a) {
        heading(a, t(a, "Save slots"));
        note(a, t(a, "Each slot is its own save. Switching restarts the game into the chosen slot: save in the game first. Each start keeps a backup of the slot in use (the newest %d).", ForkSaves.KEEP));
        String act = ForkSaves.active(a), nxt = ForkSaves.next(a);
        for (String[] sl : ForkSaves.list(a)) {
            String id = sl[0]; boolean isAct = id.equals(act), isNext = id.equals(nxt) && !isAct;
            LinearLayout c = card(a);
            titleRow(a, c, sl[1], isAct && nxt.equals(act) ? t(a, "In use") : isAct ? t(a, "In use until restart") : isNext ? t(a, "Next start") : null);
            LinearLayout r = new LinearLayout(a); c.addView(r);
            if (!id.equals(nxt)) button(a, r, t(a, "Use"), true, () -> { ForkSaves.choose(a, id); RESTART.add("DM_FORK_SLOT"); redraw(); });
            button(a, r, t(a, "Rename"), false, () -> { slotOpen = "rename:" + id; redraw(); });
            button(a, r, t(a, "Backups"), false, () -> { slotOpen = id.equals(slotOpen) ? null : id; redraw(); });
            if (!isAct && !id.equals(nxt) && !id.equals("main")) button(a, r, t(a, "Delete"), false, () -> confirm(a, t(a, "Delete the slot \"%s\" and its backups from the phone?", sl[1]), () -> { ForkSaves.delete(a, id); redraw(); }));
            if (("rename:" + id).equals(slotOpen)) {
                EditText e = field(a, c, t(a, "slot name")); e.setText(sl[1]);
                LinearLayout rr = new LinearLayout(a); c.addView(rr);
                button(a, rr, t(a, "Save"), true, () -> { ForkSaves.rename(a, id, e.getText().toString()); slotOpen = null; redraw(); });
                button(a, rr, t(a, "Cancel"), false, () -> { slotOpen = null; redraw(); }); }
            if (id.equals(slotOpen)) {
                File[] bs = ForkSaves.backups(a, id);
                if (bs.length == 0) c.addView(text(a, t(a, "No backups yet: one is made each time the game starts on this slot."), 13, SUB, false));
                for (File b : bs) {
                    String n = b.getName(); String when = n.length() >= 15 ? n.substring(0, 4) + "-" + n.substring(4, 6) + "-" + n.substring(6, 8) + " " + n.substring(9, 11) + ":" + n.substring(11, 13) : n;
                    LinearLayout br = new LinearLayout(a); br.setGravity(Gravity.CENTER_VERTICAL); c.addView(br);
                    TextView tv = text(a, when, 14, TEXT, false); br.addView(tv, new LinearLayout.LayoutParams(0, -2, 1));
                    button(a, br, t(a, "Restore"), false, () -> confirm(a, isAct ? t(a, "Restore this backup at the next start? The slot's current save is replaced (a backup of it is kept first).")
                                                                             : t(a, "Replace this slot's save with the backup from %s?", when), t(a, "OK"), () -> {
                        try { ForkSaves.restore(a, id, b); if (isAct) RESTART.add("DM_FORK_RESTORE"); } catch (IOException x) { android.widget.Toast.makeText(a, String.valueOf(x), android.widget.Toast.LENGTH_LONG).show(); }
                        slotOpen = null; redraw(); })); } }
        }
        // a new slot
        LinearLayout c = card(a); titleRow(a, c, t(a, "New slot"), null);
        EditText name = field(a, c, t(a, "name, e.g. Shiny hunt"));
        LinearLayout r = new LinearLayout(a); c.addView(r);
        Runnable[] made = new Runnable[1];
        java.util.function.Consumer<Boolean> mk = copy -> { try { ForkSaves.create(a, name.getText().toString(), copy); android.widget.Toast.makeText(a, t(a, "Slot made: tap Use to switch to it."), android.widget.Toast.LENGTH_SHORT).show(); }
                                                            catch (IOException x) { android.widget.Toast.makeText(a, String.valueOf(x), android.widget.Toast.LENGTH_LONG).show(); } redraw(); };
        button(a, r, t(a, "Empty (new game)"), true, () -> mk.accept(false));
        button(a, r, t(a, "Copy of the current slot"), false, () -> mk.accept(true));
        c.addView(text(a, t(a, "A copy takes the current slot's last save in the game."), 13, SUB, false));
    }
    private static void saves(Activity a) {
        slots(a);   // FORK v47
        heading(a, t(a, "Saves"));
        action(a, t(a, "Export save data"), t(a, "Save in the game first: the export is the last save the game wrote."),
               () -> pick(a, Intent.ACTION_CREATE_DOCUMENT, GameSettings.game(a)[2] + "-save.zip", REQ_EXPORT));
        action(a, t(a, "Import save data"), t(a, "Your current save is kept as a backup."), () -> pick(a, Intent.ACTION_OPEN_DOCUMENT, null, REQ_IMPORT));
    }
    // Online room (hle_relay.c, dm/server): local play with players elsewhere through a room server someone hosts (their own Railway service, or any PC); everyone enters the
    // same server address and room code. Applies from the next start; mods stay off for local play, as on Wi-Fi
    private static void online(Activity a) {
        heading(a, t(a, "Online room"));
        note(a, t(a, "Play the game's local link battles, trades and raids with friends anywhere: everyone enters the same room server and room code. Host your own server (dm/server in the project: Railway or any PC). Mods must be off."));
        String room = GameSettings.get(a, "DM_RELAY_ROOM");
        LinearLayout c = card(a); titleRow(a, c, room.isEmpty() ? t(a, "off (local play on this Wi-Fi)") : t(a, "Room: %s", room), RESTART.contains("DM_RELAY_ROOM") ? t(a, "Applies next start") : null);
        EditText srv = field(a, c, t(a, "server (address:port)")); srv.setText(GameSettings.get(a, "DM_RELAY"));
        EditText code = field(a, c, t(a, "room code")); code.setText(room);
        LinearLayout r = new LinearLayout(a); c.addView(r);
        button(a, r, t(a, "Join"), true, () -> { String sv = srv.getText().toString().trim(), cd = code.getText().toString().trim();
            if (sv.isEmpty() || cd.isEmpty()) { say(t(a, "Enter both the server and the room code.")); return; }
            GameSettings.set(a, "DM_RELAY", sv); GameSettings.set(a, "DM_RELAY_ROOM", cd.replaceAll("\\s+", "")); RESTART.add("DM_RELAY_ROOM"); go("online"); });
        if (!room.isEmpty()) button(a, r, t(a, "Leave room"), false, () -> { GameSettings.set(a, "DM_RELAY_ROOM", ""); RESTART.add("DM_RELAY_ROOM"); go("online"); });
    }
    // About: whether this copy is an official build (BuildSigner: signed with the release key), the credits, and the licences the APK carries (assets/licenses,
    //   build_apk.py). The app is GPL-3.0-or-later (its older code also MIT): its copyright and licence notice must stay with every copy, so a repack that strips this page breaks the licence
    private static String asset(Activity a, String name) {
        try (java.io.InputStream in = a.getAssets().open(name)) {
            java.io.ByteArrayOutputStream o = new java.io.ByteArrayOutputStream(); byte[] b = new byte[8192]; for (int n; (n = in.read(b)) > 0; ) o.write(b, 0, n); return o.toString("UTF-8");
        } catch (Exception e) { return ""; }
    }
    private static void reveal(Activity a, LinearLayout c, String label, String full) {   // a licence's text, folded until tapped
        TextView txt = text(a, full, 11, SUB, false); txt.setTypeface(Typeface.MONOSPACE); txt.setTextIsSelectable(true); txt.setVisibility(View.GONE);
        TextView b = text(a, label, 13, accent(a), true); b.setPadding(0, dp(a, 8), 0, dp(a, 2));
        b.setOnClickListener(v -> txt.setVisibility(txt.getVisibility() == View.GONE ? View.VISIBLE : View.GONE));
        c.addView(b); c.addView(txt);
    }
    private static void about(Activity a) {
        String[] app = GameSettings.app(a), sg = BuildSigner.digests(a); boolean official = BuildSigner.official(sg);
        String ver = ""; try { ver = a.getPackageManager().getPackageInfo(a.getPackageName(), 0).versionName; } catch (Exception e) { }
        String dl = "https://" + app[1] + "/releases";
        heading(a, app[0] + (ver == null || ver.isEmpty() ? "" : " v" + ver));
        LinearLayout c = card(a); titleRow(a, c, official ? t(a, "Official build") : t(a, "Unofficial build"), null);
        c.addView(text(a, official ? t(a, "Signed with the %s release key.", app[0])
                                   : t(a, "This copy was not signed by the %s project (signer %s). It may have been changed.", app[0], sg.length > 0 && sg[0].length() >= 16 ? sg[0].substring(0, 16) : "?"),
                       13, SUB, false));
        TextView link = text(a, t(a, "Official downloads: %s", dl), 13, accent(a), false); link.setPadding(0, dp(a, 6), 0, 0);
        link.setOnClickListener(v -> { try { a.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(dl))); } catch (Exception e) { } });
        c.addView(link);
        // the credits: the copyright lines of the app's licence, then the licence itself
        String own = asset(a, "licenses/LICENSE.txt"); StringBuilder who = new StringBuilder();
        for (String l : own.split("\n")) { String s_ = l.trim();   // (not the GPL text's own copyright line, and each holder once: NOTICE and LICENSE-MIT both name them)
            if (s_.startsWith("Copyright") && !s_.contains("Free Software Foundation") && who.indexOf(s_) < 0) who.append(s_).append('\n'); }
        if (who.length() == 0) who.append("Copyright (c) 2026 jhaizhou (d2r-mobile, nxnative)\nCopyright (c) 2026 the Galar Native (formerly PKShield Android) contributors\n");
        heading(a, t(a, "Credits"));
        LinearLayout cc = card(a); cc.addView(text(a, who.toString().trim(), 14, TEXT, false));
        TextView mit = text(a, t(a, "Free software under the GNU GPL, version 3 or later (older parts also MIT). This notice must stay with every copy, and the source of every release is public."), 13, SUB, false); mit.setPadding(0, dp(a, 6), 0, 0); cc.addView(mit);
        if (!own.isEmpty()) reveal(a, cc, t(a, "Show the license"), own);
        // the third-party code and fonts (licenses/index.tsv: file, name, what it is)
        heading(a, t(a, "Third-party code"));
        for (String row : asset(a, "licenses/index.tsv").split("\n")) {
            String[] f = row.split("\t"); if (f.length < 3 || f[0].equals("LICENSE.txt")) continue;
            LinearLayout tc = card(a); titleRow(a, tc, f[1], null); tc.addView(text(a, f[2], 13, SUB, false)); reveal(a, tc, t(a, "Show the license"), asset(a, "licenses/" + f[0]));
        }
    }
    private static void system(Activity a) {
        if (Fork.onlyGame(a) != null) {   // FORK: a one-game build opens straight into the game; this is the way to the setup screen (DLC, save data, driver)
            heading(a, t(a, "App"));
            action(a, t(a, "Setup screen"), t(a, "Add DLC, export or import save data, change the graphics driver. The game keeps running behind it: after adding DLC, close the app from recent apps and open it again."),
                   () -> { if (dlg != null) dlg.dismiss(); a.startActivity(new android.content.Intent(a, SetupActivity.class).putExtra("setup", true).putExtra("fork_setup", true)); });
        }
        heading(a, t(a, "Graphics driver"));
        action(a, t(a, "Graphics driver"), DriverPackages.label(DriverPackages.current(a)), () -> go("driver"));
        heading(a, t(a, "Language"));
        String[][] l = new String[T.LANGS.length][]; for (int i = 0; i < l.length; i++) l[i] = new String[] { T.LANGS[i][0], i == 0 ? t(a, "Auto") : T.LANGS[i][1], i == 0 ? t(a, "Auto (the phone's language)") : "" };
        choice(a, "DM_LANG", t(a, "Language"), l);
        heading(a, t(a, "Background"));
        // FORK v46: upstream's "Leaving the game" (DM_BACKGROUND) and the fork's keep-alive service (DM_FORK_KEEPALIVE, ForkKeepAlive) as one choice
        combo(a, t(a, "Leaving the game"), new String[] { "DM_BACKGROUND", "DM_FORK_KEEPALIVE" }, new String[][] {
            { t(a, "Keep playing"), t(a, "Keep playing: the game and its sound go on in the background (Android may close it after a few minutes)"), "", "" },
            { t(a, "Pause"), t(a, "Pause: the game stops while it is not on screen (Android may close it after a few minutes)"), "off", "" },
            { t(a, "Pause, stay open"), t(a, "Pause, and a quiet notification keeps the game open while you use other apps"), "off", "on" },
            { t(a, "Keep playing, stay open"), t(a, "Keep playing, and a quiet notification keeps the game open (uses battery)"), "", "on" } });
        // the replay on its own thread (hle_nvn.c DM_NVN_ASYNC): the game's thread hands each frame's graphics work over instead of translating it itself (about a quarter
        //   of a frame on a slow CPU, #99). Off by default until players have run it on many phones
        heading(a, t(a, "Performance"));
        choice(a, "DM_NVN_ASYNC", t(a, "Faster rendering (experimental)"), new String[][] { { "", t(a, "Off"), "" },
            { "1", t(a, "On"), t(a, "On: the graphics work runs on its own thread, next to the game's. Not tested on every phone yet: if the game misbehaves, turn it off and tell us on GitHub") } });
        // a frame capture for a bug report (nvn_vk.c, Frame capture): the engine reads capture.req (frames to wait, so the menu can be closed first) and writes the ZIP
        heading(a, t(a, "Bug reports"));
        action(a, t(a, "Capture the trainer-spotted moment"), t(a, "For the Fill, corrected picture: the first time something new is drawn over the overworld (a trainer spotting you), that frame is saved at once (Download, a ZIP)."),   // FORK v41
               () -> { try { nativeSetting("DM_FORK_CAPTURE", "trainer"); } catch (UnsatisfiedLinkError e) { }
                       android.widget.Toast.makeText(a, t(a, "Armed: close the menu and walk into a trainer's view."), android.widget.Toast.LENGTH_LONG).show(); });
        action(a, t(a, "Capture a frame"), t(a, "Close this menu within 3 seconds and stay where the problem shows: the next frame is saved, step by step, as pictures and notes in Download (a ZIP ending in frame-capture.zip). Attach it to your issue."),
               () -> { try (java.io.FileWriter w = new java.io.FileWriter(new File(GameSettings.dir(a), "capture.req"))) { w.write("90"); } catch (IOException e) { }
                       android.widget.Toast.makeText(a, t(a, "Capturing in 3 seconds: close the menu."), android.widget.Toast.LENGTH_LONG).show(); });
        // Mali's white scenes (#47 #53 #64 #68 #69 #72 #75): switches the engine has for narrowing it down, which players could only set by editing env.txt; each one a
        //   tester tries (with a capture) rules a cause in or out. Shown on Mali phones only
        if (DriverPackages.gpu(a).contains("mali")) {
            heading(a, t(a, "White screen tests (Mali)"));
            note(a, t(a, "Try one at a time, restart the game, and tell us on GitHub whether the picture changed (a capture helps)."));
            choice(a, "DM_VK_UBO_SNAPSHOT", t(a, "Copy the game's uniform data"), new String[][] { { "", t(a, "Off"), "" },
                { "1", t(a, "On"), t(a, "On: each draw's uniform data is copied when it is recorded, instead of the GPU reading the game's memory directly") } });
            choice(a, "DM_VK_FLOAT_CONTROLS", t(a, "Exact infinities in shaders"), new String[][] { { "", t(a, "Auto"), t(a, "Auto: on with ARM's driver (v0.52)") },
                { "0", t(a, "Off"), t(a, "Off: the shaders as before v0.52") } });
            choice(a, "DM_VK_NAN_MINMAX", t(a, "Min / max as on the Switch"), new String[][] { { "", t(a, "Auto"), t(a, "Auto: on with ARM's driver (v0.57)") },
                { "0", t(a, "Off"), t(a, "Off: the shaders as before v0.57") } });
            choice(a, "DM_VK_FMZ", t(a, "Multiply by zero as on the Switch"), new String[][] { { "", t(a, "Auto"), t(a, "Auto: on with PanVK (black patches), off with the phone's own Mali driver") },
                { "1", t(a, "On"), t(a, "On: a multiply with a zero gives zero even when the other value is infinite, as the Switch's GPU does (a test for white or glowing surfaces; a little slower)") },
                { "0", t(a, "Off"), t(a, "Off: plain multiplies, as before v0.74") } });
            // the white-screen probe (nvn_spv.c spv_probe, #215): one kind of value clamped in every fragment shader; the run where the white goes names the kind
            choice(a, "DM_VK_PROBE", t(a, "White-screen probe"), new String[][] { { "", t(a, "Off"), "" },
                { "samples", t(a, "Textures"), t(a, "Textures: every texture read is limited to a sane range. If the white goes away, tell us: a texture is read wrong") },
                { "inputs", t(a, "Inputs"), t(a, "Inputs: every value the vertex shader hands over is limited. If the white goes away, tell us") },
                { "div", t(a, "Divisions"), t(a, "Divisions: every division is limited. If the white goes away, tell us") } });
            choice(a, "DM_VK_DEPTH_DISCARD", t(a, "Keep unused depth in tile memory"), new String[][] { { "", t(a, "Off"), "" },
                { "1", t(a, "On"), t(a, "On: depth that nothing reads afterwards is not written back to memory (faster on Mali; a test: tell us if anything flickers)") } });
            choice(a, "DM_VK_NO_PASS_MERGE", t(a, "Merge render passes"), new String[][] { { "", t(a, "On"), "" },
                { "1", t(a, "Off"), t(a, "Off: every render target change starts a new pass") } });
        }
    }
    // the graphics driver: the phone's own, an installed package (Turnip, PanVK...), a new package from the file picker, or the releases page for this GPU; from the next start
    private static void driver(Activity a) {
        List<String[]> pk = DriverPackages.installed(a); String cur = DriverPackages.current(a); String[] src = DriverPackages.source(a);
        heading(a, t(a, "Graphics driver"));
        if (RESTART.contains("driver")) note(a, t(a, "The new driver is used from the next start."));
        driverRow(a, t(a, "The phone's own driver"), cur.isEmpty(), () -> DriverPackages.useSystem(a));
        for (String[] p : pk) driverRow(a, p[0], p[1].equals(cur), () -> { String why = DriverPackages.mismatch(a, new File(p[1]));
            if (why != null) android.widget.Toast.makeText(a, t(a, "Not used:") + " " + why, android.widget.Toast.LENGTH_LONG).show(); else DriverPackages.use(a, new File(p[1])); });
        LinearLayout r = new LinearLayout(a); body.addView(r);
        button(a, r, t(a, "Install a driver package (.zip)"), true, () -> pick(a, Intent.ACTION_OPEN_DOCUMENT, null, REQ_DRIVER));
        if (src != null) button(a, r, src[0], false, () -> a.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(src[1]))));
        if (DriverPackages.isTurnip(cur)) {
            heading(a, t(a, "Turnip options (from the next start)"));
            Set<String> on = DriverPackages.turnipOptions(a);
            for (String[] o : DriverPackages.TURNIP_OPTIONS) {
                boolean v = on.contains(o[0]); LinearLayout c = card(a); LinearLayout row = titleRow(a, c, o[1], null); row.addView(pill(a, v ? t(a, "On") : t(a, "Off"), v));
                c.setOnClickListener(x -> { if (v) on.remove(o[0]); else on.add(o[0]); DriverPackages.setTurnipOptions(a, on); RESTART.add("driver"); redraw(); });
            }
        }
        LinearLayout d = new LinearLayout(a); body.addView(d); button(a, d, t(a, "Done"), false, () -> go("system"));
    }
    private static void driverRow(Activity a, String name, boolean on, Runnable use) {
        LinearLayout c = card(a); LinearLayout row = titleRow(a, c, name, null);
        if (on) { row.addView(text(a, "✓", 18, accent(a), true)); c.setBackground(round(CARD, dp(a, 16), dp(a, 2), accent(a))); }
        c.setOnClickListener(v -> { use.run(); RESTART.add("driver"); redraw(); });
    }

    // ---------- files: save data out and in, mods and drivers in (the system file picker; GameActivity.onActivityResult hands the answer here) ----------
    static final int REQ_EXPORT = 101, REQ_IMPORT = 102, REQ_MOD = 103, REQ_DRIVER = 104, REQ_LSFG = 105;
    private static void pick(Activity a, String action, String name, int req) {
        // (to open, any file: ZIPs saved by browsers and chat apps are often typed otherwise, and the picker greyed them out; a save ZIP is checked when read)
        Intent i = new Intent(action); i.addCategory(Intent.CATEGORY_OPENABLE); i.setType(Intent.ACTION_OPEN_DOCUMENT.equals(action) ? "*/*" : "application/zip");
        if (name != null) i.putExtra(Intent.EXTRA_TITLE, name);
        a.startActivityForResult(i, req);
    }
    static boolean onResult(Activity a, int req, int res, Intent data) {
        if (req != REQ_EXPORT && req != REQ_IMPORT && req != REQ_MOD && req != REQ_DRIVER && req != REQ_LSFG) return false;
        Uri u = res == Activity.RESULT_OK && data != null ? data.getData() : null; if (u == null) return true;
        File files = GameSettings.dir(a);
        new Thread(() -> {
            String msg;
            try {
                if (req == REQ_DRIVER) msg = DriverPackages.install(a, u, displayName(a, u));
                else if (req == REQ_LSFG) {   // the DLL read whole (about 3 MB), its shaders converted into files/lsfg (android_main.c DM_LSFG_DIR)
                    java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
                    try (InputStream in = a.getContentResolver().openInputStream(u)) { byte[] b = new byte[1 << 16]; int k; long tot = 0;
                        while ((k = in.read(b)) > 0) { bo.write(b, 0, k); if ((tot += k) > (64L << 20)) throw new IOException("the file is too large for Lossless.dll"); } }
                    String why = nativeLsfgImport(bo.toByteArray(), new File(GameSettings.root(a), "lsfg").getPath());
                    msg = why == null ? t(a, "Lossless Scaling imported. Choose it under Frame generation.") : t(a, "Could not import Lossless Scaling: %s", why); }
                else if (req == REQ_EXPORT) {
                    try (OutputStream o = a.getContentResolver().openOutputStream(u, "wt"); ZipOutputStream z = new ZipOutputStream(o)) {
                        File save = new File(files, "save").getCanonicalFile(); int n = zipTree(save, save, z); msg = t(a, "Exported %d save file(s).", n); }
                } else if (req == REQ_IMPORT) {
                    try (InputStream in = a.getContentResolver().openInputStream(u)) {
                        int n = SaveImport.install(in, new File(files, "save_import"), displayName(a, u));
                        msg = t(a, "Imported %d save file(s). Close the game and start it again to load it (your current save is kept as a backup).", n) + " (" + SaveImport.lastFiles + ")"; }
                } else {
                    String name = displayName(a, u).replaceAll("(?i)\\.zip$", "").replaceAll("[^A-Za-z0-9 ._()\\[\\]-]", "_").trim();
                    if (name.isEmpty() || name.startsWith(".") || name.startsWith("-")) name = "mod";
                    File dest = new File(new File(files, "mods"), name);
                    try (InputStream in = a.getContentResolver().openInputStream(u)) { int n = installMod(in, dest);
                        msg = t(a, "Mod \"%s\" installed (%d files). It is used from the next start.", name, n); }
                }
            } catch (IOException | SecurityException e) {
                msg = t(a, req == REQ_EXPORT ? "Could not export the save: %s" : req == REQ_IMPORT ? "Could not import the save: %s" : "Could not install the mod: %s", e.getMessage()); }
            final String m = msg;
            a.runOnUiThread(() -> {   // (the answer in the panel's status line, on the page it belongs to)
                if (dlg == null) open(a, null);
                if (req == REQ_MOD) { RESTART.add("mods"); go("mods"); } else if (req == REQ_DRIVER) go("driver"); else if (req == REQ_LSFG) redraw(); else go("saves");
                say(m);
            });
        }).start();
        return true;
    }
    private static String displayName(Activity a, Uri u) {
        try (android.database.Cursor c = a.getContentResolver().query(u, new String[] { android.provider.OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0); } catch (Exception e) { }
        return "mod";
    }
    private static int zipTree(File root, File dir, ZipOutputStream z) throws IOException {
        File[] cs = dir.listFiles(); if (cs == null) throw new IOException("could not read " + dir); int n = 0;
        for (File f : cs) { File cf = f.getCanonicalFile(); if (!f.getAbsolutePath().equals(cf.getPath())) continue;   // (links stay out)
            if (f.isDirectory()) { n += zipTree(root, f, z); continue; }
            z.putNextEntry(new ZipEntry(root.toURI().relativize(f.toURI()).getPath()));
            try (InputStream in = new java.io.FileInputStream(f)) { byte[] b = new byte[1 << 16]; int k; while ((k = in.read(b)) > 0) z.write(b, 0, k); }
            z.closeEntry(); n++; }
        return n;
    }
    // A mod zip into mods/<name>/ with romfs/ and exefs/ at its top, whichever way the zip holds them: at its root, inside one folder (the mod's name), or the Atmosphere
    // layout (atmosphere/contents/<title ID>/romfs); loose .pchtxt files go to exefs/. The mod's own readme (readme*, info*, notes*: .txt / .md) becomes its notes.txt.
    // Entries are checked to stay inside the folder; at most 8 GB
    static int installMod(InputStream source, File dest) throws IOException {
        File tmp = new File(dest.getParentFile(), "." + dest.getName() + ".part"); deleteTree(tmp); tmp.mkdirs();
        int n = 0; long total = 0; String root = tmp.getCanonicalPath() + File.separator; boolean noted = false;
        try (ZipInputStream z = new ZipInputStream(source)) { ZipEntry e; byte[] b = new byte[1 << 16];
            while ((e = z.getNextEntry()) != null) {
                String p = e.getName().replace('\\', '/'); String rel = modPath(p);
                if (rel == null && !e.isDirectory() && !noted && isReadme(p)) {   // (the first readme, up to 64 KB)
                    java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream(); int k; while ((k = z.read(b)) > 0 && bo.size() < (64 << 10)) bo.write(b, 0, k);
                    try (OutputStream o = new FileOutputStream(new File(tmp, "notes.txt"))) { o.write(bo.toByteArray()); } noted = true; continue;
                }
                if (rel == null || e.isDirectory()) continue;
                File out = new File(tmp, rel); if (!out.getCanonicalPath().startsWith(root)) throw new IOException("the zip has a path outside its folder: " + p);
                out.getParentFile().mkdirs();
                try (OutputStream o = new FileOutputStream(out)) { int k; while ((k = z.read(b)) > 0) { total += k; if (total > (8L << 30)) throw new IOException("the mod is larger than 8 GB"); o.write(b, 0, k); } }
                n++;
            } }
        if (n == 0) { deleteTree(tmp); throw new IOException("no romfs, exefs or .pchtxt files in it"); }
        deleteTree(dest); if (!tmp.renameTo(dest)) throw new IOException("could not move it into place");
        return n;
    }
    static boolean isReadme(String p) {
        String last = p.substring(p.lastIndexOf('/') + 1).toLowerCase(java.util.Locale.ROOT);
        return (last.startsWith("readme") || last.startsWith("info") || last.startsWith("notes")) && (last.endsWith(".txt") || last.endsWith(".md")) && !p.contains("..");
    }
    // the path inside the mod folder for a zip entry: from its romfs/ or exefs/ part on; a .pchtxt elsewhere goes to exefs/; anything else is left out
    static String modPath(String p) {
        String[] parts = p.split("/");
        for (int i = 0; i < parts.length - 1; i++) { String q = parts[i].toLowerCase(java.util.Locale.ROOT);
            if (q.equals("romfs") || q.equals("exefs")) { StringBuilder sb = new StringBuilder(q);
                for (int j = i + 1; j < parts.length; j++) { if (parts[j].equals("..")) return null; sb.append('/').append(parts[j]); } return sb.toString(); } }
        // a mod packed from the game's own top folders, without a romfs/ above them (Team Seri's Scarlet / Violet mods: world/scene/...): those go under romfs/
        for (int i = 0; i < parts.length - 1; i++) if (ROMFS_TOPS.contains(parts[i].toLowerCase(java.util.Locale.ROOT))) { StringBuilder sb = new StringBuilder("romfs");
            for (int j = i; j < parts.length; j++) { if (parts[j].equals("..")) return null; sb.append('/').append(parts[j]); } return sb.toString(); }
        String last = parts[parts.length - 1];
        return last.toLowerCase(java.util.Locale.ROOT).endsWith(".pchtxt") && !last.contains("..") ? "exefs/" + last : null;
    }
    private static final Set<String> ROMFS_TOPS = new HashSet<>(Arrays.asList("world", "chara", "pokemon", "field", "field_graphic", "ui", "message", "script", "effect",
        "avalon", "ai", "gfx", "obj", "system_resource", "demo", "sound", "shader", "arc", "bin"));
    private static void deleteTree(File f) { File[] cs = f.listFiles(); if (cs != null) for (File c : cs) deleteTree(c); f.delete(); }
}
