import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import OmarchyMobile

// Notes: plain text and Markdown files in ~/Notes, through
// omarchy-mobile-notes. The list searches titles and text as you type; a
// note saves itself a second after the last change and on leaving it, and
// its file takes the name of its first line. Delete moves it to
// ~/Notes/.trash.
ShellRoot {
    id: root
    property string view: "list"
    property var notes: []
    property string query: ""
    // The open note: its file (empty until a new note is first saved).
    property string noteId: ""
    property bool editing: false
    property bool dirty: false
    property bool confirmDelete: false
    property string message: ""
    property var queue: []
    readonly property string home: Quickshell.env("HOME")
    readonly property string title: {
        const lines = editor.text.split("\n");
        for (const line of lines) {
            const t = line.replace(/^\s*#+\s*/, "").trim();
            if (t) return t;
        }
        return "";
    }

    function bin(name) { return home + "/.local/bin/" + name; }
    function run(args, input, then) {
        if (tool.running) { queue = queue.concat([[args, input, then]]); return; }
        tool.input = input || "";
        tool.then = then || null;
        tool.command = [bin("omarchy-mobile-notes")].concat(args);
        tool.running = true;
    }
    function next() {
        if (!queue.length || tool.running) return;
        const item = queue[0];
        queue = queue.slice(1);
        run(item[0], item[1], item[2]);
    }
    function refresh() { run(["list"].concat(query ? [query] : []), "", result => { root.notes = result.notes || []; }); }
    function keyboard(action) {
        keyboardTool.command = [bin("omarchy-mobile-keyboard"), action];
        keyboardTool.running = true;
    }
    function open(id) {
        run(["read", id], "", result => {
            noteId = result.id;
            editor.text = result.text;
            dirty = false;
            editor.cursorPosition = 0;
            view = "edit";
        });
    }
    function newNote() {
        noteId = "";
        editor.text = "";
        dirty = false;
        view = "edit";
        startEditing();
    }
    function startEditing() {
        editing = true;
        editor.forceActiveFocus();
        keyboard("show");
    }
    function save() {
        saveTimer.stop();
        if (!dirty) return;
        dirty = false;
        run(["save", noteId || "-"], editor.text, result => { if (result.id) root.noteId = result.id; });
    }
    function remove() {
        if (!confirmDelete) { confirmDelete = true; confirmTimer.restart(); return; }
        confirmDelete = false;
        saveTimer.stop();
        dirty = false;
        const id = noteId;
        leaveEditor();
        if (id) run(["delete", id], "", () => { root.message = "Moved to Notes/.trash"; root.refresh(); });
    }
    function leaveEditor() {
        save();
        editing = false;
        keyboard("hide");
        view = "list";
        refresh();
    }
    function copy() {
        copier.command = ["wl-copy", "--", editor.text];
        copier.running = true;
        message = "Copied";
    }
    function goBack() {
        if (view === "edit") { leaveEditor(); return; }
        if (search.editing) { search.editing = false; keyboard("hide"); return; }
        Qt.quit();
    }
    // Today's time, this year's date, or the full date.
    function when(seconds) {
        const date = new Date(seconds * 1000), today = new Date();
        if (date.toDateString() === today.toDateString())
            return Qt.formatTime(date, "HH:mm");
        return Qt.formatDate(date, date.getFullYear() === today.getFullYear() ? "MMM d" : "yyyy-MM-dd");
    }

    Component.onCompleted: {
        refresh();
        // A note to open first ("note:ID", as the share sheet saves one).
        const arg = Quickshell.env("OMARCHY_MOBILE_APP_ARG") || "";
        if (arg.startsWith("note:")) open(arg.slice(5));
    }
    Process {
        id: tool
        property string input: ""
        property var then: null
        stdinEnabled: true
        stdout: StdioCollector {}
        onStarted: { if (input) write(input); stdinEnabled = false; }
        onExited: {
            stdinEnabled = true;
            try {
                const result = JSON.parse(stdout.text);
                if (result.ok === false) root.message = result.error || "That did not work";
                else if (then) then(result);
            } catch (e) {
                root.message = "Notes did not answer";
            }
            Qt.callLater(root.next);
        }
    }
    Process { id: keyboardTool }
    Process { id: copier }
    Timer { id: saveTimer; interval: 1000; onTriggered: root.save() }
    Timer { id: searchTimer; interval: 250; onTriggered: root.refresh() }
    Timer { id: confirmTimer; interval: 3000; onTriggered: root.confirmDelete = false }

    AppWindow {
        compact: true
        pageMargin: 16
        kicker: ""
        appTitle: "Notes"
        heading: root.view === "edit" ? (root.title || "New note") : "Notes"
        backVisible: root.view === "edit"
        // The note through the share sheet (title and text).
        headerAccessory: Component {
            TouchButton {
                visible: root.view === "edit" && editor.text.trim().length > 0
                implicitWidth: 48
                implicitHeight: 48
                label: "\u{F0497}"
                onClicked: Quickshell.execDetached([root.bin("omarchy-mobile-share"), "--title", root.title || "", "--text", editor.text])
            }
        }
        onBackClicked: root.goBack()

        Item {
            anchors.fill: parent

            // The list: search, then the notes, newest change first.
            ColumnLayout {
                anchors.fill: parent
                visible: root.view === "list"
                spacing: 12
                TouchTextField {
                    id: search
                    Layout.fillWidth: true
                    placeholderText: "Search notes"
                    inputMethodHints: Qt.ImhNoPredictiveText
                    onEditingRequested: { editing = true; forceActiveFocus(); root.keyboard("show"); }
                    onTextChanged: { root.query = text.trim(); searchTimer.restart(); }
                    onAccepted: { editing = false; root.keyboard("hide"); }
                }
                Text {
                    visible: root.notes.length === 0
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: root.query ? "No note has \"" + root.query + "\"." : "No notes yet. Tap + to write one."
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 16
                }
                ListView {
                    id: list
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 8
                    model: root.notes
                    delegate: Rectangle {
                        id: card
                        required property var modelData
                        width: list.width
                        height: cardColumn.implicitHeight + 24
                        radius: MobileTheme.radius(14)
                        color: cardTap.pressed ? MobileTheme.selection : MobileTheme.surface
                        Column {
                            id: cardColumn
                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 12 }
                            spacing: 4
                            RowLayout {
                                width: parent.width
                                Text {
                                    Layout.fillWidth: true
                                    text: card.modelData.title
                                    color: MobileTheme.foreground
                                    font.family: MobileTheme.fontFamily
                                    font.pixelSize: 17
                                    font.bold: true
                                    elide: Text.ElideRight
                                }
                                Text {
                                    text: root.when(card.modelData.modified)
                                    color: MobileTheme.secondary
                                    font.family: MobileTheme.fontFamily
                                    font.pixelSize: 13
                                }
                            }
                            Text {
                                visible: text !== ""
                                width: parent.width
                                text: card.modelData.preview
                                color: MobileTheme.secondary
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 14
                                wrapMode: Text.WordWrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                            }
                        }
                        TapHandler { id: cardTap; onTapped: root.open(card.modelData.id) }
                    }
                }
            }
            Rectangle {
                visible: root.view === "list"
                anchors { right: parent.right; bottom: parent.bottom }
                width: 64
                height: 64
                radius: MobileTheme.radius(20)
                color: newTap.pressed ? Qt.darker(MobileTheme.accent, 1.2) : MobileTheme.accent
                Text {
                    anchors.centerIn: parent
                    text: "\u{F0415}"   // plus
                    color: MobileTheme.background
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 30
                }
                TapHandler { id: newTap; onTapped: root.newNote() }
            }

            // The editor: the note's text, the first line its title.
            ColumnLayout {
                anchors.fill: parent
                visible: root.view === "edit"
                spacing: 10
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: MobileTheme.radius(14)
                    color: MobileTheme.surface
                    border.width: root.editing ? 1 : 0
                    border.color: MobileTheme.accent
                    Flickable {
                        id: scroll
                        anchors { fill: parent; margins: 14 }
                        contentHeight: editor.contentHeight
                        clip: true
                        flickableDirection: Flickable.VerticalFlick
                        TextEdit {
                            id: editor
                            width: scroll.width
                            wrapMode: TextEdit.Wrap
                            readOnly: !root.editing
                            textFormat: TextEdit.PlainText
                            color: MobileTheme.foreground
                            selectionColor: MobileTheme.selection
                            selectedTextColor: MobileTheme.foreground
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 17
                            onTextChanged: if (root.view === "edit" && activeFocus) { root.dirty = true; saveTimer.restart(); }
                            onCursorRectangleChanged: {
                                if (cursorRectangle.y + cursorRectangle.height > scroll.contentY + scroll.height)
                                    scroll.contentY = cursorRectangle.y + cursorRectangle.height - scroll.height;
                                else if (cursorRectangle.y < scroll.contentY)
                                    scroll.contentY = cursorRectangle.y;
                            }
                            Text {
                                visible: !editor.text
                                text: "Title\nThen anything"
                                color: MobileTheme.secondary
                                font: editor.font
                            }
                            // A tap starts typing, and brings back a keyboard
                            // that was put away.
                            TapHandler { onTapped: root.startEditing() }
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    TouchButton {
                        Layout.fillWidth: true
                        label: "Copy"
                        enabled: editor.text !== ""
                        onClicked: root.copy()
                    }
                    TouchButton {
                        Layout.fillWidth: true
                        label: root.confirmDelete ? "Tap again to delete" : "Delete"
                        selected: root.confirmDelete
                        onClicked: root.remove()
                    }
                }
            }

            // A passing message.
            Rectangle {
                anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 80 }
                visible: root.message !== ""
                width: messageText.implicitWidth + 32
                height: 40
                radius: MobileTheme.radius(20)
                color: MobileTheme.surface
                border.width: 1
                border.color: MobileTheme.muted
                Text {
                    id: messageText
                    anchors.centerIn: parent
                    text: root.message
                    color: MobileTheme.foreground
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 14
                }
                onVisibleChanged: if (visible) messageTimer.restart()
                Timer { id: messageTimer; interval: 2500; onTriggered: root.message = "" }
            }
        }
    }
}
