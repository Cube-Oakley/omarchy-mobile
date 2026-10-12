import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Wayland

// Rotation lock with a suggestion, as on Android: the screen keeps its
// orientation, and when the phone is held another way this button offers
// to follow it for a few seconds (omarchy-mobile-rotation).
PanelWindow {
    id: rotate
    readonly property string helper: Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-rotation"
    // How the phone is held, and how the screen is shown.
    property string held: "undefined"
    property string shown: "normal"
    property bool offered: false
    readonly property bool differs: held !== "undefined" && held !== shown

    function apply() {
        offered = false;
        setter.command = [helper, "set", held];
        setter.running = true;
    }

    anchors { bottom: true; right: true }
    margins { bottom: 28; right: 20 }
    implicitWidth: 64; implicitHeight: 64
    exclusionMode: ExclusionMode.Ignore
    color: "transparent"
    visible: offered && differs && !MobileStatus.ambient
    WlrLayershell.namespace: "omarchy-mobile-rotate"
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.keyboardFocus: WlrKeyboardFocus.None

    onDiffersChanged: {
        offered = differs;
        if (differs) hide.restart();
    }
    onHeldChanged: if (differs) { offered = true; hide.restart(); }
    Timer { id: hide; interval: 5000; onTriggered: rotate.offered = false }

    Rectangle {
        anchors.fill: parent
        radius: MobileTheme.radius(32)
        color: tap.pressed ? MobileTheme.muted : MobileTheme.surface
        border.width: 1; border.color: MobileTheme.accent
        Text {
            anchors.centerIn: parent
            text: "󰢅"
            color: MobileTheme.accent
            font.family: MobileTheme.fontFamily; font.pixelSize: 28
        }
        TapHandler { id: tap; onTapped: rotate.apply() }
    }

    Process {
        id: setter
        stdout: StdioCollector {}
        onExited: {
            try {
                const result = JSON.parse(stdout.text);
                if (result.rotation) rotate.shown = result.rotation;
            } catch (e) {}
        }
    }
    Process {
        id: watcher
        command: ["setpriv", "--pdeathsig", "TERM", rotate.helper, "watch"]
        running: true
        stdout: SplitParser {
            onRead: data => {
                try {
                    const line = JSON.parse(data);
                    if (line.orientation) rotate.held = line.orientation;
                } catch (e) {}
            }
        }
        onExited: retry.start()
    }
    Timer { id: retry; interval: 10000; onTriggered: watcher.running = true }
    // The screen's orientation at start (the shell may restart rotated).
    Process {
        id: reader
        command: [rotate.helper, "status"]
        running: true
        stdout: StdioCollector {}
        onExited: {
            try {
                const result = JSON.parse(stdout.text);
                if (result.rotation) rotate.shown = result.rotation;
            } catch (e) {}
        }
    }
}
