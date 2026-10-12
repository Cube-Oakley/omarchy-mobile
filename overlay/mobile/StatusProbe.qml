import QtQuick
import Quickshell.Io

// Read cached status on system events, retaining polling if a monitor fails.
Item {
    id: probe
    property var sampleCommand: []
    property var eventCommand: []
    property int pollInterval: 30000
    // An inactive probe neither samples nor polls; it samples on activation.
    property bool active: true
    property var state: ({ available: false })
    property bool pending: false

    function refresh() {
        if (!active) return;
        if (sample.running) pending = true;
        else sample.running = true;
    }
    Process {
        id: sample
        command: probe.sampleCommand
        stdout: StdioCollector {}
        onExited: (code, status) => {
            try { probe.state = code === 0 ? JSON.parse(stdout.text) : { available: false }; }
            catch (e) { probe.state = { available: false }; }
            if (probe.pending) {
                probe.pending = false;
                debounce.restart();
            }
        }
    }
    Process {
        id: events
        // Ends with the shell (setpriv's parent-death signal; see shell.qml).
        command: ["setpriv", "--pdeathsig", "TERM"].concat(probe.eventCommand)
        running: probe.eventCommand.length > 0
        stdout: SplitParser {
            onRead: data => { if (data.trim().length) debounce.restart(); }
        }
        onExited: if (probe.eventCommand.length > 0) retry.start()
    }
    Timer { id: debounce; interval: 150; onTriggered: probe.refresh() }
    Timer { id: retry; interval: 10000; onTriggered: events.running = true }
    Timer {
        interval: probe.pollInterval; running: probe.active; repeat: true
        onTriggered: probe.refresh()
    }
    onActiveChanged: refresh()
    Component.onCompleted: refresh()
}
