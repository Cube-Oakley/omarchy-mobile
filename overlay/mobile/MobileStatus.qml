pragma Singleton
import QtQuick
import Quickshell
import Quickshell.Io
Singleton {
    id: status
    property alias battery: batteryProbe.state
    property alias wifi: wifiProbe.state
    property alias bluetooth: bluetoothProbe.state
    property alias volume: volumeProbe.state
    property alias stats: statsProbe.state
    // Brightness, flashlight, vibration and the alert slider (omarchy-mobile-controls).
    property alias controls: controlsProbe.state
    property bool dnd: false
    // The always-on display is showing (omarchy-mobile-display ambient).
    property bool ambient: false
    // Its brightness, on the slider's scale: dim, but readable indoors.
    readonly property int ambientLevel: 15
    function setAmbient(on) {
        if (on === ambient) return;
        ambient = on;
        // The level is not remembered; leaving restores the user's own.
        if (on) brightness(ambientLevel, false);
        else control(["restore"]);
    }
    // Set while the shade's performance page is open.
    property bool statsShown: false
    function refresh() {
        batteryProbe.refresh(); wifiProbe.refresh(); bluetoothProbe.refresh();
        volumeProbe.refresh(); statsProbe.refresh();
    }
    function setDnd(enabled) {
        dnd = enabled;
        prefsInput = JSON.stringify({dnd: enabled}) + "\n";
        prefsSave.running = true;
    }
    property string prefsInput: ""
    // Control actions run one at a time. A newer action of the same kind
    // replaces one still waiting, so a brightness drag sends only its latest.
    property var controlQueue: []
    function control(args) {
        controlQueue = controlQueue.filter(waiting => waiting[0] !== args[0]).concat([args]);
        if (!controlRunner.running) controlNext();
    }
    function controlNext() {
        if (!controlQueue.length) return;
        controlRunner.command = [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-controls"].concat(controlQueue[0]);
        controlQueue = controlQueue.slice(1);
        controlRunner.running = true;
    }
    Process {
        id: controlRunner
        stdout: StdioCollector {}
        onExited: {
            try {
                const result = JSON.parse(stdout.text);
                if (result.torch !== undefined) controlsProbe.state = result;
            } catch (e) {}
            status.controlNext();
        }
    }
    // Brightness during a drag goes to one resident writer: starting a
    // process per step made the screen lag the slider. The final level of a
    // drag is remembered.
    function brightness(level, final) {
        if (!brightnessWriter.running) brightnessWriter.running = true;
        brightnessWriter.write((final ? "remember " : "brightness ") + Math.round(level) + "\n");
        if (final) brightnessRefresh.restart();
    }
    Process {
        id: brightnessWriter
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-controls", "serve"]
        stdinEnabled: true
        running: true
    }
    Timer { id: brightnessRefresh; interval: 100; onTriggered: controlsProbe.refresh() }
    // Automatic brightness (the shade's sun icon): a resident follower of the
    // light sensor while it is on. Each change it makes moves the slider
    // through a status refresh; the slider's final level teaches it.
    readonly property bool autoBrightness: controlsProbe.state.auto === true
    // Paused while the always-on display holds its own dim level.
    readonly property bool followLight: autoBrightness && !ambient
    onFollowLightChanged: autoFollower.running = followLight
    Process {
        id: autoFollower
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-controls", "auto-run"]
        stdout: SplitParser {
            onRead: data => { if (data.trim().length) brightnessRefresh.restart(); }
        }
        onExited: if (status.followLight) autoRetry.start()
    }
    Timer { id: autoRetry; interval: 10000; onTriggered: autoFollower.running = status.followLight }
    StatusProbe {
        id: controlsProbe
        sampleCommand: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-controls", "status"]
        // A line per alert slider change; nothing else changes on its own.
        eventCommand: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-controls", "watch"]
        pollInterval: 60000
    }
    // The screen comes up at the panel's default; bring back the user's level.
    Component.onCompleted: control(["restore"])
    StatusProbe {
        id: batteryProbe
        sampleCommand: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-battery"]
        eventCommand: ["env", "SYSTEMD_IN_CHROOT=0", "stdbuf", "-oL", "udevadm", "monitor", "--kernel", "--subsystem-match=power_supply"]
    }
    StatusProbe {
        id: wifiProbe
        sampleCommand: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-wifi"]
        eventCommand: ["stdbuf", "-oL", "nmcli", "monitor"]
        pollInterval: 30000
    }
    StatusProbe {
        id: bluetoothProbe
        sampleCommand: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-bluetooth"]
        // BlueZ signals power, connection and pairing changes on the system bus.
        eventCommand: ["dbus-monitor", "--system", "type='signal',sender='org.bluez'"]
        pollInterval: 30000
    }
    StatusProbe {
        id: volumeProbe
        sampleCommand: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-volume"]
        pollInterval: 10000
    }
    StatusProbe {
        id: statsProbe
        sampleCommand: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-stats"]
        pollInterval: 3000
        // The full snapshot costs 0.1 CPU-s of Python, and every new result
        // stalled the shell's animations for 60 ms, so it only runs while
        // its page is shown.
        active: status.statsShown
    }
    Process {
        id: prefsLoad
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-prefs"]
        running: true
        stdout: StdioCollector {}
        onExited: {
            try { status.dnd = JSON.parse(stdout.text).dnd === true; }
            catch (e) { status.dnd = false; }
        }
    }
    Process {
        id: prefsSave
        stdinEnabled: true
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-prefs", "set"]
        stdout: StdioCollector {}
        onStarted: { write(status.prefsInput); status.prefsInput = ""; stdinEnabled = false; }
        onExited: {
            stdinEnabled = true;
            try { status.dnd = JSON.parse(stdout.text).dnd === true; }
            catch (e) {}
        }
    }
}
