package dev.wakamex.gesso;

import android.app.Activity;
import android.os.Build;
import android.view.WindowInsets;
import java.nio.charset.StandardCharsets;
import org.libsdl.app.SDLActivity;

// The on-screen keyboard, for gs_ui (src/gs_android.c): its height, which SDL's safe area leaves out,
// so that a field at the bottom of the window does not sit under it; and where it is to compose.
public final class Keyboard {
    // In pixels from the bottom of the window; 0 when it is hidden, or before Android 11, which has no
    // inset for it.
    public static int height(Activity activity) {
        if (Build.VERSION.SDK_INT < 30) return 0;
        WindowInsets insets = activity.getWindow().getDecorView().getRootWindowInsets();
        return insets == null ? 0 : insets.getInsets(WindowInsets.Type.ime()).bottom;
    }

    // Starts the keyboard's input again at the app's cursor, `before` being the text before it.
    public static void restart(byte[] before) {
        SDLActivity.restartTextInput(new String(before, StandardCharsets.UTF_8));
    }
}
