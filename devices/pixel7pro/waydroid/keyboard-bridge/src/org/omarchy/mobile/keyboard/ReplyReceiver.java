package org.omarchy.mobile.keyboard;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Base64;
import android.util.Log;

import java.nio.charset.StandardCharsets;

/**
 * A reply from the phone's notification shade, sent by waydroid-container.sh's
 * watcher as root:
 *   am broadcast -n org.omarchy.mobile.keyboard/.ReplyReceiver
 *       --es package PACKAGE --es title64 BASE64 --es text64 BASE64
 * (title and text are UTF-8 in base64). The receiver needs
 * WRITE_SECURE_SETTINGS, which no ordinary app holds, so other Android apps
 * cannot send messages through it.
 */
public class ReplyReceiver extends BroadcastReceiver {
    private static String decode(String value) {
        if (value == null)
            return "";
        try {
            return new String(Base64.decode(value, Base64.DEFAULT), StandardCharsets.UTF_8);
        } catch (IllegalArgumentException e) {
            return "";
        }
    }

    @Override
    public void onReceive(Context context, Intent intent) {
        String packageName = intent.getStringExtra("package");
        String title = decode(intent.getStringExtra("title64"));
        String text = decode(intent.getStringExtra("text64"));
        ReplyService service = ReplyService.instance;
        if (packageName == null || text.isEmpty()) {
            Log.w(ReplyService.TAG, "reply: no package or text");
        } else if (service == null) {
            Log.w(ReplyService.TAG, "reply " + packageName + ": the listener is not connected");
        } else {
            Log.i(ReplyService.TAG, "reply " + packageName + ": " + service.reply(packageName, title, text));
        }
    }
}
