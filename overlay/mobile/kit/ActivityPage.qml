import QtQuick
import Quickshell
import Quickshell.Io

// Settings > Activity: step counting on or off, today's steps and the week.
// The counting itself runs in the sensor hub (omarchy-mobile-steps).
Flickable {
    id: page
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    contentHeight: body.height
    property var status: ({})
    property var days: []
    readonly property string helper: Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-steps"
    function refresh() {
        if (!probe.running) probe.running = true;
        if (!week.running) week.running = true;
    }
    Component.onCompleted: refresh()
    onVisibleChanged: if (visible) refresh()
    Process {
        id: probe
        command: [page.helper, "status"]
        stdout: StdioCollector {}
        onExited: { try { page.status = JSON.parse(stdout.text); } catch (e) { page.status = {}; } }
    }
    Process {
        id: week
        command: [page.helper, "days", "7"]
        stdout: StdioCollector {}
        onExited: { try { page.days = (JSON.parse(stdout.text).days || []).slice().reverse(); } catch (e) { page.days = []; } }
    }
    Process {
        id: toggle
        stdout: StdioCollector {}
        onExited: page.refresh()
    }
    // Refresh while open: steps arrive a minute at a time.
    Timer { interval: 30000; repeat: true; running: page.visible; onTriggered: page.refresh() }
    Column {
        id: body
        width: page.width
        spacing: 12
        SettingsRow {
            width: parent.width
            label: "Count steps"
            value: page.status.available === false ? "No step counter" : page.status.enabled ? "On" : "Off"
            selected: !!page.status.enabled
            onClicked: {
                if (toggle.running || page.status.available === false) return;
                toggle.command = [page.helper, page.status.enabled ? "disable" : "enable"];
                toggle.running = true;
            }
        }
        Text {
            visible: !!page.status.enabled || page.days.some(d => d.steps > 0)
            width: parent.width
            topPadding: 8
            text: (page.status.today || 0).toLocaleString(Qt.locale(), "f", 0) + " steps today"
            color: MobileTheme.foreground
            font.family: MobileTheme.fontFamily
            font.pixelSize: 22
            font.bold: true
        }
        Repeater {
            model: page.days.slice(1)
            delegate: DetailRow {
                required property var modelData
                width: body.width
                label: new Date(modelData.date + "T12:00:00").toLocaleDateString(Qt.locale(), "dddd d MMM")
                value: modelData.steps.toLocaleString(Qt.locale(), "f", 0)
            }
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            topPadding: 8
            text: "The sensor hub counts steps by itself, without waking the phone. Steps are kept a minute at a time in /var/lib/omarchy-mobile/steps/steps.db, which apps and syncs can read (omarchy-mobile-steps export)."
            color: MobileTheme.secondary
            font.family: MobileTheme.fontFamily
            font.pixelSize: 14
        }
    }
}
