package org.omarchy.mobile.keyboard;

import android.app.Notification;
import android.app.PendingIntent;
import android.app.RemoteInput;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;
import android.service.notification.NotificationListenerService;
import android.service.notification.StatusBarNotification;

import java.util.ArrayList;
import java.util.List;

/**
 * Replies to Android notifications from the phone's notification shade.
 * Waydroid's notification bridge passes an app's actions to the shell as
 * plain buttons, so a reply action (one with a RemoteInput, as WhatsApp's and
 * Signal's) fired from there carried no text. This listener finds the
 * notification again and fires its reply action with the text filled in, as
 * a watch or a car does. ReplyReceiver hands it the request.
 *
 * Notification access is granted by waydroid-container.sh's watcher
 * (cmd notification allow_listener).
 */
public class ReplyService extends NotificationListenerService {
    static final String TAG = "OmarchyReply";
    static volatile ReplyService instance;

    private MediaBridge media;

    @Override
    public void onListenerConnected() {
        instance = this;
        // Notification access also lets the media card follow other apps'
        // media sessions.
        media = new MediaBridge(this, new ComponentName(this, ReplyService.class));
        media.start();
    }

    @Override
    public void onListenerDisconnected() {
        if (instance == this)
            instance = null;
        if (media != null) {
            media.stop();
            media = null;
        }
    }

    private static Notification.Action replyAction(Notification notification) {
        if (notification.actions == null)
            return null;
        for (Notification.Action action : notification.actions) {
            RemoteInput[] inputs = action.getRemoteInputs();
            if (action.actionIntent == null || inputs == null)
                continue;
            for (RemoteInput input : inputs)
                if (input.getAllowFreeFormInput())
                    return action;
        }
        return null;
    }

    private static String title(Notification notification) {
        CharSequence title = notification.extras.getCharSequence(Notification.EXTRA_TITLE);
        return title == null ? "" : title.toString();
    }

    /**
     * Sends text through the reply action of PACKAGE's notification titled
     * TITLE (the newest such). With no title match, only a lone repliable
     * notification of the package is used: guessing between conversations
     * could send the text to the wrong one. Returns what happened, for the log
     * (never the text).
     */
    String reply(String packageName, String wantedTitle, String text) {
        StatusBarNotification[] active = getActiveNotifications();
        if (active == null)
            return "no notifications";
        List<StatusBarNotification> repliable = new ArrayList<>();
        StatusBarNotification match = null;
        for (StatusBarNotification sbn : active) {
            if (!sbn.getPackageName().equals(packageName) || replyAction(sbn.getNotification()) == null)
                continue;
            repliable.add(sbn);
            if (title(sbn.getNotification()).equals(wantedTitle)
                    && (match == null || sbn.getPostTime() > match.getPostTime()))
                match = sbn;
        }
        if (match == null && repliable.size() == 1)
            match = repliable.get(0);
        if (match == null)
            return repliable.isEmpty() ? "no repliable notification" : "no notification titled as asked";
        Notification.Action action = replyAction(match.getNotification());
        RemoteInput[] inputs = action.getRemoteInputs();
        Bundle results = new Bundle();
        for (RemoteInput input : inputs)
            if (input.getAllowFreeFormInput())
                results.putCharSequence(input.getResultKey(), text);
        Intent intent = new Intent();
        RemoteInput.addResultsToIntent(inputs, intent, results);
        RemoteInput.setResultsSource(intent, RemoteInput.SOURCE_FREE_FORM_INPUT);
        try {
            action.actionIntent.send(this, 0, intent);
        } catch (PendingIntent.CanceledException e) {
            return "reply action canceled";
        }
        return "sent";
    }
}
