package org.omarchy.mobile.keyboard;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/**
 * Closes the keyboard bridge, and only that: the shell asks when an Android
 * app with its keyboard up leaves the screen, so the app lays itself out at
 * full height again before the switcher pictures it and Android sleeps
 * (Back could reach the app instead, if the keyboard had already closed).
 * Sent by waydroid-container.sh's watcher as root:
 *   am broadcast -n org.omarchy.mobile.keyboard/.HideReceiver
 * Like ReplyReceiver it needs WRITE_SECURE_SETTINGS.
 */
public class HideReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        BridgeIme ime = BridgeIme.instance;
        if (ime != null)
            ime.requestHideSelf(0);
    }
}
