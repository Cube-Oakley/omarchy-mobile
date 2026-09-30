import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import OmarchyMobile

// Messages: conversations, a conversation, and a new message. Texts go
// through omarchy-mobile-messages (ModemManager); names and photos come from
// omarchy-mobile-contacts. Every colour comes from MobileTheme.
ShellRoot {
    id: root
    property bool ready: false
    property bool allowed: false
    property var requests: []
    property string view: "inbox"       // inbox, thread, new
    property var threads: []
    property string query: ""
    property var current: ({})          // thread, number, name, photo, initials, contact
    property var messages: []
    property var people: []
    property string recipient: ""
    property string message: ""
    property bool composing: false
    property bool sending: false
    property var segments: ({ parts: 0, left: 160, unicode: false })
    property var menuFor: null
    property var threadMenu: null
    property bool confirmDelete: false
    property var queue: []
    property real now: Date.now() / 1000
    readonly property var shownThreads: {
        const q = query.trim().toLowerCase();
        if (!q) return threads;
        return threads.filter(t => (t.name || "").toLowerCase().indexOf(q) >= 0
            || (t.number || "").indexOf(q) >= 0 || (t.body || "").toLowerCase().indexOf(q) >= 0);
    }
    readonly property var suggestions: {
        const q = recipient.trim().toLowerCase();
        const digits = q.replace(/[^0-9]/g, "");
        const out = [];
        for (let i = 0; i < people.length && out.length < 30; i++) {
            const p = people[i];
            const phones = p.phones || [];
            if (!phones.length) continue;
            const nameHit = q && (p.name || "").toLowerCase().indexOf(q) >= 0;
            for (let j = 0; j < phones.length; j++) {
                const value = phones[j].value || "";
                const hit = !q || nameHit || (digits.length >= 2 && value.replace(/[^0-9]/g, "").indexOf(digits) >= 0);
                if (hit) out.push({ name: p.name, photo: p.photo || "", initials: p.initials || "", number: value, type: phones[j].type || "" });
            }
        }
        return out;
    }
    readonly property bool typedNumber: /^\+?[0-9 ()\-.]{3,24}$/.test(recipient.trim())
        && recipient.replace(/[^0-9]/g, "").length >= 3

    function bin(name) { return Quickshell.env("HOME") + "/.local/bin/" + name; }
    function helper(args) { return [bin("omarchy-mobile-messages")].concat(args); }
    function run(kind, args, extra) {
        if (tool.running) { queue = queue.concat([[kind, args, extra]]); return; }
        tool.kind = kind;
        tool.extra = extra === undefined ? null : extra;
        tool.command = args;
        tool.running = true;
    }
    function next() {
        if (!queue.length || tool.running) return;
        const item = queue[0];
        queue = queue.slice(1);
        run(item[0], item[1], item[2]);
    }
    function refresh() {
        if (view === "thread" && current.thread) run("thread", helper(["thread", current.thread]));
        else run("threads", helper(["threads"]));
    }
    function openThread(entry) {
        current = { thread: entry.thread, number: entry.number, name: entry.name, photo: entry.photo || "",
                    initials: entry.initials || "", display: entry.display || entry.number };
        messages = [];
        view = "thread";
        message = "";
        run("thread", helper(["thread", entry.thread]));
        run("read", helper(["read", entry.thread]));
    }
    function openNumber(number, name, photo, initials) {
        const digits = number.replace(/[^0-9]/g, "");
        const key = digits.length >= 10 ? digits.slice(-10) : digits;
        openThread({ thread: key, number: number, name: name || number, photo: photo || "", initials: initials || "" });
        keyboard("hide");
    }
    function startNew() {
        recipient = "";
        view = "new";
        if (!people.length) run("people", [bin("omarchy-mobile-contacts"), "list"]);
    }
    function goBack() {
        if (menuFor || threadMenu) { menuFor = null; threadMenu = null; confirmDelete = false; return; }
        if (composing) { stopComposing(); return; }
        if (view === "thread" || view === "new") { view = "inbox"; keyboard("hide"); refresh(); return; }
        Qt.quit();
    }
    function keyboard(action) {
        keyboardTool.command = [bin("omarchy-mobile-keyboard"), action];
        keyboardTool.running = true;
    }
    function startComposing() {
        composing = true;
        compose.forceActiveFocus();
        keyboard("show");
    }
    function stopComposing() {
        composing = false;
        keyboard("hide");
    }
    function send() {
        const text = compose.text.trim();
        if (!text || sending || !current.number) return;
        sending = true;
        // Shown at once; the helper's own record replaces it on refresh.
        messages = messages.concat([{ id: -1, direction: "out", body: text, time: Date.now() / 1000, state: "sending" }]);
        compose.text = "";
        list.positionViewAtEnd();
        run("send", helper(["send", current.number, text]));
    }
    function countSegments() {
        if (segmentTool.running) { segmentAgain.restart(); return; }
        if (!compose.text) { segments = { parts: 0, left: 160, unicode: false }; return; }
        segmentTool.command = helper(["segments", compose.text]);
        segmentTool.running = true;
    }
    function copy(text) {
        copyTool.command = ["wl-copy", "--", text];
        copyTool.running = true;
    }
    function dayLabel(epoch) {
        const then = new Date(epoch * 1000), today = new Date();
        if (then.toDateString() === today.toDateString()) return "Today";
        const yesterday = new Date(today.getTime() - 86400000);
        if (then.toDateString() === yesterday.toDateString()) return "Yesterday";
        if (Date.now() / 1000 - epoch < 6 * 86400) return then.toLocaleDateString(Qt.locale(), "dddd");
        return then.toLocaleDateString(Qt.locale(), "d MMMM yyyy");
    }
    function shortTime(epoch) {
        const then = new Date(epoch * 1000);
        const minutes = Math.floor((now - epoch) / 60);
        if (minutes < 1) return "Now";
        if (minutes < 60) return minutes + " min";
        if (then.toDateString() === new Date().toDateString()) return then.toLocaleTimeString(Qt.locale(), "h:mm ap");
        if (now - epoch < 6 * 86400) return then.toLocaleDateString(Qt.locale(), "ddd");
        return then.toLocaleDateString(Qt.locale(), "d MMM");
    }
    function stateLine(item) {
        if (item.state === "sending") return "Sending…";
        if (item.state === "failed") return "Not sent · tap to try again";
        if (item.state === "unknown") return "Delivery uncertain · check with recipient";
        if (item.state === "delivered") return "Delivered";
        return "Sent";
    }
    function lastOutgoing() {
        for (let i = messages.length - 1; i >= 0; i--) if (messages[i].direction === "out") return i;
        return -1;
    }

    Component.onCompleted: run("grants", [bin("omarchy-mobile-app"), "status", "messages"])

    Process {
        id: tool
        property string kind: ""
        property var extra: null
        stdout: StdioCollector {}
        stderr: StdioCollector {}
        onExited: {
            let result = {};
            try { result = JSON.parse(stdout.text); }
            catch (e) {
                const err = (stderr.text || "").trim().split("\n").pop();
                result = { ok: false, error: err || "The messages helper did not answer" };
            }
            const kind = tool.kind;
            if (kind === "grants") {
                const items = result.permissions || [];
                root.requests = items;
                root.allowed = items.length > 0 && items.every(p => p.granted);
                root.ready = result.ok !== false;
                if (result.ok === false) root.message = result.error || "Messages is not installed";
                if (root.allowed) root.begin();
            } else if (kind === "grant") {
                if (result.ok !== false && (result.permissions || []).every(p => p.granted)) {
                    root.allowed = true;
                    root.begin();
                }
            } else if (kind === "people") {
                if (result.ok !== false) root.people = result.contacts || [];
            } else if (result.ok === false) {
                if (kind === "send") root.sending = false;
                root.message = result.error || "That didn't work";
                if (kind === "send") root.refresh();
            } else if (kind === "threads") {
                root.threads = result.threads || [];
            } else if (kind === "thread") {
                if (root.view === "thread" && result.thread === root.current.thread) {
                    const atEnd = list.atYEnd || !root.messages.length;
                    root.messages = result.messages || [];
                    if (result.name) root.current = Object.assign({}, root.current,
                        { name: result.name, photo: result.photo || "", initials: result.initials || "",
                          contact: result.contact || "", display: result.display || root.current.display,
                          number: root.current.number && /[0-9]{3}/.test(root.current.number) ? root.current.number : result.number });
                    if (atEnd) Qt.callLater(list.positionViewAtEnd);
                }
            } else if (kind === "send") {
                root.sending = false;
                root.message = "";
                root.refresh();
            } else if (kind === "retry" || kind === "delete") {
                root.refresh();
            } else if (kind === "delete-thread") {
                root.view = "inbox";
                root.refresh();
            }
            Qt.callLater(root.next);
        }
    }
    function begin() {
        const arg = Quickshell.env("OMARCHY_MOBILE_APP_ARG") || "";
        if (arg.startsWith("thread:")) {
            const key = arg.slice(7);
            openThread({ thread: key, number: key, name: key });
        } else if (arg.startsWith("sms:")) {
            openNumber(arg.slice(4), "", "", "");
        }
        refresh();
        watcher.running = true;
        run("people", [bin("omarchy-mobile-contacts"), "list"]);
    }
    // Live updates: the helper reports each change to the message store.
    Process {
        id: watcher
        command: root.helper(["watch"])
        stdout: SplitParser {
            onRead: line => {
                let data = {};
                try { data = JSON.parse(line); } catch (e) { return; }
                if (data.ok === false) return;
                root.now = Date.now() / 1000;
                if (root.view === "thread" && root.current.thread) {
                    root.run("thread", root.helper(["thread", root.current.thread]));
                    if ((data.new || []).some(m => m.thread === root.current.thread))
                        root.run("read", root.helper(["read", root.current.thread]));
                }
                root.run("threads", root.helper(["threads"]));
            }
        }
        onExited: if (root.allowed) watchRetry.start()
    }
    Timer { id: watchRetry; interval: 3000; onTriggered: watcher.running = true }
    Timer { interval: 30000; repeat: true; running: root.allowed; onTriggered: root.now = Date.now() / 1000 }
    Process { id: keyboardTool }
    Process { id: copyTool }
    Process {
        id: segmentTool
        stdout: StdioCollector {
            onStreamFinished: { try { root.segments = JSON.parse(this.text); } catch (e) {} }
        }
    }
    Timer { id: segmentAgain; interval: 120; onTriggered: root.countSegments() }

    component Glyph: Text {
        color: MobileTheme.foreground
        font.family: MobileTheme.fontFamily
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    component IconButton: Rectangle {
        id: iconButton
        property string glyph: ""
        property color tint: "transparent"
        property color ink: MobileTheme.foreground
        property bool enabled: true
        signal clicked()
        implicitWidth: 48
        implicitHeight: 48
        radius: MobileTheme.radius(24)
        opacity: enabled ? 1 : 0.4
        color: iconTap.pressed ? MobileTheme.muted : tint
        Glyph { anchors.centerIn: parent; text: iconButton.glyph; color: iconButton.ink; font.pixelSize: 22 }
        TapHandler { id: iconTap; enabled: iconButton.enabled; onTapped: iconButton.clicked() }
    }

    AppWindow {
        id: window
        compact: true
        pageMargin: 16
        kicker: ""
        appTitle: "Messages"
        heading: root.view === "thread" ? (root.current.name || root.current.display || "")
               : root.view === "new" ? "New message" : "Messages"
        backVisible: root.allowed && root.view !== "inbox"
        onBackClicked: root.goBack()

        GrantPage {
            anchors.fill: parent
            visible: root.ready && !root.allowed
            requests: root.requests
            onAllowed: {
                for (let i = 0; i < root.requests.length; i++)
                    root.run("grant", [root.bin("omarchy-mobile-app"), "grant", "messages", root.requests[i].id]);
            }
            onDenied: Qt.quit()
        }

        // Conversations.
        ColumnLayout {
            anchors.fill: parent
            visible: root.allowed && root.view === "inbox"
            spacing: 10
            TouchTextField {
                id: search
                Layout.fillWidth: true
                placeholderText: "\u{F0349}  Search messages"
                onTextChanged: root.query = text
                onEditingRequested: { editing = true; forceActiveFocus(); root.keyboard("show"); }
                onEditingChanged: if (!editing) root.keyboard("hide")
                visible: root.threads.length > 0
            }
            ListView {
                id: inbox
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 4
                model: root.shownThreads
                delegate: Rectangle {
                    id: rowItem
                    required property var modelData
                    readonly property bool unread: modelData.unread > 0
                    width: inbox.width
                    implicitHeight: 76
                    radius: MobileTheme.radius(14)
                    color: rowTap.pressed ? MobileTheme.selection : unread ? MobileTheme.surface : "transparent"
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 12
                        spacing: 12
                        Avatar {
                            size: 50
                            photo: rowItem.modelData.photo || ""
                            initials: rowItem.modelData.initials || ""
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3
                            RowLayout {
                                Layout.fillWidth: true
                                Text {
                                    Layout.fillWidth: true
                                    text: rowItem.modelData.name
                                    textFormat: Text.PlainText
                                    color: MobileTheme.foreground
                                    font.family: MobileTheme.fontFamily
                                    font.pixelSize: 16
                                    font.bold: rowItem.unread
                                    elide: Text.ElideRight
                                }
                                Text {
                                    text: root.shortTime(rowItem.modelData.time)
                                    color: rowItem.unread ? MobileTheme.accent : MobileTheme.secondary
                                    font.family: MobileTheme.fontFamily
                                    font.pixelSize: 12
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text {
                                    Layout.fillWidth: true
                                    text: (rowItem.modelData.direction === "out" ? "You: " : "") + rowItem.modelData.body.replace(/\s+/g, " ")
                                    textFormat: Text.PlainText
                                    color: rowItem.modelData.state === "failed" ? MobileTheme.danger
                                         : rowItem.unread ? MobileTheme.foreground : MobileTheme.secondary
                                    font.family: MobileTheme.fontFamily
                                    font.pixelSize: 14
                                    elide: Text.ElideRight
                                    maximumLineCount: 1
                                }
                                Rectangle {
                                    visible: rowItem.unread
                                    implicitWidth: Math.max(22, badge.implicitWidth + 12)
                                    implicitHeight: 22
                                    radius: MobileTheme.radius(11)
                                    color: MobileTheme.accent
                                    Text {
                                        id: badge
                                        anchors.centerIn: parent
                                        text: rowItem.modelData.unread
                                        color: MobileTheme.background
                                        font.family: MobileTheme.fontFamily
                                        font.pixelSize: 12
                                        font.bold: true
                                    }
                                }
                            }
                        }
                    }
                    TapHandler {
                        id: rowTap
                        longPressThreshold: 0.5
                        onTapped: root.openThread(rowItem.modelData)
                        onLongPressed: { root.threadMenu = rowItem.modelData; root.confirmDelete = false; }
                    }
                }
                Column {
                    anchors.centerIn: parent
                    visible: root.threads.length === 0
                    spacing: 10
                    Glyph { anchors.horizontalCenter: parent.horizontalCenter; text: "\u{F036A}"; font.pixelSize: 56; color: MobileTheme.muted }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "No messages yet"
                        color: MobileTheme.secondary
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 15
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                visible: root.message.length > 0
                text: root.message
                color: MobileTheme.danger
                font.family: MobileTheme.fontFamily
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }
            TouchButton {
                Layout.fillWidth: true
                label: "\u{F0653}  New message"
                selected: true
                onClicked: root.startNew()
            }
        }

        // A new message: pick a contact or type a number.
        ColumnLayout {
            anchors.fill: parent
            visible: root.allowed && root.view === "new"
            spacing: 10
            TouchTextField {
                id: to
                Layout.fillWidth: true
                placeholderText: "To: name or number"
                editing: root.view === "new"
                inputMethodHints: Qt.ImhNoPredictiveText
                onTextChanged: root.recipient = text
                onEditingRequested: { forceActiveFocus(); root.keyboard("show"); }
                onAccepted: if (root.typedNumber) root.openNumber(root.recipient.trim(), "", "", "")
                onVisibleChanged: if (visible) { text = ""; forceActiveFocus(); }
            }
            Rectangle {
                visible: root.typedNumber
                Layout.fillWidth: true
                implicitHeight: 60
                radius: MobileTheme.radius(14)
                color: typedTap.pressed ? MobileTheme.selection : MobileTheme.surface
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 12
                    Glyph { text: "\u{F0653}"; font.pixelSize: 24; color: MobileTheme.accent }
                    Text {
                        Layout.fillWidth: true
                        text: "Send to " + root.recipient.trim()
                        textFormat: Text.PlainText
                        color: MobileTheme.foreground
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 16
                        elide: Text.ElideRight
                    }
                }
                TapHandler { id: typedTap; onTapped: root.openNumber(root.recipient.trim(), "", "", "") }
            }
            ListView {
                id: picker
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 4
                model: root.suggestions
                delegate: Rectangle {
                    id: suggestion
                    required property var modelData
                    width: picker.width
                    implicitHeight: 64
                    radius: MobileTheme.radius(14)
                    color: pickTap.pressed ? MobileTheme.selection : "transparent"
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 12
                        Avatar { size: 44; photo: suggestion.modelData.photo; initials: suggestion.modelData.initials }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                Layout.fillWidth: true
                                text: suggestion.modelData.name
                                textFormat: Text.PlainText
                                color: MobileTheme.foreground
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 16
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: suggestion.modelData.number + (suggestion.modelData.type ? "  ·  " + suggestion.modelData.type : "")
                                textFormat: Text.PlainText
                                color: MobileTheme.secondary
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 13
                                elide: Text.ElideRight
                            }
                        }
                    }
                    TapHandler {
                        id: pickTap
                        onTapped: root.openNumber(suggestion.modelData.number, suggestion.modelData.name,
                                                  suggestion.modelData.photo, suggestion.modelData.initials)
                    }
                }
                Text {
                    anchors.centerIn: parent
                    visible: root.suggestions.length === 0 && !root.typedNumber
                    width: picker.width - 32
                    text: root.people.length ? "No matching contacts" : "Type a phone number"
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 15
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }
            }
        }

        // One conversation.
        ColumnLayout {
            anchors.fill: parent
            visible: root.allowed && root.view === "thread"
            spacing: 8
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                Avatar { size: 40; photo: root.current.photo || ""; initials: root.current.initials || "" }
                Text {
                    Layout.fillWidth: true
                    text: root.current.name && root.current.name !== root.current.display ? (root.current.display || root.current.number || "") : ""
                    textFormat: Text.PlainText
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }
                IconButton {
                    glyph: root.current.contact ? "\u{F0004}" : "\u{F0014}"
                    ink: MobileTheme.accent
                    visible: /[0-9]/.test(root.current.number || "")
                    onClicked: Quickshell.execDetached([root.bin("omarchy-mobile-app"), "launch", "contacts",
                        root.current.contact ? "uid:" + root.current.contact : "new:" + root.current.number])
                }
                IconButton {
                    glyph: "\u{F03F2}"
                    ink: MobileTheme.success
                    visible: /[0-9]/.test(root.current.number || "")
                    onClicked: Quickshell.execDetached([root.bin("omarchy-mobile-app"), "launch", "phone", "tel:" + root.current.number])
                }
            }
            ListView {
                id: list
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 3
                model: root.messages
                onCountChanged: if (atYEnd || count < 3) Qt.callLater(positionViewAtEnd)
                delegate: Item {
                    id: bubbleRow
                    required property var modelData
                    required property int index
                    readonly property bool mine: modelData.direction === "out"
                    readonly property var previous: index > 0 ? root.messages[index - 1] : null
                    readonly property bool newDay: !previous || new Date(previous.time * 1000).toDateString() !== new Date(modelData.time * 1000).toDateString()
                    readonly property bool gap: !previous || modelData.time - previous.time > 600 || previous.direction !== modelData.direction
                    readonly property bool showState: mine && index === root.lastOutgoing()
                    width: list.width
                    implicitHeight: column.implicitHeight
                    Column {
                        id: column
                        width: parent.width
                        spacing: 4
                        topPadding: bubbleRow.gap ? 8 : 0
                        Text {
                            visible: bubbleRow.newDay
                            width: parent.width
                            topPadding: 8
                            bottomPadding: 4
                            text: root.dayLabel(bubbleRow.modelData.time)
                            color: MobileTheme.secondary
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 12
                            font.letterSpacing: 1.2
                            horizontalAlignment: Text.AlignHCenter
                        }
                        Rectangle {
                            id: bubble
                            x: bubbleRow.mine ? parent.width - width : 0
                            width: Math.min(body.implicitWidth + 28, parent.width * 0.8)
                            height: body.implicitHeight + 20
                            radius: MobileTheme.radius(18)
                            color: bubbleRow.modelData.state === "failed" ? MobileTheme.surface
                                 : bubbleRow.mine ? MobileTheme.accent : MobileTheme.surface
                            border.width: bubbleRow.modelData.state === "failed" ? 1 : 0
                            border.color: MobileTheme.danger
                            opacity: bubbleRow.modelData.state === "sending" ? 0.7 : 1
                            TextEdit {
                                id: body
                                anchors.centerIn: parent
                                width: Math.min(implicitWidth, bubbleRow.width * 0.8 - 28)
                                text: bubbleRow.modelData.body
                                textFormat: TextEdit.PlainText
                                readOnly: true
                                selectByMouse: false
                                wrapMode: TextEdit.Wrap
                                color: bubbleRow.mine && bubbleRow.modelData.state !== "failed" ? MobileTheme.background : MobileTheme.foreground
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 16
                            }
                            TapHandler {
                                longPressThreshold: 0.45
                                onTapped: if (bubbleRow.modelData.state === "failed") root.run("retry", root.helper(["retry", String(bubbleRow.modelData.id)]))
                                onLongPressed: root.menuFor = bubbleRow.modelData
                            }
                        }
                        Text {
                            visible: bubbleRow.showState || (bubbleRow.gap && !bubbleRow.mine)
                            width: parent.width
                            text: bubbleRow.mine ? root.stateLine(bubbleRow.modelData) + " · " + root.shortTime(bubbleRow.modelData.time)
                                                 : root.shortTime(bubbleRow.modelData.time)
                            color: bubbleRow.modelData.state === "failed" ? MobileTheme.danger : MobileTheme.secondary
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 11
                            horizontalAlignment: bubbleRow.mine ? Text.AlignRight : Text.AlignLeft
                        }
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                visible: root.message.length > 0
                text: root.message
                color: MobileTheme.danger
                font.family: MobileTheme.fontFamily
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: Math.min(Math.max(48, compose.contentHeight + 24), 150)
                    radius: MobileTheme.radius(20)
                    color: MobileTheme.surface
                    border.width: root.composing ? 1 : 0
                    border.color: MobileTheme.accent
                    Flickable {
                        id: composeScroll
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 14
                        anchors.topMargin: 12
                        anchors.bottomMargin: 12
                        contentHeight: compose.contentHeight
                        clip: true
                        flickableDirection: Flickable.VerticalFlick
                        TextEdit {
                            id: compose
                            width: composeScroll.width
                            wrapMode: TextEdit.Wrap
                            readOnly: !root.composing
                            color: MobileTheme.foreground
                            selectionColor: MobileTheme.selection
                            selectedTextColor: MobileTheme.foreground
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 16
                            textFormat: TextEdit.PlainText
                            onTextChanged: root.countSegments()
                            onCursorRectangleChanged: {
                                if (cursorRectangle.y + cursorRectangle.height > composeScroll.contentY + composeScroll.height)
                                    composeScroll.contentY = cursorRectangle.y + cursorRectangle.height - composeScroll.height;
                            }
                            Text {
                                visible: !compose.text
                                text: "Text message"
                                color: MobileTheme.secondary
                                font: compose.font
                            }
                            // Typing already: a tap places the cursor, and
                            // brings back a keyboard that was put away.
                            TapHandler {
                                enabled: root.composing
                                onTapped: root.keyboard("show")
                            }
                        }
                    }
                    // Not typing: a tap anywhere on the box starts. The text
                    // underneath would otherwise take the press itself.
                    MouseArea {
                        anchors.fill: parent
                        enabled: !root.composing
                        onClicked: root.startComposing()
                    }
                }
                Column {
                    spacing: 2
                    Text {
                        visible: root.segments.parts > 1 || (root.segments.parts === 1 && root.segments.left < 20)
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: root.segments.left + (root.segments.parts > 1 ? " / " + root.segments.parts : "")
                        color: MobileTheme.secondary
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 11
                    }
                    IconButton {
                        glyph: "\u{F048A}"
                        tint: MobileTheme.accent
                        ink: MobileTheme.background
                        enabled: compose.text.trim().length > 0 && !root.sending
                        onClicked: root.send()
                    }
                }
            }
        }

        // Long-press on a message.
        MenuOverlay {
            open: root.menuFor !== null
            onDismissed: root.menuFor = null
            ColumnLayout {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 8
                spacing: 8
                TouchButton { Layout.fillWidth: true; label: "\u{F018F}  Copy"; onClicked: { root.copy(root.menuFor.body); root.menuFor = null; } }
                TouchButton {
                    Layout.fillWidth: true
                    visible: root.menuFor !== null && root.menuFor.state === "failed"
                    label: "\u{F0450}  Send again"
                    onClicked: { root.run("retry", root.helper(["retry", String(root.menuFor.id)])); root.menuFor = null; }
                }
                TouchButton {
                    Layout.fillWidth: true
                    visible: root.menuFor !== null && root.menuFor.id >= 0
                    label: "\u{F01B4}  Delete"
                    onClicked: { root.run("delete", root.helper(["delete", String(root.menuFor.id)])); root.menuFor = null; }
                }
                TouchButton { Layout.fillWidth: true; label: "Cancel"; onClicked: root.menuFor = null }
            }
        }
        // Long-press on a conversation.
        MenuOverlay {
            open: root.threadMenu !== null
            onDismissed: { root.threadMenu = null; root.confirmDelete = false; }
            ColumnLayout {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 8
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    text: root.threadMenu ? root.threadMenu.name : ""
                    textFormat: Text.PlainText
                    color: MobileTheme.foreground
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 18
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                }
                TouchButton {
                    Layout.fillWidth: true
                    visible: root.threadMenu !== null && root.threadMenu.unread > 0
                    label: "\u{F012C}  Mark as read"
                    onClicked: { root.run("read", root.helper(["read", root.threadMenu.thread])); root.run("threads", root.helper(["threads"])); root.threadMenu = null; }
                }
                TouchButton {
                    Layout.fillWidth: true
                    label: root.confirmDelete ? "Tap again to delete this conversation" : "\u{F01B4}  Delete conversation"
                    onClicked: {
                        if (!root.confirmDelete) { root.confirmDelete = true; return; }
                        root.run("delete-thread", root.helper(["delete-thread", root.threadMenu.thread]));
                        root.threadMenu = null;
                        root.confirmDelete = false;
                    }
                }
                TouchButton { Layout.fillWidth: true; label: "Cancel"; onClicked: { root.threadMenu = null; root.confirmDelete = false; } }
            }
        }
    }
}
