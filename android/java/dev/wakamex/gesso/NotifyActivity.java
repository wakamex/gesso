package dev.wakamex.gesso;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

// Opened by a click on one of Notify's notifications: hands its tag to the app while the app runs,
// brings the app's task forward, and closes. Declared in the app's manifest (see gs_notify.h).
public final class NotifyActivity extends Activity {
    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        String tag = getIntent().getStringExtra(Notify.TAG);
        if (tag != null && Notify.ready) Notify.clicked(tag);
        Intent app = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (app != null) startActivity(app.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED));
        finish();
    }
}
