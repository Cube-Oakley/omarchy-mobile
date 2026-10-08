import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import Quickshell.Wayland

// The share sheet: what an app shares (text, a link, files), and where it
// can go. Apps ask through omarchy-mobile-share, which keeps the request and
// names it on the shell's request FIFO ("share ID"); the helper also lists
// the targets for it and carries the share out.
PanelWindow {
    id: sheet
    property var request: null
    property string message: ""
    property bool busy: false
    readonly property string helper: Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-share"
    readonly property bool opened: request !== null

    function show(ident) {
        if (!/^[0-9a-f]{16}$/.test(ident)) return;
        message = "";
        loader.command = [helper, "show", ident];
        loader.running = true;
    }
    function close() { if (!busy) { request = null; message = ""; } }
    function choose(target) {
        if (busy || !request) return;
        busy = true;
        message = "";
        runner.command = [helper, "run", request.id, target];
        runner.running = true;
    }

    visible: opened || closing.running
    anchors { top: true; bottom: true; left: true; right: true }
    exclusionMode: ExclusionMode.Ignore
    exclusiveZone: 0
    color: "transparent"
    WlrLayershell.namespace: "omarchy-mobile-share"
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.keyboardFocus: WlrKeyboardFocus.None

    Process {
        id: loader
        stdout: StdioCollector {}
        onExited: {
            try {
                const result = JSON.parse(loader.stdout.text);
                if (result.ok) sheet.request = result;
            } catch (e) {}
        }
    }
    Process {
        id: runner
        stdout: StdioCollector {}
        onExited: {
            sheet.busy = false;
            let result = {};
            try { result = JSON.parse(runner.stdout.text); } catch (e) {}
            if (result.ok) {
                sheet.message = result.message || "";
                if (sheet.message) closing.restart(); else sheet.close();
            } else {
                sheet.message = result.error || "That did not work";
            }
        }
    }
    // A done message shows for a moment before the sheet goes.
    Timer { id: closing; interval: 900; onTriggered: sheet.close() }

    Rectangle {
        anchors.fill: parent
        color: "#99000000"
        opacity: sheet.opened ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 160 } }
        // Above the panel only: the panel's taps reach this handler too.
        TapHandler { onTapped: point => { if (point.position.y < panel.y) sheet.close(); } }
    }

    Rectangle {
        id: panel
        anchors { left: parent.left; right: parent.right }
        y: sheet.opened ? parent.height - height : parent.height
        Behavior on y { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
        height: content.implicitHeight + 48
        radius: MobileTheme.radius(24)
        color: MobileTheme.background
        border.width: 1
        border.color: MobileTheme.muted

        ColumnLayout {
            id: content
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 22 }
            spacing: 14
            Rectangle { Layout.alignment: Qt.AlignHCenter; width: 48; height: 4; radius: 2; color: MobileTheme.muted }
            Text {
                Layout.fillWidth: true
                text: "Share"
                color: MobileTheme.foreground
                font.family: MobileTheme.fontFamily
                font.pixelSize: 22
                font.bold: true
            }
            // What is shared.
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                Rectangle {
                    readonly property var picture: {
                        const files = sheet.request && sheet.request.files ? sheet.request.files : [];
                        return files.length && files[0].mime.startsWith("image/") ? files[0].path : "";
                    }
                    Layout.preferredWidth: 56
                    Layout.preferredHeight: 56
                    radius: MobileTheme.radius(12)
                    color: MobileTheme.surface
                    clip: true
                    Image {
                        anchors.fill: parent
                        visible: parent.picture !== ""
                        source: parent.picture ? "file://" + parent.picture : ""
                        sourceSize: Qt.size(168, 168)
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                    }
                    Text {
                        anchors.centerIn: parent
                        visible: parent.picture === ""
                        text: !sheet.request ? "" : sheet.request.kind === "url" ? "\u{F0337}"
                            : sheet.request.kind === "files" ? "\u{F0214}" : "\u{F09ED}"
                        color: MobileTheme.accent
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 26
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        Layout.fillWidth: true
                        text: {
                            const r = sheet.request;
                            if (!r) return "";
                            if (r.title) return r.title;
                            if (r.kind === "files") return r.files.length === 1 ? r.files[0].name : r.files.length + " files";
                            return r.kind === "url" ? r.url : r.text.split("\n")[0];
                        }
                        textFormat: Text.PlainText
                        color: MobileTheme.foreground
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 15
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: {
                            const r = sheet.request;
                            if (!r) return "";
                            if (r.kind === "url") return r.title ? r.url : "";
                            if (r.kind === "files") return r.files.map(f => f.name).join(", ");
                            return r.title ? r.text : "";
                        }
                        textFormat: Text.PlainText
                        color: MobileTheme.secondary
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 13
                        elide: Text.ElideRight
                        maximumLineCount: 2
                        wrapMode: Text.Wrap
                    }
                }
            }
            // Where it can go.
            Grid {
                Layout.fillWidth: true
                columns: 4
                rowSpacing: 14
                columnSpacing: 0
                Repeater {
                    model: sheet.request ? sheet.request.targets : []
                    Item {
                        required property var modelData
                        width: content.width / 4
                        height: 86
                        Rectangle {
                            id: face
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: 56
                            height: 56
                            radius: MobileTheme.radius(18)
                            color: tap.pressed ? MobileTheme.selection : MobileTheme.surface
                            border.width: 1
                            border.color: MobileTheme.muted
                            Text {
                                anchors.centerIn: parent
                                text: modelData.icon
                                color: MobileTheme.accent
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 26
                            }
                        }
                        Text {
                            anchors { top: face.bottom; topMargin: 6; left: parent.left; right: parent.right }
                            horizontalAlignment: Text.AlignHCenter
                            text: modelData.name
                            textFormat: Text.PlainText
                            color: MobileTheme.foreground
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                        TapHandler { id: tap; enabled: !sheet.busy; onTapped: sheet.choose(modelData.id) }
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                visible: sheet.message !== "" || sheet.busy
                text: sheet.busy ? "Sharing…" : sheet.message
                horizontalAlignment: Text.AlignHCenter
                color: MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 14
                wrapMode: Text.WordWrap
            }
        }
    }
}
