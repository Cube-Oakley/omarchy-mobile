package org.omarchy.mobile.keyboard;

import android.content.ComponentName;
import android.content.Context;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.media.MediaMetadata;
import android.media.session.MediaController;
import android.media.session.MediaSessionManager;
import android.media.session.PlaybackState;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.List;

/**
 * What Android is playing, for the phone's media card (shade, lock screen):
 * the session that plays, else the most recent one, as a JSON line in the
 * log under OmarchyMedia whenever it changes. waydroid-container.sh's watcher
 * keeps the newest line in a file the phone's MPRIS bridge
 * (omarchy-mobile-android-media) publishes; the artwork goes to this app's
 * files (media-art.png), which the watcher copies out. Commands come back
 * through MediaReceiver. Reading other apps' sessions needs notification
 * access, which ReplyService has.
 */
class MediaBridge extends MediaController.Callback
        implements MediaSessionManager.OnActiveSessionsChangedListener {
    static final String TAG = "OmarchyMedia";
    static volatile MediaBridge instance;
    private static final int ART_SIZE = 512;

    private final Context context;
    private final ComponentName listener;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private MediaSessionManager manager;
    private MediaController current;
    private String lastLine = "";
    private String lastArt = "";
    private int artVersion;

    MediaBridge(Context context, ComponentName listener) {
        this.context = context;
        this.listener = listener;
    }

    void start() {
        manager = context.getSystemService(MediaSessionManager.class);
        try {
            manager.addOnActiveSessionsChangedListener(this, listener, handler);
            onActiveSessionsChanged(manager.getActiveSessions(listener));
        } catch (SecurityException e) {
            Log.w(TAG, "no notification access");
            return;
        }
        instance = this;
    }

    void stop() {
        if (instance == this)
            instance = null;
        if (manager != null)
            manager.removeOnActiveSessionsChangedListener(this);
        follow(null);
    }

    private static boolean playing(MediaController controller) {
        PlaybackState state = controller.getPlaybackState();
        return state != null && (state.getState() == PlaybackState.STATE_PLAYING
                || state.getState() == PlaybackState.STATE_BUFFERING);
    }

    @Override
    public void onActiveSessionsChanged(List<MediaController> controllers) {
        MediaController pick = null;
        if (controllers != null) {
            for (MediaController controller : controllers)
                if (playing(controller)) {
                    pick = controller;
                    break;
                }
            if (pick == null && !controllers.isEmpty())
                pick = controllers.get(0);
        }
        follow(pick);
    }

    private void follow(MediaController controller) {
        boolean same = current != null && controller != null
                && current.getSessionToken().equals(controller.getSessionToken());
        if (!same) {
            if (current != null)
                current.unregisterCallback(this);
            current = controller;
            if (current != null)
                current.registerCallback(this, handler);
        }
        report();
    }

    @Override
    public void onPlaybackStateChanged(PlaybackState state) {
        // Another session may have started playing; the list says which.
        if (manager != null && !playing(current))
            onActiveSessionsChanged(manager.getActiveSessions(listener));
        else
            report();
    }

    @Override
    public void onMetadataChanged(MediaMetadata metadata) {
        report();
    }

    @Override
    public void onSessionDestroyed() {
        if (current != null)
            current.unregisterCallback(this);
        current = null;
        if (manager != null)
            onActiveSessionsChanged(manager.getActiveSessions(listener));
    }

    private static String text(MediaMetadata metadata, String... keys) {
        for (String key : keys) {
            CharSequence value = metadata.getText(key);
            if (value != null && value.length() > 0)
                return value.toString();
        }
        return "";
    }

    private String label(String packageName) {
        PackageManager pm = context.getPackageManager();
        try {
            return pm.getApplicationLabel(pm.getApplicationInfo(packageName, 0)).toString();
        } catch (PackageManager.NameNotFoundException e) {
            return packageName;
        }
    }

    /** Saves the artwork when it changed; returns its version, 0 for none. */
    private int art(MediaMetadata metadata, String key) {
        Bitmap bitmap = metadata.getBitmap(MediaMetadata.METADATA_KEY_ART);
        if (bitmap == null)
            bitmap = metadata.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART);
        if (bitmap == null)
            bitmap = metadata.getBitmap(MediaMetadata.METADATA_KEY_DISPLAY_ICON);
        if (bitmap == null)
            return 0;
        key += "/" + bitmap.getWidth() + "x" + bitmap.getHeight() + "/" + bitmap.getGenerationId();
        if (key.equals(lastArt))
            return artVersion;
        int width = bitmap.getWidth(), height = bitmap.getHeight();
        if (Math.max(width, height) > ART_SIZE) {
            float scale = (float) ART_SIZE / Math.max(width, height);
            bitmap = Bitmap.createScaledBitmap(bitmap, Math.max(1, Math.round(width * scale)),
                    Math.max(1, Math.round(height * scale)), true);
        }
        File temp = new File(context.getFilesDir(), "media-art.png.tmp");
        try (FileOutputStream out = new FileOutputStream(temp)) {
            bitmap.compress(Bitmap.CompressFormat.PNG, 100, out);
        } catch (IOException e) {
            return 0;
        }
        if (!temp.renameTo(new File(context.getFilesDir(), "media-art.png")))
            return 0;
        // World-readable is not needed: the watcher copies it as root.
        lastArt = key;
        return ++artVersion;
    }

    private void report() {
        JSONObject o = new JSONObject();
        try {
            MediaController controller = current;
            MediaMetadata metadata = controller == null ? null : controller.getMetadata();
            PlaybackState state = controller == null ? null : controller.getPlaybackState();
            if (controller == null || (metadata == null && state == null)) {
                o.put("state", "none");
            } else {
                String packageName = controller.getPackageName();
                o.put("package", packageName);
                o.put("app", label(packageName));
                String status = "stopped";
                long position = 0, actions = 0;
                float speed = 1;
                if (state != null) {
                    switch (state.getState()) {
                    case PlaybackState.STATE_PLAYING:
                    case PlaybackState.STATE_BUFFERING:
                        status = "playing";
                        break;
                    case PlaybackState.STATE_PAUSED:
                        status = "paused";
                        break;
                    default:
                        break;
                    }
                    position = state.getPosition();
                    speed = state.getPlaybackSpeed();
                    if (status.equals("playing") && state.getLastPositionUpdateTime() > 0)
                        position += (long) ((SystemClock.elapsedRealtime()
                                - state.getLastPositionUpdateTime()) * speed);
                    actions = state.getActions();
                }
                o.put("state", status);
                o.put("position", Math.max(0, position));
                o.put("speed", speed);
                o.put("next", (actions & PlaybackState.ACTION_SKIP_TO_NEXT) != 0);
                o.put("previous", (actions & PlaybackState.ACTION_SKIP_TO_PREVIOUS) != 0);
                o.put("seek", (actions & PlaybackState.ACTION_SEEK_TO) != 0);
                if (metadata != null) {
                    String title = text(metadata, MediaMetadata.METADATA_KEY_TITLE,
                            MediaMetadata.METADATA_KEY_DISPLAY_TITLE);
                    String album = text(metadata, MediaMetadata.METADATA_KEY_ALBUM);
                    o.put("title", title);
                    o.put("artist", text(metadata, MediaMetadata.METADATA_KEY_ARTIST,
                            MediaMetadata.METADATA_KEY_ALBUM_ARTIST, MediaMetadata.METADATA_KEY_DISPLAY_SUBTITLE));
                    o.put("album", album);
                    o.put("duration", Math.max(0, metadata.getLong(MediaMetadata.METADATA_KEY_DURATION)));
                    o.put("art", art(metadata, packageName + "/" + title + "/" + album));
                }
            }
        } catch (JSONException e) {
            return;
        }
        String line = o.toString();
        if (!line.equals(lastLine)) {
            lastLine = line;
            Log.i(TAG, line);
        }
    }

    /** CMD: play, pause, toggle, next, previous or seek (to POSITION ms). */
    String command(String cmd, long position) {
        MediaController controller = current;
        if (controller == null)
            return "no session";
        MediaController.TransportControls controls = controller.getTransportControls();
        switch (cmd) {
        case "play": controls.play(); break;
        case "pause": controls.pause(); break;
        case "toggle":
            if (playing(controller))
                controls.pause();
            else
                controls.play();
            break;
        case "next": controls.skipToNext(); break;
        case "previous": controls.skipToPrevious(); break;
        case "seek":
            if (position >= 0)
                controls.seekTo(position);
            break;
        default:
            return "unknown command";
        }
        return "sent";
    }
}
