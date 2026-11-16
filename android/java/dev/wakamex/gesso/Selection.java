package dev.wakamex.gesso;

import android.app.Activity;
import android.graphics.Rect;
import android.os.Build;
import android.view.ActionMode;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;

// The floating toolbar over a selection, as Android shows over selected text (src/gs_android.c):
// Copy, and Cut, Paste and Select all in a field. gs_ui draws the text and the selection; a choice
// comes back to it as the keyboard shortcut for it.
public final class Selection {
    static final int COPY = 1, CUT = 2, PASTE = 3, SELECT_ALL = 4;

    static native void chosen(int action);
    static native void closed();

    private static ActionMode mode;
    private static final Rect where = new Rect();
    private static boolean editable, selected;

    // Shows it over x, y, w, h (pixels in the window), or moves it there.
    public static void show(final Activity activity, final int x, final int y, final int w, final int h, final boolean inField, final boolean anything) {
        if (Build.VERSION.SDK_INT < 23) return;
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                where.set(x, y, x + w, y + h);
                editable = inField;
                selected = anything;
                if (mode != null) {
                    mode.invalidate();
                    mode.invalidateContentRect();
                    return;
                }
                View view = activity.getWindow().getDecorView();
                mode = view.startActionMode(new ActionMode.Callback2() {
                    @Override
                    public boolean onCreateActionMode(ActionMode m, Menu menu) {
                        menu.add(Menu.NONE, CUT, 1, android.R.string.cut);
                        menu.add(Menu.NONE, COPY, 2, android.R.string.copy);
                        menu.add(Menu.NONE, PASTE, 3, android.R.string.paste);
                        menu.add(Menu.NONE, SELECT_ALL, 4, android.R.string.selectAll);
                        return true;
                    }

                    @Override
                    public boolean onPrepareActionMode(ActionMode m, Menu menu) {
                        menu.findItem(CUT).setVisible(editable && selected);
                        menu.findItem(COPY).setVisible(selected);
                        menu.findItem(PASTE).setVisible(editable);
                        menu.findItem(SELECT_ALL).setVisible(editable);
                        return true;
                    }

                    @Override
                    public boolean onActionItemClicked(ActionMode m, MenuItem item) {
                        chosen(item.getItemId());
                        m.finish();
                        return true;
                    }

                    @Override
                    public void onDestroyActionMode(ActionMode m) {
                        mode = null;
                        closed();
                    }

                    @Override
                    public void onGetContentRect(ActionMode m, View v, Rect out) {
                        out.set(where);
                    }
                }, ActionMode.TYPE_FLOATING);
            }
        });
    }

    public static void hide(Activity activity) {
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (mode != null) mode.finish();
            }
        });
    }
}
