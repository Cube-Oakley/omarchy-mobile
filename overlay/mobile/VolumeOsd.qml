import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import Quickshell.Wayland

PanelWindow {
    id: osd
    // Android-style: the keys change one group (media unless a call or other
    // higher group is playing); the chevron expands to every group's slider.
    readonly property var groups: [
        {id: "media", label: "Media", icon: "󰝚"},
        {id: "ring", label: "Ring", icon: "󰂚"},
        {id: "call", label: "Calls", icon: "󰏲"},
        {id: "alarm", label: "Alarms", icon: "󰀠"}
    ]
    property var state: ({available: false, percent: 0, muted: false, keys: "media", groups: {}})
    property string focusGroup: "media"
    property bool expanded: false
    property var queue: []
    function group(id) {
        const g = state.groups ? state.groups[id] : undefined;
        return g || (id === "media" ? state : {available: false, percent: 0, muted: false});
    }
    function showFor() { shown = true; hide.restart(); }
    function dismiss() { hide.stop(); dragging = false; shown = false; expanded = false; }
    function adjust(action, target) {
        if (["up", "down", "mute", "status"].indexOf(action) < 0) return;
        const id = target || state.keys || "media";
        const level = group(id);
        if ((action === "up" || action === "down") && level.available) {
            // Move the slider now; the backend's answer settles it once the queue drains.
            const groups = Object.assign({}, state.groups || {});
            groups[id] = Object.assign({}, level, {
                percent: Math.max(0, Math.min(100, level.percent + (action === "up" ? 5 : -5)))});
            state = Object.assign({}, state, {groups: groups}, id === "media" ? groups[id] : {});
            focusGroup = id;
        }
        if (action !== "status") showFor();
        queue = queue.concat([target ? [action, target] : [action]]);
        runNext();
    }
    function setVolume(target, percent) {
        // Coalesce slider requests per group while keeping ordered key presses.
        queue = queue.filter(r => !(r[0] === "set" && r[2] === target))
            .concat([["set", String(Math.round(percent)), target]]);
        showFor();
        runNext();
    }
    // Volume changes the phone did not make: the headphones' own buttons change
    // their absolute volume. Reading status also lets volume.py follow outputs.
    property bool checking: false
    function checkExternal() {
        if (request.running || queue.length > 0) return;
        checking = true;
        request.command = [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-volume"];
        request.running = true;
    }
    function runNext() {
        if (request.running || queue.length === 0) return;
        const args = queue[0]; queue = queue.slice(1);
        request.command = [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-volume"].concat(args);
        request.running = true;
    }
    Process {
        id: request
        stdout: StdioCollector {}
        onExited: (code, status) => {
            // Intermediate answers would pull the slider back behind queued presses.
            if (osd.queue.length === 0) {
                const before = osd.state;
                try { osd.state = JSON.parse(stdout.text); }
                catch (e) { osd.state = {available: false, percent: 0, muted: false, keys: "media", groups: {}}; }
                const was = before.output || {}, now = osd.state.output || {};
                const a = (before.groups || {}).media || {}, b = (osd.state.groups || {}).media || {};
                if (osd.checking && now.bluetooth && was.name === now.name
                        && (a.percent !== b.percent || a.muted !== b.muted)) {
                    osd.focusGroup = "media";
                    osd.showFor();
                }
                if (!osd.expanded) osd.focusGroup = osd.state.changed || osd.state.keys || "media";
            }
            osd.checking = false;
            if (osd.shown) hide.restart();
            Qt.callLater(osd.runNext);
        }
    }
    Process {
        id: watch
        command: ["setpriv", "--pdeathsig", "TERM", "pactl", "subscribe"]
        running: true
        stdout: SplitParser {
            // Sink volumes and the default output; stream events are too frequent.
            onRead: data => { if (/on (sink #|server)/.test(data)) settle.restart(); }
        }
        onExited: rewatch.start()
    }
    Timer { id: settle; interval: 200; onTriggered: osd.checkExternal() }
    Timer { id: rewatch; interval: 5000; onTriggered: watch.running = true }
    // Visibility is its own state: restarting a running Timer briefly stops it,
    // which would unmap the panel (a flash) and collapse it on every press.
    Timer {
        id: hide
        interval: osd.expanded ? 5000 : 2500
        onTriggered: if (!osd.dragging) { osd.shown = false; osd.expanded = false; }
    }
    property bool shown: false
    property bool dragging: false
    // While shown, the panel covers the whole screen with a transparent surface:
    // a layer surface never hears touches beyond its own bounds, and a tap
    // anywhere outside the panel should close it.
    visible: shown
    anchors { top: true; bottom: true; left: true; right: true }
    exclusionMode: ExclusionMode.Ignore
    color: "transparent"
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.namespace: "omarchy-mobile-volume"
    WlrLayershell.keyboardFocus: WlrKeyboardFocus.None

    MouseArea {
        anchors.fill: parent
        // Presses on the panel's own empty areas reach here too; keep those.
        onPressed: mouse => {
            if (panel.contains(mapToItem(panel, mouse.x, mouse.y))) mouse.accepted = false;
            else osd.dismiss();
        }
    }
    Rectangle {
        id: panel
        // 36 px status bar plus the panel's 70 px offset below it.
        anchors { top: parent.top; right: parent.right; topMargin: 106; rightMargin: 12 }
        width: osd.expanded ? osd.groups.length * 76 + 24 : 84
        height: 318
        radius: MobileTheme.radius(24)
        color: MobileTheme.surface
        border.color: MobileTheme.muted
        ColumnLayout {
            anchors.fill: parent; anchors.margins: 12; spacing: 6
            Text {
                // The output the groups play into: the speakers or Bluetooth headphones.
                readonly property var output: osd.state.output || {}
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: (output.bluetooth ? "󰋋" : "󰓃")
                    + (osd.expanded && output.description ? "  " + output.description : "")
                textFormat: Text.PlainText
                elide: Text.ElideRight
                font.family: MobileTheme.fontFamily; font.pixelSize: 14
                color: output.bluetooth ? MobileTheme.accent : MobileTheme.secondary
            }
            RowLayout {
                Layout.fillWidth: true; Layout.fillHeight: true
                spacing: 16
                Repeater {
                    model: osd.expanded ? osd.groups : osd.groups.filter(g => g.id === osd.focusGroup)
                    delegate: VolumeGroupSlider {
                        level: osd.group(modelData.id)
                        onVolumeMoved: (group, percent) => osd.setVolume(group, percent)
                        onMuteTapped: group => osd.adjust("mute", group)
                        onDragChanged: dragging => { osd.dragging = dragging; if (!dragging) osd.showFor(); }
                    }
                }
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: osd.expanded ? "󰅂" : "󰅁"
                font.family: MobileTheme.fontFamily; font.pixelSize: 22
                color: MobileTheme.secondary
                TapHandler {
                    // Generous target: the glyph alone is too small for a thumb.
                    margin: 14
                    onTapped: { osd.expanded = !osd.expanded; osd.showFor(); }
                }
            }
        }
    }
}
