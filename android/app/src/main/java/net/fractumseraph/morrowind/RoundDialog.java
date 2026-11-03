package net.fractumseraph.morrowind;

import android.app.Activity;
import android.app.AlertDialog;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * A dialog that fits a round watch's display: its title, words, an optional view (a text box, a progress bar) and
 * its buttons one under another, all in a scrolling column kept inside the circle (the system's dialog put its
 * buttons along the bottom, where the circle cut them off). On a phone it is the same column, which reads as well.
 */
final class RoundDialog {
    /** A button: its word, and what it does (null: only closes the dialog) */
    static final class Choice {
        final String label;
        final Runnable action;

        Choice(String label, Runnable action) {
            this.label = label;
            this.action = action;
        }
    }

    private RoundDialog() {
    }

    private static int dp(Activity activity, float value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value,
            activity.getResources().getDisplayMetrics());
    }

    /** Shows it; onDismiss (or null) runs however it closes, after a button's action */
    static AlertDialog show(Activity activity, String title, String message, View extra, boolean cancelable,
            Runnable onDismiss, Choice... choices) {
        boolean round = activity.getResources().getConfiguration().isScreenRound();
        // (a round display's corners are outside the circle: the column keeps a sixth of the width clear each side, an
        // eighth of the height above the title and a quarter below the last button, so it scrolls up into the middle)
        int width = activity.getResources().getDisplayMetrics().widthPixels;
        int height = activity.getResources().getDisplayMetrics().heightPixels;
        int side = round ? width / 6 : dp(activity, 20);
        LinearLayout column = new LinearLayout(activity);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setPadding(side, round ? height / 8 : dp(activity, 16), side, round ? height / 4 : dp(activity, 16));
        column.setGravity(Gravity.CENTER_HORIZONTAL);
        if (title != null && !title.isEmpty()) {
            TextView t = new TextView(activity);
            t.setText(title);
            t.setTextSize(TypedValue.COMPLEX_UNIT_SP, round ? 14 : 16);
            t.setGravity(Gravity.CENTER);
            t.setPadding(0, 0, 0, dp(activity, 6));
            column.addView(t);
        }
        if (message != null && !message.isEmpty()) {
            TextView m = new TextView(activity);
            m.setText(message);
            m.setTextSize(TypedValue.COMPLEX_UNIT_SP, round ? 12 : 13);
            m.setGravity(Gravity.CENTER);
            m.setPadding(0, 0, 0, dp(activity, 8));
            column.addView(m);
        }
        if (extra != null)
            column.addView(extra, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        ScrollView scroll = new ScrollView(activity);
        scroll.addView(column);
        AlertDialog dialog = new AlertDialog.Builder(activity)
            .setView(scroll)
            .setCancelable(cancelable)
            .create();
        for (Choice choice : choices) {
            Button b = new Button(activity);
            b.setText(choice.label);
            b.setAllCaps(false);
            b.setTextColor(0xFF101010);     // (the theme's light buttons came with white words)
            b.setOnClickListener(v -> {
                // (the action first: onDismiss then sees what it did)
                if (choice.action != null)
                    choice.action.run();
                dialog.dismiss();
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
            lp.topMargin = dp(activity, 4);
            column.addView(b, lp);
        }
        if (onDismiss != null)
            dialog.setOnDismissListener(d -> onDismiss.run());
        dialog.show();
        // (the bezel scrolls the column: Wear OS sends its turns to the view with the focus)
        scroll.setFocusable(true);
        scroll.setFocusableInTouchMode(true);
        if (extra == null)
            scroll.requestFocus();
        return dialog;
    }
}
