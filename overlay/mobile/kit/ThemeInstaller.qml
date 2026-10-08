import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Io

Item {
    id: installer
    property var themes: []
    property string query: ""
    property string message: ""
    property string logText: ""
    property bool searching: false
    property bool urlEditing: false
    property var pending: null
    property bool confirming: false
    property bool busy: false
    signal installed()
    signal keyboardRequested(bool wanted)
    readonly property var shown: {
        const needle = installer.query.trim().toLowerCase();
        const items = [];
        for (let i = 0; i < installer.themes.length; i++) {
            const item = installer.themes[i];
            if (!needle || item.name.toLowerCase().indexOf(needle) >= 0 || item.id.indexOf(needle) >= 0)
                items.push(item);
        }
        return items;
    }

    function hideKeyboard() {
        searching = false;
        urlEditing = false;
        keyboardRequested(false);
    }
    function refresh() {
        if (catalog.running) return;
        installer.message = "";
        catalog.running = true;
    }
    function reset() {
        pending = null;
        confirming = false;
        busy = false;
        message = "";
        logText = "";
        hideKeyboard();
    }
    function openRepo() {
        if (!installer.pending) return;
        browser.command = [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-browser", installer.pending.repo];
        browser.running = true;
    }
    function dismissConfirm() {
        Qt.callLater(() => {
            installer.confirming = false;
            installer.pending = null;
        });
    }
    function note(line) {
        const text = String(line).replace(/\u001b\[[0-9;]*m/g, "").replace(/\r/g, "").trim();
        if (!text || text.charAt(0) === "{") return;
        installer.logText = ((installer.logText ? installer.logText + "\n" : "") + text).split("\n").slice(-10).join("\n");
        installer.message = text;
    }
    function installRepo(repo) {
        if (installer.busy || !repo) return;
        const target = repo;
        installer.busy = true;
        installer.logText = "Installing…";
        installer.message = "Installing…";
        hideKeyboard();
        install.command = [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-theme-install", "url", target];
        install.running = true;
        dismissConfirm();
    }
    Component.onCompleted: refresh()

    Process {
        id: catalog
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-theme-install", "catalog"]
        stdout: StdioCollector {}
        stderr: StdioCollector {}
        onExited: (code, status) => {
            if (code !== 0) {
                installer.message = stderr.text.trim() || "Could not load the theme catalog";
                return;
            }
            try { installer.themes = JSON.parse(stdout.text).themes || []; }
            catch (e) { installer.message = "Could not read the theme catalog"; }
        }
    }
    Process {
        id: install
        stdout: SplitParser { onRead: line => installer.note(line) }
        stderr: SplitParser { onRead: line => installer.note(line) }
        onExited: (exitCode, exitStatus) => {
            installer.busy = false;
            installer.confirming = false;
            installer.pending = null;
            if (exitCode === 0) {
                installer.message = "";
                installer.installed();
                return;
            }
            if (!installer.message || installer.message === "Installing…")
                installer.note("Install failed");
        }
    }
    Process { id: browser }
    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        TouchTextField {
        Layout.fillWidth: true
        placeholderText: "Search themes"
        color: MobileTheme.foreground
        placeholderTextColor: MobileTheme.secondary
        font.family: MobileTheme.fontFamily
        font.pixelSize: 16
        editing: installer.searching
        onEditingRequested: { installer.searching = true; installer.urlEditing = false; forceActiveFocus(); installer.keyboardRequested(true); }
        onTextChanged: installer.query = text
        background: Rectangle { radius: MobileTheme.radius(14); color: MobileTheme.surface; border.color: MobileTheme.muted }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        TouchTextField {
            id: urlField
            Layout.fillWidth: true
            placeholderText: "GitHub link"
            color: MobileTheme.foreground
            placeholderTextColor: MobileTheme.secondary
            font.family: MobileTheme.fontFamily
            font.pixelSize: 15
            editing: installer.urlEditing
            onEditingRequested: { installer.urlEditing = true; installer.searching = false; forceActiveFocus(); installer.keyboardRequested(true); }
            background: Rectangle { radius: MobileTheme.radius(14); color: MobileTheme.surface; border.color: MobileTheme.muted }
        }
        TouchButton {
            label: "Install"
            implicitWidth: 96
            implicitHeight: 48
            enabled: !installer.busy && urlField.text.trim().length > 0
            onClicked: installer.installRepo(urlField.text.trim())
        }
    }
    Flickable {
        visible: installer.logText !== ""
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(logBody.implicitHeight + 8, 168)
        clip: true
        contentHeight: logBody.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        Text {
            id: logBody
            width: parent.width
            text: installer.logText
            wrapMode: Text.Wrap
            color: installer.busy ? MobileTheme.foreground : MobileTheme.accent
            font.family: "monospace"
            font.pixelSize: 12
        }
    }
    GridView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        cellWidth: width / 2
        cellHeight: cellWidth * 0.72
        model: installer.shown
        delegate: Item {
            required property var modelData
            width: GridView.view.cellWidth
            height: GridView.view.cellHeight
            Rectangle {
                anchors.fill: parent
                anchors.margins: 6
                radius: MobileTheme.radius(14)
                color: MobileTheme.surface
                clip: true
                Image {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: parent.height - 36
                    source: modelData.preview || ""
                    sourceSize: Qt.size(360, 200)
                    asynchronous: true
                    fillMode: Image.PreserveAspectCrop
                    cache: true
                }
                Text {
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: 8
                    text: modelData.name + (modelData.installed ? "  ·  Installed" : "")
                    color: MobileTheme.foreground
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                TapHandler {
                    enabled: !installer.confirming
                    onTapped: {
                        installer.pending = {
                            name: modelData.name,
                            id: modelData.id,
                            repo: modelData.repo,
                            preview: modelData.preview
                        };
                        installer.confirming = true;
                        installer.hideKeyboard();
                    }
                }
            }
        }
    }
    }
    Rectangle {
        anchors.fill: parent
        visible: installer.confirming
        color: Qt.rgba(0, 0, 0, 0.72)
        z: 4
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 28, 420)
            height: cardColumn.implicitHeight + 36
            radius: MobileTheme.radius(22)
            color: MobileTheme.surface
            border.color: MobileTheme.muted
            TapHandler { onTapped: {} }
            Column {
                id: cardColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 18
                spacing: 14
                Image {
                    width: parent.width
                    height: 150
                    source: installer.pending ? installer.pending.preview : ""
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                }
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: installer.pending ? installer.pending.name : ""
                    color: MobileTheme.foreground
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 22
                    font.bold: true
                }
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: "Install this theme?"
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 15
                }
                TouchButton {
                    width: parent.width
                    label: "Install"
                    implicitHeight: 52
                    enabled: !installer.busy
                    onClicked: installer.installRepo(installer.pending.repo)
                }
                TouchButton {
                    width: parent.width
                    label: "View on GitHub"
                    implicitHeight: 52
                    onClicked: installer.openRepo()
                }
                TouchButton {
                    width: parent.width
                    label: "Cancel"
                    implicitHeight: 48
                    onClicked: installer.dismissConfirm()
                }
            }
        }
        TapHandler { onTapped: installer.dismissConfirm() }
    }
}
