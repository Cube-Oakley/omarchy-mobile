package org.omarchy.mobile.keyboard;

import android.inputmethodservice.InputMethodService;
import android.provider.Settings;
import android.util.Log;
import android.view.View;

/**
 * Android's input method on the phone, standing in for the phone's own
 * keyboard. When a text field takes focus Android shows this keyboard: an
 * empty view as tall as the phone's keyboard, so the app lays itself out
 * above the keys. It logs "shown" and "hidden" (tag OmarchyKeyboard), which
 * waydroid-container.sh's watcher passes to the shell, and the shell shows
 * the phone's keyboard over the bottom of the app while it is shown. (With
 * Waydroid's per-app windows it had a window of its own, waydroid.InputMethod;
 * in single-window mode it is drawn into the app's window.) Keys reach the
 * app as hardware keys through Waydroid's Wayland keyboard, which this method
 * passes through untouched.
 *
 * The height, in dp (the shell's logical pixels at Android's 480 dpi), is the
 * global setting omarchy_keyboard_height, which waydroid-container.sh sets
 * from the phone's keyboard settings at each start.
 */
public class BridgeIme extends InputMethodService {
    private static final int DEFAULT_HEIGHT_DP = 348; // strip 44 + keys 280 + navigation 24
    private static final String TAG = "OmarchyKeyboard";
    // For HideReceiver: the running input method, if any.
    static volatile BridgeIme instance;

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
    }

    @Override
    public View onCreateInputView() {
        int dp = Settings.Global.getInt(getContentResolver(), "omarchy_keyboard_height", DEFAULT_HEIGHT_DP);
        if (dp < 100 || dp > 700)
            dp = DEFAULT_HEIGHT_DP;
        final int px = Math.round(dp * getResources().getDisplayMetrics().density);
        // The height is the view's own measure. setInputView gives the view
        // MATCH_PARENT x WRAP_CONTENT layout parameters in place of any it
        // had, and a plain View measured WRAP_CONTENT takes all the height
        // offered: the input method then covered the whole display, and
        // apps shrank to nothing or panned off the top (October 6, WhatsApp's
        // phone number screen).
        return new View(this) {
            @Override
            protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
                setMeasuredDimension(MeasureSpec.getSize(widthMeasureSpec), px);
            }
        };
    }

    /** Shown with a hardware keyboard too: Waydroid's keyboard is one. */
    @Override
    public boolean onEvaluateInputViewShown() {
        super.onEvaluateInputViewShown();
        return true;
    }

    @Override
    public boolean onEvaluateFullscreenMode() {
        return false;
    }

    @Override
    public void onWindowShown() {
        super.onWindowShown();
        Log.i(TAG, "shown");
    }

    @Override
    public void onWindowHidden() {
        super.onWindowHidden();
        Log.i(TAG, "hidden");
    }

    @Override
    public void onDestroy() {
        Log.i(TAG, "hidden");
        if (instance == this)
            instance = null;
        super.onDestroy();
    }
}
