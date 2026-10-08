package org.omarchy.mobile.keyboard;

import android.app.Activity;

/**
 * A blank screen in front of Android just before it sleeps. Android's
 * screen-off lays a black layer (ColorFade) over everything, and Waydroid's
 * single-window mode draws every layer into the window of the app in front
 * of Android: that app's window turned black, in the switcher and when it
 * came back. With this in front first, Waydroid freezes the app's window at
 * its last frame (as for any app no longer in front) and the black lands in
 * this one's window, which hypr-mobile.lua keeps out of sight. It closes as
 * soon as anything covers it or the screen goes off, so it never stays in
 * Android's task list.
 *
 * Started by waydroid-container.sh's watcher as root:
 *   am start -W -n org.omarchy.mobile.keyboard/.SleepActivity
 */
public class SleepActivity extends Activity {
    @Override
    protected void onStop() {
        super.onStop();
        finish();
    }
}
