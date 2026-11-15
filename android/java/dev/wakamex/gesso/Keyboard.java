package dev.wakamex.gesso;

import android.app.Activity;
import android.os.Build;
import android.view.WindowInsets;

// The on-screen keyboard's height, for gs_ui (src/gs_android.c): SDL's safe area leaves it out, so a
// field at the bottom of the window would sit under it.
public final class Keyboard {
    // In pixels from the bottom of the window; 0 when it is hidden, or before Android 11, which has no
    // inset for it.
    public static int height(Activity activity) {
        if (Build.VERSION.SDK_INT < 30) return 0;
        WindowInsets insets = activity.getWindow().getDecorView().getRootWindowInsets();
        return insets == null ? 0 : insets.getInsets(WindowInsets.Type.ime()).bottom;
    }
}
