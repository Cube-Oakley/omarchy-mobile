import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import Quickshell.Wayland

PanelWindow {
    id: menu
    property bool opened: false
    property var available: ({restart: false, bootloader: false})
    property string error: ""
    property string destination: ""
    property bool restarting: false
    readonly property string helper: Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-power-menu"
    function show() {
        if (restarting) return;
        error = "";
        opened = true;
        capabilities.running = true;
    }
    function close() { if (!restarting) opened = false; }
    function restart(target) {
        if (restarting || !available[target]) return;
        error = "";
        destination = target;
        restarting = true;
        reboot.command = [helper, target];
        reboot.running = true;
    }
    visible: opened
    anchors { top: true; bottom: true; left: true; right: true }
    exclusionMode: ExclusionMode.Ignore
    exclusiveZone: 0
    color: "transparent"
    WlrLayershell.namespace: "omarchy-mobile-power-menu"
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.keyboardFocus: WlrKeyboardFocus.OnDemand

    Rectangle {
        anchors.fill: parent
        color: "#b3000000"
        TapHandler { onTapped: menu.close() }
    }
    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 40, 360)
        height: content.implicitHeight + 40
        radius: MobileTheme.radius(24)
        color: MobileTheme.background
        // Keep taps inside the sheet from reaching the dismiss backdrop.
        TapHandler {}
        ColumnLayout {
            id: content
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 20 }
            spacing: 12
            Text {
                Layout.fillWidth: true
                text: menu.restarting ? "Restarting…" : "Power menu"
                color: MobileTheme.foreground
                font.family: MobileTheme.fontFamily
                font.pixelSize: 24; font.bold: true
            }
            Text {
                Layout.fillWidth: true
                visible: menu.restarting || menu.error !== ""
                text: menu.restarting ? (menu.destination === "bootloader"
                    ? "Opening the bootloader. This can take about 30 seconds."
                    : "Restarting your phone. This can take about 30 seconds.") : menu.error
                color: MobileTheme.foreground
                font.family: MobileTheme.fontFamily; font.pixelSize: 15
                wrapMode: Text.WordWrap
            }
            TouchButton {
                Layout.fillWidth: true
                label: "Restart"
                visible: !menu.restarting
                enabled: menu.available.restart
                opacity: enabled ? 1 : 0.4
                onClicked: menu.restart("restart")
            }
            TouchButton {
                Layout.fillWidth: true
                label: "Restart to bootloader"
                visible: !menu.restarting && menu.available.bootloader
                onClicked: menu.restart("bootloader")
            }
            TouchButton {
                Layout.fillWidth: true
                label: "Cancel"
                visible: !menu.restarting
                onClicked: menu.close()
            }
        }
    }
    Process {
        id: capabilities
        command: [menu.helper, "status"]
        stdout: StdioCollector {}
        onExited: (code, status) => {
            try {
                menu.available = code === 0 ? JSON.parse(stdout.text) : ({});
            } catch (e) { menu.available = ({}); }
            if (!menu.available.restart) menu.error = "Restart is not available on this device.";
        }
    }
    Process {
        id: reboot
        stdout: StdioCollector {}
        stderr: StdioCollector {}
        onExited: (code, status) => {
            if (code !== 0) {
                menu.restarting = false;
                menu.error = "Could not restart safely. Please try again.";
                console.warn("MOBILE_REBOOT_FAILED " + stderr.text.trim());
            }
        }
    }
}
