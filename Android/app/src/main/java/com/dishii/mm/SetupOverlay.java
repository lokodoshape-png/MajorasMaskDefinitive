package com.dishii.mm;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextPaint;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipInputStream;

// The screen that prepares the game files before the opening (the ROM questions, the copy of the app's files and the
// extraction's progress), in the look of the Definitive Edition's title menu: its background and moon, its fonts, a
// purple panel with gold edges. The PC draws the same screen in mm/2s2h/Extractor/SetupScreen.cpp.
//
// It is a full-screen view over everything, the opening video included. The game (SetupScreen.cpp) asks its questions
// through MainActivity.setupAsk, which waits here for the answer.
final class SetupOverlay extends View {

    interface Answer {
        void chosen(int index);
    }

    private static final float KW = 1280f, KH = 720f; // layout units, scaled to fit the screen

    private static final int GOLD = rgb(244, 204, 98), GOLD_BRIGHT = rgb(255, 232, 150), GOLD_DARK = rgb(150, 105, 40);
    private static final int TEXT = rgb(240, 228, 204), DIM = rgb(196, 178, 206), HINT = rgb(200, 184, 150);

    private final MainActivity activity;
    private Bitmap background, moon;
    private Typeface titleFont = Typeface.create(Typeface.SERIF, Typeface.BOLD);
    private Typeface bodyFont = Typeface.SERIF;
    private final long start = SystemClock.uptimeMillis();

    // What is on screen: a question (buttons != null) or the progress bar.
    private String title = "", text = "";
    private String[] buttons;
    private int selected, cancel = -1;
    private Answer answer;
    private float fraction = -1f;
    private float appear;

    private final RectF[] buttonRects = { new RectF(), new RectF(), new RectF(), new RectF() };
    private int pressed = -1;
    private int stickDir;

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final TextPaint textPaint = new TextPaint(Paint.ANTI_ALIAS_FLAG);

    // Screen mapping of the layout units, worked out on each frame.
    private float s = 1f, ox, oy;

    private static SetupOverlay sOverlay;

    private SetupOverlay(MainActivity activity) {
        super(activity);
        this.activity = activity;
        setFocusable(true);
        setFocusableInTouchMode(true);
        loadArtInBackground();
    }

    private static int rgb(int r, int g, int b) {
        return 0xFF000000 | (r << 16) | (g << 8) | b;
    }

    private static int argb(int a, int r, int g, int b) {
        return (a << 24) | (r << 16) | (g << 8) | b;
    }

    // ----- Showing and hiding (UI thread)

    static boolean isShown(MainActivity activity) {
        return sOverlay != null && sOverlay.getParent() != null;
    }

