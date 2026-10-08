package org.omarchy.mobile.keyboard;

import android.content.Context;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.SystemClock;

/**
 * A media session of the bridge's own, for testing the media card's path
 * without an app or sound: "media test" through the watcher's FIFO starts it
 * playing a 3-minute track, "media test-end" ends it. It answers play,
 * pause, skips and seeks as a player would.
 */
class TestSession extends MediaSession.Callback {
    private static MediaSession session;
    private static int track = 1;
    private static final long LENGTH = 180_000;
    private long position;
    private boolean playing;
    private long since;

    static void start(Context context) {
        if (session != null)
            return;
        session = new MediaSession(context, "OmarchyTest");
        TestSession callback = new TestSession();
        session.setCallback(callback);
        session.setActive(true);
        callback.playing = true;
        callback.since = SystemClock.elapsedRealtime();
        callback.publish();
    }

    static void end() {
        if (session == null)
            return;
        session.setActive(false);
        session.release();
        session = null;
    }

    private long now() {
        return playing ? position + SystemClock.elapsedRealtime() - since : position;
    }

    private void publish() {
        session.setMetadata(new MediaMetadata.Builder()
                .putString(MediaMetadata.METADATA_KEY_TITLE, "Test track " + track)
                .putString(MediaMetadata.METADATA_KEY_ARTIST, "Omarchy bridge")
                .putString(MediaMetadata.METADATA_KEY_ALBUM, "Self-test")
                .putLong(MediaMetadata.METADATA_KEY_DURATION, LENGTH)
                .build());
        session.setPlaybackState(new PlaybackState.Builder()
                .setActions(PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE
                        | PlaybackState.ACTION_PLAY_PAUSE | PlaybackState.ACTION_SKIP_TO_NEXT
                        | PlaybackState.ACTION_SKIP_TO_PREVIOUS | PlaybackState.ACTION_SEEK_TO)
                .setState(playing ? PlaybackState.STATE_PLAYING : PlaybackState.STATE_PAUSED,
                        now(), playing ? 1 : 0, SystemClock.elapsedRealtime())
                .build());
    }

    private void change(boolean play, long at) {
        position = Math.max(0, Math.min(LENGTH, at));
        playing = play;
        since = SystemClock.elapsedRealtime();
        publish();
    }

    @Override public void onPlay() { change(true, now()); }
    @Override public void onPause() { change(false, now()); }
    @Override public void onSeekTo(long to) { change(playing, to); }
    @Override public void onSkipToNext() { track++; change(playing, 0); }
    @Override public void onSkipToPrevious() { track = Math.max(1, track - 1); change(playing, 0); }
}
