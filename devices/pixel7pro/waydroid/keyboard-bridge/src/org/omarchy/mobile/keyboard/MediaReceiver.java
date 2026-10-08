package org.omarchy.mobile.keyboard;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

/**
 * The media card's buttons: the watcher's "media CMD [POSITION]" from the
 * phone's MPRIS bridge, as a broadcast with extras cmd and position (ms).
 * Like ReplyReceiver it needs WRITE_SECURE_SETTINGS (the shell's user), so
 * other Android apps cannot send it.
 */
public class MediaReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        MediaBridge bridge = MediaBridge.instance;
        String cmd = intent.getStringExtra("cmd");
        if ("test".equals(cmd)) {
            TestSession.start(context.getApplicationContext());
            return;
        }
        if ("test-end".equals(cmd)) {
            TestSession.end();
            return;
        }
        if (bridge == null || cmd == null) {
            Log.w(MediaBridge.TAG, "command without a bridge or a command");
            return;
        }
        Log.i(MediaBridge.TAG + "Cmd", cmd + ": " + bridge.command(cmd, intent.getLongExtra("position", -1)));
    }
}