    // The overlay over everything, made on first use. The activity's content may not exist yet (onCreate): then it
    // is added a moment later.
    private static SetupOverlay obtain(MainActivity activity) {
        if (sOverlay == null) {
            sOverlay = new SetupOverlay(activity);
        }
        if (sOverlay.getParent() == null) {
            ViewGroup content = (ViewGroup) activity.getContentView();
            if (content != null) {
                content.addView(sOverlay, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.MATCH_PARENT));
                sOverlay.requestFocus();
                activity.onSetupOverlayShown();
            } else {
                new Handler(Looper.getMainLooper()).postDelayed(() -> {
                    if (sOverlay != null && sOverlay.wanted) {
                        obtain(activity);
                    }
                }, 50);
            }
        } else {
            sOverlay.bringToFront();
        }
        sOverlay.wanted = true;
        return sOverlay;
    }

    private boolean wanted;

    static void ask(MainActivity activity, String title, String text, String[] buttons, int def, int cancel,
                    Answer answer) {
        SetupOverlay o = obtain(activity);
        o.title = title;
        o.text = text;
        o.buttons = buttons.length > 4 ? java.util.Arrays.copyOf(buttons, 4) : buttons;
        o.selected = Math.max(0, Math.min(def, o.buttons.length - 1));
        o.cancel = cancel;
        o.answer = answer;
        o.pressed = -1;
        o.appear = 0f;
        o.invalidate();
    }

    // From a thread other than the UI thread: shows the question and waits for its answer.
    static int askAndWait(MainActivity activity, String title, String text, String[] buttons, int def, int cancel) {
        if (buttons == null || buttons.length == 0) {
            return cancel;
        }
        final int[] result = { cancel };
        final CountDownLatch done = new CountDownLatch(1);
        activity.runOnUiThread(() -> ask(activity, title, text, buttons, def, cancel, index -> {
            result[0] = index;
            done.countDown();
        }));
        try {
            done.await();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        return result[0];
    }

    // fraction 0..1, or below 0 while the amount of work is not known.
    static void progress(MainActivity activity, String title, String text, float fraction) {
        SetupOverlay o = obtain(activity);
        if (o.buttons != null) {
            o.appear = 0f;
        }
        o.title = title;
        o.text = text;
        o.buttons = null;
        o.answer = null;
        o.fraction = fraction;
        o.invalidate();
    }

    static void hide(MainActivity activity) {
        if (sOverlay == null) {
            return;
        }
        sOverlay.wanted = false;
        if (sOverlay.getParent() != null) {
            ((ViewGroup) sOverlay.getParent()).removeView(sOverlay);
            activity.onSetupOverlayHidden();
        }
        sOverlay = null;
    }

    // ----- The art: the title menu's pictures and fonts, from 2ship.o2r (the data folder's copy, or the APK's)

    private void loadArtInBackground() {
        new Thread(() -> {
            Map<String, byte[]> files = new HashMap<>();
            String[] names = { "overlays/titlemenu/background.jpg", "overlays/titlemenu/moon.png",
                    "fonts/Alegreya-Bold.ttf", "fonts/Alegreya-Regular.ttf" };
            File o2r = new File(new File(Environment.getExternalStorageDirectory(), MainActivity.DATA_FOLDER),
                    "2ship.o2r");
            try (ZipFile zip = new ZipFile(o2r)) {
                for (String name : names) {
                    ZipEntry e = zip.getEntry(name);
                    if (e != null) {
                        try (InputStream in = zip.getInputStream(e)) {
                            files.put(name, readAll(in));
                        }
                    }
                }
            } catch (IOException | SecurityException e) {
                // Not copied yet: the APK has one too.
            }
            if (files.size() < names.length) {
                try (ZipInputStream zin = new ZipInputStream(activity.getAssets().open("2ship.o2r"))) {
                    ZipEntry e;
                    while ((e = zin.getNextEntry()) != null && files.size() < names.length) {
                        for (String name : names) {
                            if (name.equals(e.getName()) && !files.containsKey(name)) {
                                files.put(name, readAll(zin));
                            }
                        }
                    }
                } catch (IOException e) {
                    MainActivity.startupLog("Tela de preparação: sem as imagens do menu (" + e + ")");
                }
            }
            final Bitmap bg = decode(files.get(names[0]));
            final Bitmap mn = decode(files.get(names[1]));
            final Typeface bold = font(files.get(names[2]), "setup-title.ttf");
            final Typeface regular = font(files.get(names[3]), "setup-body.ttf");
            post(() -> {
                background = bg;
                moon = mn;
                if (bold != null) {
                    titleFont = bold;
                }
                if (regular != null) {
                    bodyFont = regular;
                }
                invalidate();
            });
        }, "SetupOverlayArt").start();
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[65536];
        int n;
        while ((n = in.read(buf)) > 0) {
            out.write(buf, 0, n);
        }
        return out.toByteArray();
    }

    private static Bitmap decode(byte[] data) {
        return data == null ? null : BitmapFactory.decodeByteArray(data, 0, data.length);
    }

    private Typeface font(byte[] data, String cacheName) {
        if (data == null) {
            return null;
        }
        File f = new File(activity.getCacheDir(), cacheName);
        try (FileOutputStream out = new FileOutputStream(f)) {
            out.write(data);
        } catch (IOException e) {
            return null;
        }
        try {
            return Typeface.createFromFile(f);
        } catch (RuntimeException e) {
            return null;
        }
    }

    // ----- Drawing

    private float X(float x) {
        return ox + x * s;
    }

    private float Y(float y) {
        return oy + y * s;
    }

    private RectF R(float x0, float y0, float x1, float y1) {
        return new RectF(X(x0), Y(y0), X(x1), Y(y1));
    }

    private float time() {
        return (SystemClock.uptimeMillis() - start) / 1000f;
    }

    @Override
    protected void onDraw(Canvas c) {
        int w = getWidth(), h = getHeight();
        s = Math.min(w / KW, h / KH);
        ox = (w - KW * s) / 2;
        oy = (h - KH * s) / 2;
        float t = time();
        appear = Math.min(1f, appear + 0.12f);

        drawScene(c, w, h, t);

        float textW = 700;
        StaticLayout body = layout(text, bodyFont, 26, textW, TEXT);
        float textH = body.getHeight() / s;
        float panelH = buttons != null ? Math.max(300, 104 + textH + 18 + 58 + 70) : Math.max(300, 104 + textH + 18 + 130);
        float pw = 820, x0 = (KW - pw) / 2, x1 = x0 + pw;
        float y0 = Math.max(60, Math.min(KH * 0.62f - panelH / 2, Math.max(60, KH - 24 - panelH)));
        y0 += (1 - appear) * 16;
        float y1 = y0 + panelH;

        paint.setShader(null);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(argb(110, 0, 0, 0));
        c.drawRoundRect(R(x0 + 6, y0 + 10, x1 + 6, y1 + 10), 14 * s, 14 * s, paint);
        panel(c, x0, y0, x1, y1, argb(245, 66, 30, 96), argb(245, 26, 10, 40), 14);
        goldFrame(c, x0, y0, x1, y1, 14, 3, false, 0);
        diamond(c, x0 + 18, y0 + 18, 6, GOLD);
        diamond(c, x1 - 18, y0 + 18, 6, GOLD);
        diamond(c, x0 + 18, y1 - 18, 6, GOLD);
        diamond(c, x1 - 18, y1 - 18, 6, GOLD);

        textCentered(c, title, titleFont, 44, KW / 2, y0 + 26, GOLD_BRIGHT);
        paint.setColor(argb(150, 244, 204, 98));
        paint.setStrokeWidth(1.5f * s);
        c.drawLine(X(KW / 2 - 200), Y(y0 + 84), X(KW / 2 + 200), Y(y0 + 84), paint);
        diamond(c, KW / 2, y0 + 84, 5, GOLD);

        c.save();
        c.translate(X(KW / 2 - textW / 2), Y(y0 + 104));
        shadowed(c, body, layout(text, bodyFont, 26, textW, argb(150, 0, 0, 0)));
        c.restore();

        if (buttons != null) {
            drawButtons(c, y1, t);
        } else {
            drawProgress(c, y1, t);
        }
        postInvalidateOnAnimation();
    }

    private void drawScene(Canvas c, int w, int h, float t) {
        c.drawColor(rgb(10, 6, 18));
        paint.setShader(null);
        paint.setStyle(Paint.Style.FILL);
        paint.setAlpha(255);
        if (background != null) {
            float k = Math.max((float) w / background.getWidth(), (float) h / background.getHeight());
            float bw = background.getWidth() * k, bh = background.getHeight() * k;
            paint.setColor(0xFFFFFFFF);
            c.drawBitmap(background, null, new RectF((w - bw) / 2, (h - bh) / 2, (w + bw) / 2, (h + bh) / 2), paint);
        }
        paint.setColor(argb(120, 8, 4, 16));
        c.drawRect(0, 0, w, h, paint);
        if (moon != null) {
            float mh = KH * 0.62f, mw = mh * moon.getWidth() / moon.getHeight();
            float bob = (float) Math.sin(t * 0.6f) * 4f;
            // The moon keeps to the screen's right edge on phones wider than 16:9.
            float right = (w - ox) / s;
            float mx = right - mw * 0.82f, my = -mh * 0.18f + bob;
            paint.setColor(0xFFFFFFFF);
            paint.setAlpha(235);
            c.drawBitmap(moon, null, R(mx, my, mx + mw, my + mh), paint);
            paint.setAlpha(255);
        }
        paint.setShader(new LinearGradient(0, h * 0.55f, 0, h, 0, argb(170, 0, 0, 0), Shader.TileMode.CLAMP));
        c.drawRect(0, h * 0.55f, w, h, paint);
        paint.setShader(null);
        textCentered(c, "MAJORA'S MASK  ·  DEFINITIVE EDITION", titleFont, 30, KW / 2, 28, GOLD);
    }

    private void drawButtons(Canvas c, float bottom, float t) {
        int n = buttons.length;
        float bw = Math.min(250, (760 - (n - 1) * 20f) / n), bh = 58;
        float total = n * bw + (n - 1) * 20f;
        float x = KW / 2 - total / 2, y = bottom - bh - 44;
        for (int i = 0; i < n; i++) {
            float ax = x + i * (bw + 20), bx = ax + bw;
            buttonRects[i].set(X(ax), Y(y), X(bx), Y(y + bh));
            boolean on = selected == i;
            boolean down = pressed == i;
            float pulse = on ? 0.5f + 0.5f * (float) Math.sin(t * 4f) : 0f;
            panel(c, ax, y, bx, y + bh, down ? rgb(96, 44, 124) : on ? rgb(128, 66, 160) : rgb(74, 40, 96),
                    down ? rgb(56, 20, 80) : on ? rgb(76, 28, 104) : rgb(42, 20, 58), 10);
            goldFrame(c, ax, y, bx, y + bh, 10, on ? 2.5f : 1.5f, on, pulse);
            textCentered(c, buttons[i], titleFont, 30, (ax + bx) / 2, y + 12, on ? rgb(255, 246, 214) : DIM);
        }
        textCentered(c, "Toque para escolher  ·  D-pad e A também funcionam", bodyFont, 19, KW / 2, bottom - 30, HINT);
    }

    private void drawProgress(Canvas c, float bottom, float t) {
        float x0 = KW / 2 - 320, x1 = KW / 2 + 320, y = bottom - 112;
        paint.setShader(null);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(rgb(16, 6, 24));
        c.drawRoundRect(R(x0, y, x1, y + 30), 15 * s, 15 * s, paint);
        if (fraction >= 0f) {
            float f = Math.max(0f, Math.min(1f, fraction));
            if (f > 0f) {
                RectF bar = R(x0 + 3, y + 3, x0 + 3 + (x1 - x0 - 6) * f, y + 27);
                paint.setShader(new LinearGradient(0, bar.top, 0, bar.bottom, new int[] { GOLD_BRIGHT, GOLD, GOLD_DARK },
                        null, Shader.TileMode.CLAMP));
                c.drawRoundRect(bar, 12 * s, 12 * s, paint);
                paint.setShader(null);
            }
            float sweep = (t * 0.6f % 1f) * f;
            float sx = x0 + 3 + (x1 - x0 - 6) * sweep;
            paint.setColor(argb(60, 255, 255, 230));
            c.drawRect(R(Math.max(x0 + 3, sx - 30), y + 3, sx, y + 27), paint);
            textCentered(c, Math.round(f * 100) + "%", titleFont, 30, KW / 2, y + 40, GOLD_BRIGHT);
        } else {
            float k = 0.5f + 0.5f * (float) Math.sin(t * 2.4f);
            float px = x0 + 3 + (x1 - x0 - 126) * k;
            paint.setColor(GOLD);
            c.drawRoundRect(R(px, y + 3, px + 120, y + 27), 12 * s, 12 * s, paint);
            textCentered(c, "Preparando...", bodyFont, 24, KW / 2, y + 42, DIM);
        }
        goldFrame(c, x0, y, x1, y + 30, 15, 2, false, 0);
        textCentered(c, "Não feche o app até terminar.", bodyFont, 19, KW / 2, bottom - 30, HINT);
    }

    private void panel(Canvas c, float x0, float y0, float x1, float y1, int top, int bottom, float r) {
        paint.setStyle(Paint.Style.FILL);
        RectF rect = R(x0, y0, x1, y1);
        paint.setShader(new LinearGradient(0, rect.top, 0, rect.bottom, top, bottom, Shader.TileMode.CLAMP));
        c.drawRoundRect(rect, r * s, r * s, paint);
        paint.setShader(null);
    }

    private void goldFrame(Canvas c, float x0, float y0, float x1, float y1, float r, float w, boolean glow,
                           float pulse) {
        paint.setShader(null);
        paint.setStyle(Paint.Style.STROKE);
        if (glow) {
            for (int i = 5; i >= 1; i--) {
                paint.setColor(argb(20 * (6 - i), 255, 210, 90));
                paint.setStrokeWidth(3 * s);
                c.drawRoundRect(R(x0 - i * 2, y0 - i * 2, x1 + i * 2, y1 + i * 2), (r + i * 2) * s, (r + i * 2) * s,
                        paint);
            }
            paint.setColor(argb((int) (90 * pulse), 255, 226, 140));
            paint.setStrokeWidth(2 * s);
            c.drawRoundRect(R(x0 - 3, y0 - 3, x1 + 3, y1 + 3), (r + 2) * s, (r + 2) * s, paint);
        }
        paint.setColor(argb(230, 40, 20, 10));
        paint.setStrokeWidth((w + 2) * s);
        c.drawRoundRect(R(x0, y0, x1, y1), r * s, r * s, paint);
        paint.setColor(GOLD);
        paint.setStrokeWidth(w * s);
        c.drawRoundRect(R(x0, y0, x1, y1), r * s, r * s, paint);
        paint.setColor(argb(110, 255, 240, 180));
        paint.setStrokeWidth(Math.max(1f, s));
        c.drawRoundRect(R(x0 + w * 0.5f, y0 + w * 0.5f, x1 - w * 0.5f, y1 - w * 0.5f), r * s, r * s, paint);
        paint.setStyle(Paint.Style.FILL);
    }

    private void diamond(Canvas c, float x, float y, float d, int color) {
        Path p = new Path();
        p.moveTo(X(x), Y(y - d));
        p.lineTo(X(x + d), Y(y));
        p.lineTo(X(x), Y(y + d));
        p.lineTo(X(x - d), Y(y));
        p.close();
        paint.setShader(null);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(color);
        c.drawPath(p, paint);
    }

    // One line of text centered on cx, its top at y, with a soft shadow like the PC screen's.
    private void textCentered(Canvas c, String str, Typeface face, float size, float cx, float y, int color) {
        textPaint.setTypeface(face);
        textPaint.setTextSize(size * s);
        textPaint.setTextAlign(Paint.Align.CENTER);
        Paint.FontMetrics fm = textPaint.getFontMetrics();
        float base = Y(y) - fm.ascent;
        textPaint.setColor(argb(150, 0, 0, 0));
        c.drawText(str, X(cx) + 1.5f * s, base + 2 * s, textPaint);
        textPaint.setColor(color);
        c.drawText(str, X(cx), base, textPaint);
        textPaint.setTextAlign(Paint.Align.LEFT);
    }

    @SuppressWarnings("deprecation")
    private StaticLayout layout(String str, Typeface face, float size, float width, int color) {
        TextPaint p = new TextPaint(Paint.ANTI_ALIAS_FLAG);
        p.setTypeface(face);
        p.setTextSize(size * s);
        p.setColor(color);
        return new StaticLayout(str, p, Math.max(1, (int) (width * s)), Layout.Alignment.ALIGN_CENTER, 1.15f, 0f,
                false);
    }

    private void shadowed(Canvas c, StaticLayout text, StaticLayout shadow) {
        c.save();
        c.translate(1.5f * s, 2 * s);
        shadow.draw(c);
        c.restore();
        text.draw(c);
    }

    // ----- Input: touch, the controller and the keyboard

    private void choose(int index) {
        if (answer == null) {
            return;
        }
        Answer a = answer;
        answer = null;
        buttons = null;
        // Between the answer and the game's next step, the screen stays with a waiting bar.
        fraction = -1f;
        title = "";
        text = "";
        pressed = -1;
        a.chosen(index);
        invalidate();
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (buttons == null) {
            return true; // nothing under the screen reacts while it is up
        }
        int hit = -1;
        for (int i = 0; i < buttons.length; i++) {
            if (buttonRects[i].contains(e.getX(), e.getY())) {
                hit = i;
            }
        }
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_MOVE:
                pressed = hit;
                if (hit >= 0) {
                    selected = hit;
                }
                break;
            case MotionEvent.ACTION_UP:
                pressed = -1;
                if (hit >= 0) {
                    choose(hit);
                }
                break;
            case MotionEvent.ACTION_CANCEL:
                pressed = -1;
                break;
        }
        invalidate();
        return true;
    }

    // Keys from MainActivity.dispatchKeyEvent while the screen is up. All of them stop here.
    boolean handleKey(KeyEvent e) {
        int code = e.getKeyCode();
        if (code == KeyEvent.KEYCODE_VOLUME_UP || code == KeyEvent.KEYCODE_VOLUME_DOWN
                || code == KeyEvent.KEYCODE_VOLUME_MUTE) {
            return false;
        }
        if (buttons == null || e.getAction() != KeyEvent.ACTION_DOWN) {
            return true;
        }
        int n = buttons.length;
        switch (code) {
            case KeyEvent.KEYCODE_DPAD_LEFT:
            case KeyEvent.KEYCODE_DPAD_UP:
                selected = (selected + n - 1) % n;
                break;
            case KeyEvent.KEYCODE_DPAD_RIGHT:
            case KeyEvent.KEYCODE_DPAD_DOWN:
            case KeyEvent.KEYCODE_TAB:
                selected = (selected + 1) % n;
                break;
            case KeyEvent.KEYCODE_BUTTON_A:
            case KeyEvent.KEYCODE_BUTTON_START:
            case KeyEvent.KEYCODE_DPAD_CENTER:
            case KeyEvent.KEYCODE_ENTER:
            case KeyEvent.KEYCODE_NUMPAD_ENTER:
            case KeyEvent.KEYCODE_SPACE:
                if (e.getRepeatCount() == 0) {
                    choose(selected);
                }
                break;
            case KeyEvent.KEYCODE_BUTTON_B:
            case KeyEvent.KEYCODE_BACK:
            case KeyEvent.KEYCODE_ESCAPE:
                if (cancel >= 0 && e.getRepeatCount() == 0) {
                    choose(cancel);
                }
                break;
            default:
                break;
        }
        invalidate();
        return true;
    }

    // The left stick and the hat move the choice too.
    boolean handleMotion(MotionEvent e) {
        if (buttons == null || (e.getSource() & InputDevice.SOURCE_CLASS_JOYSTICK) == 0) {
            return true;
        }
        float x = e.getAxisValue(MotionEvent.AXIS_X);
        float hat = e.getAxisValue(MotionEvent.AXIS_HAT_X);
        float v = Math.abs(hat) > 0.5f ? hat : x;
        int dir = v < -0.5f ? -1 : v > 0.5f ? 1 : 0;
        if (dir != 0 && dir != stickDir) {
            int n = buttons.length;
            selected = (selected + n + dir) % n;
            invalidate();
        }
        stickDir = dir;
        return true;
    }

    static boolean dispatchKey(KeyEvent e) {
        return sOverlay != null && sOverlay.getParent() != null && sOverlay.handleKey(e);
    }

    static boolean dispatchMotion(MotionEvent e) {
        return sOverlay != null && sOverlay.getParent() != null && sOverlay.handleMotion(e);
    }
}
