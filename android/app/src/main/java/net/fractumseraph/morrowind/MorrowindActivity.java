package net.fractumseraph.morrowind;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.text.InputType;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.WindowManager;
import android.widget.EditText;

import java.util.concurrent.ArrayBlockingQueue;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLControllerManager;

/**
 * The game (libmain.so, android/src) in SDL's activity, with what native code cannot see by itself: whether the
 * display is round, a watch's turning bezel, its back button, and the system's keyboard for the game's text boxes.
 */
public class MorrowindActivity extends SDLActivity {
    private static MorrowindActivity instance;

    /** bezel clicks, clockwise positive (android_input.cpp) */
    public static native void nativeRotary(float clicks);

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        instance = this;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        // A round display (a watch's): the game's screens are fitted inside its circle (android_input.cpp). Set by
        // SDL's native code, before SDL starts the game's thread (once the surface is there)
        try {
            SDLActivity.nativeSetenv("MW3DS_ROUND", getResources().getConfiguration().isScreenRound() ? "1" : "0");
        } catch (UnsatisfiedLinkError e) {
            // (SDL's libraries did not load: SDLActivity shows why)
        }
        // a new version looked for while the game starts (a release's build only)
        Updater.start(this);
    }

    /** a watch's turning bezel or crown (clockwise is negative, as Wear OS scrolls a list down for it) */
    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (event.getAction() == MotionEvent.ACTION_SCROLL && event.isFromSource(InputDevice.SOURCE_ROTARY_ENCODER)) {
            float clicks = -event.getAxisValue(MotionEvent.AXIS_SCROLL);
            if (Float.isFinite(clicks) && clicks != 0) {
                try {
                    nativeRotary(clicks);
                } catch (UnsatisfiedLinkError e) {
                    // (the game is not loaded)
                }
            }
            return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }

    /**
     * The watch's back button to the game, whichever view has the focus (SDL hears keys only through its surface's
     * listener); a controller's back button goes on as SDL's joystick handling has it
     */
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK
                && !SDLControllerManager.isDeviceSDLJoystick(event.getDeviceId())) {
            if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0)
                SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_BACK);
            else if (event.getAction() == KeyEvent.ACTION_UP)
                SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_BACK);
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    /**
     * A short buzz for a press of the watch's controls or something felt in the game (android_input.cpp), from the
     * game's thread: times of them, as long apart as each lasts
     */
    public static void buzz(int milliseconds, int times) {
        MorrowindActivity activity = instance;
        Vibrator vibrator = activity != null ? activity.getSystemService(Vibrator.class) : null;
        if (vibrator == null || !vibrator.hasVibrator())
            return;
        if (times <= 1) {
            vibrator.vibrate(VibrationEffect.createOneShot(milliseconds, VibrationEffect.DEFAULT_AMPLITUDE));
            return;
        }
        long[] timings = new long[times * 2];
        for (int i = 0; i < times; i++) {
            timings[i * 2] = i == 0 ? 0 : milliseconds;
            timings[i * 2 + 1] = milliseconds;
        }
        vibrator.vibrate(VibrationEffect.createWaveform(timings, -1));
    }

    /**
     * The 3DS's system font, which only a console has, as the watch's own (gles_gpu.cpp C2D_DrawText): characters 32 to
     * 255 in a grid of 16 columns, each in a cell cellW x cellH (the game lays the system font's text out with every
     * character the same width), white on clear, as ARGB pixels, the top row first
     */
    public static int[] renderGlyphs(int cellW, int cellH) {
        int columns = 16, rows = 14;
        Bitmap atlas = Bitmap.createBitmap(columns * cellW, rows * cellH, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(atlas);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(0xFFFFFFFF);
        paint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.NORMAL));
        paint.setTextSize(cellH * 0.62f);
        Paint.FontMetrics metrics = paint.getFontMetrics();
        float baseline = (cellH - (metrics.descent - metrics.ascent)) / 2 - metrics.ascent;
        for (int c = 32; c < 256; c++) {
            if (c >= 127 && c < 160)
                continue;
            String text = String.valueOf((char) c);
            int at = c - 32;
            float x = (at % columns) * cellW, y = (at / columns) * cellH;
            float width = paint.measureText(text);
            // (a character wider than its cell squeezed into it)
            float scale = width > cellW * 0.92f ? cellW * 0.92f / width : 1.0f;
            canvas.save();
            canvas.translate(x + cellW / 2.0f, y + baseline);
            canvas.scale(scale, 1.0f);
            canvas.drawText(text, -width / 2.0f, 0, paint);
            canvas.restore();
        }
        int[] pixels = new int[atlas.getWidth() * atlas.getHeight()];
        atlas.getPixels(pixels, 0, atlas.getWidth(), 0, 0, atlas.getWidth(), atlas.getHeight());
        atlas.recycle();
        return pixels;
    }

    /**
     * The game's text box (the 3DS's software keyboard: swkbdInputText, android_input.cpp): a dialog with the system's
     * keyboard, while the game's thread waits. The text, or null when cancelled
     */
    public static String askText(String hint, String initial, boolean password, boolean numbers) {
        MorrowindActivity activity = instance;
        if (activity == null)
            return null;
        ArrayBlockingQueue<String[]> answer = new ArrayBlockingQueue<>(1);
        activity.runOnUiThread(() -> {
            EditText field = new EditText(activity);
            field.setText(initial);
            field.setHint(hint);
            field.setSingleLine(true);
            int type = numbers ? InputType.TYPE_CLASS_NUMBER : InputType.TYPE_CLASS_TEXT;
            if (password)
                type |= numbers ? InputType.TYPE_NUMBER_VARIATION_PASSWORD : InputType.TYPE_TEXT_VARIATION_PASSWORD;
            field.setInputType(type);
            field.setSelection(field.getText().length());
            final boolean[] given = { false };
            RoundDialog.show(activity, hint == null || hint.isEmpty() ? "Text" : hint, null, field, true,
                () -> {
                    if (!given[0])
                        answer.offer(new String[] { null });
                },
                new RoundDialog.Choice("OK", () -> {
                    given[0] = true;
                    answer.offer(new String[] { field.getText().toString() });
                }),
                new RoundDialog.Choice("Cancel", () -> {
                    given[0] = true;
                    answer.offer(new String[] { null });
                }));
            field.requestFocus();
        });
        try {
            return answer.take()[0];
        } catch (InterruptedException e) {
            return null;
        }
    }
}
