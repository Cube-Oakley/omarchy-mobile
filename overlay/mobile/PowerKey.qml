import QtQuick
import Quickshell
import Quickshell.Hyprland
import Quickshell.Io

// The power and volume keys, from Hyprland as global shortcuts
// (hypr-mobile.lua): press and release arrive here at once, with no process
// to start for either.
// - A dark screen or the always-on display wakes on the press, as on Android.
// - On a lit screen a tap turns it off, or to the always-on display, on the
//   release; held two seconds the key opens the power menu instead.
// - A ringing call is silenced instead.
// - A press while the screen is still changing is dropped with its release:
//   acting once the change finished would undo it.
// - Power and Volume Down together, in either order, take a screenshot: as
//   on Android, Volume Down waits 150 ms for Power before it steps, and
//   neither key then does its own thing.
// - A volume key held repeats.
// The change itself is omarchy-mobile-display's.
Scope {
    id: key
    property bool lit: true
    property bool ambient: false
    property bool alwaysOn: false
    property bool ringing: false
    signal silence()
    signal powerMenu()
    signal volume(string action)
    signal screenshot()

    readonly property string display: Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-display"
    property bool down: false
    // This press has acted already (woke the screen, silenced a call) or is dropped.
    property bool consumed: false
    property bool held: false
    // A change takes under a second and a half; one that runs longer (a
    // request the shell missed, waiting out its timeout) no longer drops
    // presses.
    property double started: 0
    function busy() { return change.running && Date.now() - started < 2000; }

    function run(action) {
        started = Date.now();
        // Past a stalled change, which still holds the tracked process.
        if (change.running) {
            Quickshell.execDetached([display, action]);
            return;
        }
        change.command = [display, action];
        change.running = true;
    }
    Process { id: change }

    GlobalShortcut {
        appid: "omarchy-mobile"
        name: "power"
        description: "Phone power button"
        onPressed: key.press()
        onReleased: key.release()
    }

    function press() {
        down = true;
        held = false;
        consumed = false;
        if (volumeDown && lit && !ambient) {
            chord();
            return;
        }
        if (busy()) {
            consumed = true;
            return;
        }
        if (ringing) {
            silence();
            consumed = true;
            return;
        }
        if (!lit || ambient) {
            run("on");
            consumed = true;
        }
        hold.restart();
    }
    function release() {
        down = false;
        hold.stop();
        if (consumed || held) return;
        run(alwaysOn ? "ambient" : "off");
    }
    Timer {
        id: hold
        interval: 2000
        onTriggered: if (key.down) { key.held = true; key.powerMenu(); }
    }

    // ---- volume keys
    property bool volumeDown: false
    // This Volume Down press made a screenshot: it steps no volume.
    property bool chorded: false
    property string repeating: ""
    function chord() {
        consumed = true;
        held = true;
        hold.stop();
        chorded = true;
        volumeWait.stop();
        volumeRepeat.stop();
        screenshot();
    }
    function volumePress(action) {
        if (action === "down") {
            volumeDown = true;
            chorded = false;
            if (down && !consumed && lit && !ambient) {
                chord();
                return;
            }
            volumeWait.restart();
            return;
        }
        step(action);
    }
    function volumeRelease(action) {
        if (action === "down") {
            volumeDown = false;
            // A quick tap: it steps now, without waiting out the chord.
            if (volumeWait.running) {
                volumeWait.stop();
                if (!chorded) volume("down");
            }
        }
        if (repeating === action) {
            volumeRepeat.stop();
            repeating = "";
        }
    }
    function step(action) {
        volume(action);
        repeating = action;
        volumeRepeat.interval = 400;
        volumeRepeat.restart();
    }
    Timer {
        id: volumeWait
        interval: 150
        onTriggered: if (key.volumeDown && !key.chorded) key.step("down")
    }
    Timer {
        id: volumeRepeat
        interval: 400
        repeat: true
        onTriggered: {
            interval = 100;
            if (key.repeating !== "") key.volume(key.repeating);
        }
    }
    GlobalShortcut {
        appid: "omarchy-mobile"
        name: "volume-up"
        description: "Phone volume up"
        onPressed: key.volumePress("up")
        onReleased: key.volumeRelease("up")
    }
    GlobalShortcut {
        appid: "omarchy-mobile"
        name: "volume-down"
        description: "Phone volume down"
        onPressed: key.volumePress("down")
        onReleased: key.volumeRelease("down")
    }
    GlobalShortcut {
        appid: "omarchy-mobile"
        name: "volume-mute"
        description: "Phone mute"
        onPressed: key.volume("mute")
    }
}
