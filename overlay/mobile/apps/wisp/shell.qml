import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import OmarchyMobile

// Wisp: the assistant, through omarchy-mobile-assistant. A language model on
// the phone answers by text, or by voice (dictation in, Kokoro's voices out), and sees
// pictures; chats are kept and searched, and what the user says about
// themselves is remembered. Every colour comes from MobileTheme.
ShellRoot {
    id: root
    property string view: "chats"       // chats, chat, memory, settings
    property var chats: []
    property string query: ""
    property int conversation: 0        // 0: a chat not started yet
    property string chatTitle: ""
    property var messages: []
    property string streaming: ""       // the reply as it arrives
    property bool thinking: false       // sent, nothing back yet
    property string error: ""
    property string notice: ""
    property var memories: []
    property var status: ({})
    property bool composing: false
    property bool listening: false
    property bool micLive: false        // the microphone hears ("listening" from the helper)
    property string heardDone: ""       // dictated phrases so far
    property string heardNow: ""        // the phrase being spoken
    property bool voiceTurn: false      // asked by voice: the reply is spoken
    property bool speakerStarted: false
    property bool speaking: false
    property var pendingSpeech: []      // pieces for the speaker before it has started
    property int spokenTo: 0            // how much of the streaming reply went to the speaker
    property string picture: ""         // the picture going with the next message
    property string capturePath: ""
    property bool pickerOpen: false
    property var recentPhotos: []
    property var menuFor: null          // a long-pressed chat or message
    property bool askMic: false
    property bool micAllowed: false
    property string installing: ""      // a voice or a model id
    property bool previewPending: false // a voice was picked: say hello in it
    property real progress: 0
    property string installState: ""
    property var queue: []
    // A Process started while the window is still being built does not
    // report running yet: the queue keeps its own flag.
    property bool toolBusy: false
    readonly property var settings: status.settings || ({})
    readonly property bool busy: thinking || sender.running
    readonly property bool speakReplies: status.voice === true && (voiceTurn || settings.speak === true)
    readonly property string home: Quickshell.env("HOME")

    function bin(name) { return home + "/.local/bin/" + name; }
    function helper(args) { return [bin("omarchy-mobile-assistant")].concat(args); }
    function run(kind, args) {
        if (toolBusy) { queue = queue.concat([[kind, args]]); return; }
        toolBusy = true;
        tool.kind = kind;
        tool.command = args;
        tool.running = true;
    }
    function next() {
        if (!queue.length || toolBusy) return;
        const item = queue[0];
        queue = queue.slice(1);
        run(item[0], item[1]);
    }
    function refresh() { run("chats", helper(["conversations"].concat(query.trim() ? [query.trim()] : []))); }
    function refreshStatus() { run("status", helper(["status"])); }
    function keyboard(action) {
        keyboardTool.command = [bin("omarchy-mobile-keyboard"), action];
        keyboardTool.running = true;
    }
    function openChat(entry) {
        conversation = entry.id;
        chatTitle = entry.title;
        messages = [];
        streaming = "";
        error = "";
        view = "chat";
        run("messages", helper(["messages", String(entry.id)]));
    }
    function newChat() {
        conversation = 0;
        chatTitle = "";
        messages = [];
        streaming = "";
        error = "";
        view = "chat";
    }
    function goBack() {
        if (pickerOpen) { pickerOpen = false; return; }
        if (menuFor !== null || askMic) { menuFor = null; askMic = false; return; }
        if (listening) { stopListening(); return; }
        if (composing) { stopComposing(); return; }
        if (view !== "chats") {
            if (view === "chat") stopSpeaking();
            view = "chats";
            keyboard("hide");
            refresh();
            return;
        }
        Qt.quit();
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
    function send(text) {
        text = text.trim();
        if ((!text && !picture) || busy) return;
        error = "";
        stopSpeaking();
        spokenTo = 0;
        if (speakReplies) ensureSpeaker();
        messages = messages.concat([{ role: "user", content: text, image: picture, created: Date.now() / 1000 }]);
        if (!conversation) chatTitle = text.length > 40 ? text.slice(0, 39) + "…" : (text || "Picture");
        streaming = "";
        thinking = true;
        sender.input = text;
        sender.command = helper(["send"].concat(conversation ? [String(conversation)] : [])
                                        .concat(picture ? ["--image", picture] : []));
        sender.running = true;
        compose.text = "";
        picture = "";
        Qt.callLater(list.positionViewAtEnd);
    }
    function stopReply() { sender.running = false; stopSpeaking(); }
    // The speaker keeps the voice loaded while Wisp is open (it takes
    // seconds to load); it is started when a spoken reply is likely.
    function ensureSpeaker() {
        if (status.voice === true && !speaker.running) speaker.running = true;
    }
    function speak(text) {
        if (status.voice !== true || !text.trim()) return;
        ensureSpeaker();
        const line = JSON.stringify({ say: text }) + "\n";
        if (speakerStarted) speaker.write(line);
        else pendingSpeech = pendingSpeech.concat([line]);
    }
    function stopSpeaking() {
        pendingSpeech = [];
        if (speakerStarted) speaker.write('{"stop": true}\n');
    }
    // A voice picked in Settings: the speaker starts again with it and
    // says hello.
    function pickVoice(id) {
        run("voice", helper(["set", "voice", id]));
    }
    function previewVoice() {
        stopSpeaking();
        if (speaker.running) { previewPending = true; speaker.running = false; }
        else speak("Hi, I'm Wisp. This is how I sound.");
    }
    // Whole sentences of the reply as they arrive, the rest at its end.
    function speakAlong(text, final) {
        let end = final ? text.length : spokenTo;
        if (!final) {
            const boundary = /[.!?…]["')\]]*\s|\n/g;
            boundary.lastIndex = spokenTo;
            let found;
            while ((found = boundary.exec(text)) !== null) end = found.index + found[0].length;
        }
        const piece = text.slice(spokenTo, end).trim();
        // Not a list's "1." alone.
        if (!final && piece.length < 16) return;
        spokenTo = end;
        if (piece) speak(piece);
    }
    function startListening() {
        if (!micAllowed) { askMic = true; return; }
        if (busy) return;
        stopSpeaking();
        ensureSpeaker();
        stopComposing();
        heardDone = "";
        heardNow = "";
        error = "";
        micLive = false;
        listening = true;
        listener.running = true;
    }
    function stopListening() {
        if (listener.running) listener.write("stop\n");
        else listening = false;
    }
    function heardText() { return (heardDone + " " + heardNow).trim(); }
    function openPicker() {
        pickerOpen = true;
        run("photos", helper(["photos"]));
    }
    function takePhoto() {
        pickerOpen = false;
        run("capture", helper(["capture"]));
    }
    function copy(text) {
        copyTool.command = ["wl-copy", "--", text];
        copyTool.running = true;
    }
    function escaped(text) {
        return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    }
    function shortTime(epoch) {
        const d = new Date(epoch * 1000);
        const now = new Date();
        if (d.toDateString() === now.toDateString()) return Qt.formatTime(d, "h:mm AP");
        if (now - d < 6 * 86400000) return Qt.formatDate(d, "ddd");
        return Qt.formatDate(d, "MMM d");
    }

    Component.onCompleted: {
        refresh();
        refreshStatus();
        run("grants", [bin("omarchy-mobile-app"), "status", "wisp"]);
    }

    // One-shot commands: one JSON object each.
    Process {
        id: tool
        property string kind: ""
        stdout: StdioCollector {}
        onExited: {
            root.toolBusy = false;
            let result = {};
            try { result = JSON.parse(stdout.text); } catch (e) {}
            const kind = tool.kind;
            if (result.error) root.error = result.error;
            else if (kind === "chats") root.chats = result.conversations || [];
            else if (kind === "messages" && result.conversation === root.conversation) {
                root.messages = result.messages || [];
                Qt.callLater(list.positionViewAtEnd);
            } else if (kind === "status") root.status = result;
            else if (kind === "memories") root.memories = result.memories || [];
            else if (kind === "grants") root.micAllowed = (result.permissions || []).every(p => p.granted);
            else if (kind === "grant") {
                root.micAllowed = (result.permissions || []).every(p => p.granted);
                if (root.micAllowed) root.startListening();
            } else if (kind === "photos") root.recentPhotos = result.photos || [];
            else if (kind === "capture" && result.path) {
                root.capturePath = result.path;
                captureWait.command = ["sh", "-c", 'for i in $(seq 1200); do [ -s "$1" ] && exit 0; sleep 0.25; done; exit 1',
                                       "wait", result.path];
                captureWait.running = true;
            } else if (kind === "voice") {
                root.refreshStatus();
                root.previewVoice();
            } else if (kind === "settings" || kind === "forget" || kind === "remember") {
                if (kind === "settings") root.refreshStatus();
                else root.run("memories", root.helper(["memories"]));
            } else if (kind === "delete") root.refresh();
            Qt.callLater(root.next);
        }
    }
    // A message and its reply, as it comes.
    Process {
        id: sender
        property string input: ""
        stdinEnabled: true
        onStarted: { write(input); stdinEnabled = false; }
        stdout: SplitParser {
            onRead: line => {
                let data = {};
                try { data = JSON.parse(line); } catch (e) { return; }
                if (data.conversation && !data.done) root.conversation = data.conversation;
                if (data.delta !== undefined) {
                    root.thinking = false;
                    root.streaming += data.delta;
                    // Each sentence as it is written: kokoro-pi keeps ahead
                    // even beside the model (no gaps; the first sound came
                    // sooner than waiting for the whole reply).
                    if (root.speakReplies) root.speakAlong(root.streaming, false);
                    if (list.atYEnd || list.followEnd) Qt.callLater(list.positionViewAtEnd);
                } else if (data.done) {
                    if (root.speakReplies) root.speakAlong(root.streaming, true);
                    root.messages = root.messages.concat([{ role: "assistant", content: data.text, created: Date.now() / 1000 }]);
                    root.streaming = "";
                    root.thinking = false;
                    root.voiceTurn = false;
                    Qt.callLater(list.positionViewAtEnd);
                } else if (data.memory) {
                    root.notice = "Remembered: " + data.memory;
                    noticeTimer.restart();
                } else if (data.error) {
                    root.error = data.error;
                    root.thinking = false;
                }
            }
        }
        onExited: {
            stdinEnabled = true;
            if (root.streaming) {
                // Stopped part way: what came is kept.
                root.messages = root.messages.concat([{ role: "assistant", content: root.streaming, created: Date.now() / 1000 }]);
                root.streaming = "";
            }
            root.thinking = false;
            root.voiceTurn = false;
        }
    }
    // Speech in: Moonshine, through omarchy-mobile-dictate.
    Process {
        id: listener
        command: [root.bin("omarchy-mobile-dictate"), "listen"]
        stdinEnabled: true
        stdout: SplitParser {
            onRead: line => {
                let data = {};
                try { data = JSON.parse(line); } catch (e) { return; }
                if (data.state === "listening") root.micLive = true;
                else if (data.partial !== undefined) root.heardNow = data.partial;
                else if (data.line !== undefined) { root.heardDone = (root.heardDone + " " + data.line).trim(); root.heardNow = ""; }
                else if (data.error) root.error = data.setup ? "Voice typing is not set up yet: Settings, Keyboard, Voice typing" : data.error;
            }
        }
        onExited: {
            const text = root.heardText();
            root.listening = false;
            root.micLive = false;
            root.heardDone = "";
            root.heardNow = "";
            if (text) { root.voiceTurn = true; root.send(text); }
        }
    }
    // Speech out: the voice, loaded once (it exits after five quiet minutes).
    Process {
        id: speaker
        command: root.helper(["voice", "serve"])
        stdinEnabled: true
        stdout: SplitParser {
            onRead: line => {
                let data = {};
                try { data = JSON.parse(line); } catch (e) { return; }
                if (data.speaking !== undefined) root.speaking = data.speaking;
                else if (data.error) root.error = data.error;
            }
        }
        onStarted: {
            root.speakerStarted = true;
            for (const line of root.pendingSpeech) write(line);
            root.pendingSpeech = [];
        }
        onExited: {
            root.speakerStarted = false;
            root.speaking = false;
            if (root.previewPending) {
                root.previewPending = false;
                root.speak("Hi, I'm Wisp. This is how I sound.");
            }
        }
    }
    // Downloads: the voice or a model, with progress.
    Process {
        id: installer
        stdout: SplitParser {
            onRead: line => {
                let data = {};
                try { data = JSON.parse(line); } catch (e) { return; }
                if (data.progress !== undefined) root.progress = data.progress;
                if (data.state) root.installState = data.state;
                if (data.error) root.error = data.error;
            }
        }
        onExited: code => {
            // The voice installed from Settings says hello.
            if (code === 0 && root.installing === "voice") root.previewVoice();
            root.installing = "";
            root.progress = 0;
            root.installState = "";
            root.refreshStatus();
        }
    }
    Process {
        id: captureWait
        onExited: code => {
            if (code === 0 && root.capturePath !== "") root.picture = root.capturePath;
            root.capturePath = "";
        }
    }
    Process { id: keyboardTool }
    Process { id: copyTool }
    Timer { id: noticeTimer; interval: 4000; onTriggered: root.notice = "" }
    Timer { id: searchDelay; interval: 250; onTriggered: root.refresh() }
    onQueryChanged: searchDelay.restart()

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
    component Toggle: Rectangle {
        id: toggle
        property string label: ""
        property string detail: ""
        property bool checked: false
        property bool enabled: true
        signal toggled()
        Layout.fillWidth: true
        implicitHeight: toggleText.implicitHeight + 28
        radius: MobileTheme.radius(16)
        color: MobileTheme.surface
        opacity: enabled ? 1 : 0.5
        ColumnLayout {
            id: toggleText
            anchors { left: parent.left; right: knob.left; verticalCenter: parent.verticalCenter; margins: 16 }
            spacing: 2
            Text { text: toggle.label; color: MobileTheme.foreground; font.family: MobileTheme.fontFamily; font.pixelSize: 16 }
            Text {
                Layout.fillWidth: true
                visible: toggle.detail !== ""
                text: toggle.detail
                color: MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }
        }
        Rectangle {
            id: knob
            anchors { right: parent.right; rightMargin: 16; verticalCenter: parent.verticalCenter }
            width: 48; height: 28; radius: 14
            color: toggle.checked ? MobileTheme.accent : MobileTheme.muted
            Rectangle {
                width: 22; height: 22; radius: 11
                y: 3
                x: toggle.checked ? parent.width - width - 3 : 3
                color: MobileTheme.background
                Behavior on x { NumberAnimation { duration: 120 } }
            }
        }
        TapHandler { enabled: toggle.enabled; onTapped: toggle.toggled() }
    }

    AppWindow {
        id: window
        compact: true
        pageMargin: 16
        kicker: ""
        appTitle: "Wisp"
        heading: root.view === "chat" ? (root.chatTitle || "New chat")
               : root.view === "memory" ? "Memory" : root.view === "settings" ? "Settings" : "Wisp"
        backVisible: root.view !== "chats"
        onBackClicked: root.goBack()
        headerAccessory: Component {
            RowLayout {
                spacing: 2
                IconButton {
                    visible: root.view === "chats"
                    glyph: "\u{F09D1}"
                    ink: MobileTheme.secondary
                    onClicked: { root.view = "memory"; root.run("memories", root.helper(["memories"])); }
                }
                IconButton {
                    visible: root.view === "chats"
                    glyph: "\u{F0493}"
                    ink: MobileTheme.secondary
                    onClicked: { root.view = "settings"; root.refreshStatus(); }
                }
                // Read replies aloud (needs the voice, in Settings).
                IconButton {
                    visible: root.view === "chat" && root.status.voice === true
                    glyph: root.settings.speak ? "\u{F057E}" : "\u{F0581}"
                    ink: root.settings.speak ? MobileTheme.accent : MobileTheme.secondary
                    onClicked: {
                        if (root.settings.speak) root.stopSpeaking();
                        else root.ensureSpeaker();
                        root.run("settings", root.helper(["set", "speak", root.settings.speak ? "false" : "true"]));
                    }
                }
            }
        }

        // Chats: search, the list, and a new one.
        ColumnLayout {
            anchors.fill: parent
            visible: root.view === "chats"
            spacing: 10
            TouchTextField {
                id: search
                Layout.fillWidth: true
                visible: root.chats.length > 0 || root.query !== ""
                placeholderText: "\u{F0349}  Search chats"
                onTextChanged: root.query = text
                onEditingRequested: { editing = true; forceActiveFocus(); root.keyboard("show"); }
                onEditingChanged: if (!editing) root.keyboard("hide")
            }
            ListView {
                id: chatList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 6
                model: root.chats
                delegate: Rectangle {
                    id: chatRow
                    required property var modelData
                    width: chatList.width
                    implicitHeight: chatColumn.implicitHeight + 24
                    radius: MobileTheme.radius(16)
                    color: chatTap.pressed ? MobileTheme.selection : MobileTheme.surface
                    ColumnLayout {
                        id: chatColumn
                        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: 14 }
                        spacing: 3
                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                Layout.fillWidth: true
                                text: chatRow.modelData.title || "Chat"
                                textFormat: Text.PlainText
                                color: MobileTheme.foreground
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 16
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            Text {
                                text: root.shortTime(chatRow.modelData.updated)
                                color: MobileTheme.secondary
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 12
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            // A search's matches come between \x01 and \x02.
                            text: root.escaped(chatRow.modelData.snippet.replace(/\s+/g, " "))
                                      .replace(/\x01/g, "<b><font color='" + MobileTheme.accent + "'>")
                                      .replace(/\x02/g, "</font></b>")
                            textFormat: Text.StyledText
                            color: MobileTheme.secondary
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 14
                            maximumLineCount: 2
                            wrapMode: Text.WordWrap
                            elide: Text.ElideRight
                        }
                    }
                    TapHandler {
                        id: chatTap
                        longPressThreshold: 0.45
                        onTapped: root.openChat(chatRow.modelData)
                        onLongPressed: root.menuFor = { chat: chatRow.modelData }
                    }
                }
                Column {
                    anchors.centerIn: parent
                    width: parent.width - 48
                    visible: root.chats.length === 0
                    spacing: 12
                    Image {
                        anchors.horizontalCenter: parent.horizontalCenter
                        visible: root.query === ""
                        source: Quickshell.iconPath("omarchy-wisp", true)
                        sourceSize.width: 96
                        sourceSize.height: 96
                    }
                    Text {
                        width: parent.width
                        text: root.query !== "" ? "No chats match" :
                              "Ask Wisp anything, by typing or by voice. It runs on this phone, offline, and remembers what you tell it about yourself."
                        color: MobileTheme.secondary
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 15
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                    }
                }
            }
            TouchButton {
                Layout.fillWidth: true
                label: "\u{F0415}  New chat"
                onClicked: root.newChat()
            }
        }

        // One chat.
        ColumnLayout {
            anchors.fill: parent
            visible: root.view === "chat"
            spacing: 8
            ListView {
                id: list
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 8
                model: root.messages
                property bool followEnd: true
                onMovementEnded: followEnd = atYEnd
                onContentHeightChanged: if (followEnd && !moving) Qt.callLater(positionViewAtEnd)
                delegate: Item {
                    id: bubbleRow
                    required property var modelData
                    readonly property bool mine: modelData.role === "user"
                    width: list.width
                    implicitHeight: bubbleColumn.implicitHeight
                    Column {
                        id: bubbleColumn
                        width: parent.width
                        spacing: 4
                        Image {
                            visible: bubbleRow.modelData.image !== undefined && bubbleRow.modelData.image !== ""
                            x: bubbleRow.mine ? parent.width - width : 0
                            width: parent.width * 0.6
                            height: visible && status === Image.Ready && implicitWidth > 0 ? width * implicitHeight / implicitWidth : (visible ? 160 : 0)
                            source: visible ? "file://" + bubbleRow.modelData.image : ""
                            sourceSize.width: 600
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                        }
                        Rectangle {
                            visible: bubbleRow.modelData.content !== ""
                            x: bubbleRow.mine ? parent.width - width : 0
                            width: Math.min(body.implicitWidth + 28, parent.width * 0.86)
                            height: body.implicitHeight + 20
                            radius: MobileTheme.radius(18)
                            color: bubbleRow.mine ? MobileTheme.accent : MobileTheme.surface
                            Text {
                                id: body
                                anchors.centerIn: parent
                                width: Math.min(implicitWidth, bubbleRow.width * 0.86 - 28)
                                text: bubbleRow.modelData.content
                                // The model writes Markdown (lists, bold); the user's text is shown as typed.
                                textFormat: bubbleRow.mine ? Text.PlainText : Text.MarkdownText
                                wrapMode: Text.Wrap
                                color: bubbleRow.mine ? MobileTheme.background : MobileTheme.foreground
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 16
                            }
                            TapHandler {
                                longPressThreshold: 0.45
                                onLongPressed: root.menuFor = { message: bubbleRow.modelData }
                            }
                        }
                    }
                }
                footer: Item {
                    width: list.width
                    implicitHeight: root.streaming || root.thinking ? replyBubble.height + 8 : 0
                    Rectangle {
                        id: replyBubble
                        visible: root.streaming !== "" || root.thinking
                        y: 8
                        width: Math.min(replyText.implicitWidth + 28, parent.width * 0.86)
                        height: replyText.implicitHeight + 20
                        radius: MobileTheme.radius(18)
                        color: MobileTheme.surface
                        Text {
                            id: replyText
                            anchors.centerIn: parent
                            width: Math.min(implicitWidth, list.width * 0.86 - 28)
                            text: root.streaming || "…"
                            textFormat: root.streaming ? Text.MarkdownText : Text.PlainText
                            wrapMode: Text.Wrap
                            color: root.streaming ? MobileTheme.foreground : MobileTheme.secondary
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: root.streaming ? 16 : 22
                            SequentialAnimation on opacity {
                                running: root.thinking
                                loops: Animation.Infinite
                                NumberAnimation { from: 1; to: 0.3; duration: 600 }
                                NumberAnimation { from: 0.3; to: 1; duration: 600 }
                            }
                        }
                    }
                }
                Text {
                    anchors.centerIn: parent
                    width: parent.width - 48
                    visible: root.messages.length === 0 && !root.busy
                    text: "Type a question, tap \u{F036C} to speak, or \u{F02E9} to ask about a picture."
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 15
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }
            }
            Text {
                Layout.fillWidth: true
                visible: root.error !== "" || root.notice !== ""
                text: root.error || root.notice
                color: root.error ? MobileTheme.danger : MobileTheme.accent
                font.family: MobileTheme.fontFamily
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }
            // The picture going with the next message.
            Item {
                Layout.fillWidth: true
                visible: root.picture !== "" || root.capturePath !== ""
                implicitHeight: 80
                Rectangle {
                    width: 80; height: 80
                    radius: MobileTheme.radius(12)
                    color: MobileTheme.surface
                    clip: true
                    Image {
                        anchors.fill: parent
                        visible: root.picture !== ""
                        source: root.picture ? "file://" + root.picture : ""
                        sourceSize.width: 160
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                    }
                    Glyph { anchors.centerIn: parent; visible: root.picture === ""; text: "\u{F0100}"; color: MobileTheme.secondary; font.pixelSize: 24 }
                    Rectangle {
                        anchors { top: parent.top; right: parent.right; margins: 3 }
                        width: 24; height: 24; radius: 12
                        color: MobileTheme.surface
                        opacity: 0.9
                        Glyph { anchors.centerIn: parent; text: "×"; font.pixelSize: 16 }
                    }
                    TapHandler { onTapped: { root.picture = ""; root.capturePath = ""; captureWait.running = false; } }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                IconButton {
                    glyph: "\u{F02E9}"
                    ink: MobileTheme.secondary
                    enabled: !root.busy && !root.listening
                    onClicked: root.openPicker()
                }
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: Math.min(Math.max(48, compose.contentHeight + 24), 150)
                    radius: MobileTheme.radius(20)
                    color: MobileTheme.surface
                    border.width: root.composing || root.listening ? 1 : 0
                    border.color: root.listening ? MobileTheme.danger : MobileTheme.accent
                    Flickable {
                        id: composeScroll
                        anchors { fill: parent; leftMargin: 14; rightMargin: 14; topMargin: 12; bottomMargin: 12 }
                        contentHeight: compose.contentHeight
                        clip: true
                        flickableDirection: Flickable.VerticalFlick
                        TextEdit {
                            id: compose
                            width: composeScroll.width
                            wrapMode: TextEdit.Wrap
                            readOnly: !root.composing
                            visible: !root.listening
                            color: MobileTheme.foreground
                            selectionColor: MobileTheme.selection
                            selectedTextColor: MobileTheme.foreground
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 16
                            textFormat: TextEdit.PlainText
                            onCursorRectangleChanged: {
                                if (cursorRectangle.y + cursorRectangle.height > composeScroll.contentY + composeScroll.height)
                                    composeScroll.contentY = cursorRectangle.y + cursorRectangle.height - composeScroll.height;
                            }
                            Text {
                                visible: !compose.text
                                text: root.picture ? "Ask about the picture" : "Ask Wisp"
                                color: MobileTheme.secondary
                                font: compose.font
                            }
                            TapHandler { enabled: root.composing; onTapped: root.keyboard("show") }
                        }
                        Text {
                            width: composeScroll.width
                            visible: root.listening
                            text: root.heardText() || (root.micLive ? "Listening…" : "Starting…")
                            color: root.heardText() ? MobileTheme.foreground : MobileTheme.secondary
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 16
                            wrapMode: Text.Wrap
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: !root.composing && !root.listening
                        onClicked: root.startComposing()
                    }
                }
                // Send what is typed; with nothing typed, speak instead. While
                // listening it stops (and sends what was heard); while
                // answering it stops the reply, and while reading one aloud,
                // the voice.
                IconButton {
                    readonly property bool typed: compose.text.trim().length > 0 || root.picture !== ""
                    readonly property bool stops: root.busy || root.listening || (root.speaking && !typed)
                    glyph: stops ? "\u{F04DB}" : typed ? "\u{F048A}" : "\u{F036C}"
                    tint: root.listening ? MobileTheme.danger : MobileTheme.accent
                    ink: MobileTheme.background
                    onClicked: {
                        if (root.busy) root.stopReply();
                        else if (root.listening) root.stopListening();
                        else if (typed) root.send(compose.text);
                        else if (root.speaking) root.stopSpeaking();
                        else root.startListening();
                    }
                }
            }
        }

        // What Wisp remembers.
        ColumnLayout {
            anchors.fill: parent
            visible: root.view === "memory"
            spacing: 10
            Text {
                Layout.fillWidth: true
                text: "Wisp keeps what you tell it about yourself and uses it in every chat. Say “remember that …”, or add it here."
                color: MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 14
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                TouchTextField {
                    id: memoryField
                    Layout.fillWidth: true
                    placeholderText: "Something to remember"
                    onEditingRequested: { editing = true; forceActiveFocus(); root.keyboard("show"); }
                    onEditingChanged: if (!editing) root.keyboard("hide")
                    onAccepted: addMemory.clicked()
                }
                IconButton {
                    id: addMemory
                    glyph: "\u{F0415}"
                    tint: MobileTheme.accent
                    ink: MobileTheme.background
                    enabled: memoryField.text.trim().length > 2
                    onClicked: {
                        root.run("remember", root.helper(["remember", memoryField.text.trim()]));
                        memoryField.text = "";
                        memoryField.editing = false;
                    }
                }
            }
            ListView {
                id: memoryList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 6
                model: root.memories
                delegate: Rectangle {
                    id: memoryRow
                    required property var modelData
                    width: memoryList.width
                    implicitHeight: memoryText.implicitHeight + 24
                    radius: MobileTheme.radius(14)
                    color: MobileTheme.surface
                    Text {
                        id: memoryText
                        anchors { left: parent.left; right: forget.left; verticalCenter: parent.verticalCenter; leftMargin: 14; rightMargin: 6 }
                        text: memoryRow.modelData.text
                        textFormat: Text.PlainText
                        color: MobileTheme.foreground
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 15
                        wrapMode: Text.WordWrap
                    }
                    IconButton {
                        id: forget
                        anchors { right: parent.right; verticalCenter: parent.verticalCenter; rightMargin: 4 }
                        glyph: "\u{F01B4}"
                        ink: MobileTheme.secondary
                        onClicked: root.run("forget", root.helper(["forget", String(memoryRow.modelData.id)]))
                    }
                }
                Text {
                    anchors.centerIn: parent
                    width: parent.width - 48
                    visible: root.memories.length === 0
                    text: "Nothing yet."
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 15
                    horizontalAlignment: Text.AlignHCenter
                }
            }
        }

        // Settings: the model, the voice, learning.
        Flickable {
            anchors.fill: parent
            visible: root.view === "settings"
            contentHeight: settingsColumn.implicitHeight
            clip: true
            ColumnLayout {
                id: settingsColumn
                width: parent.width
                spacing: 10
                Text {
                    text: "MODEL"
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 12
                    font.letterSpacing: 1.2
                }
                Repeater {
                    model: root.status.models || []
                    delegate: Rectangle {
                        id: modelRow
                        required property var modelData
                        readonly property bool chosen: root.settings.model === modelData.id
                        Layout.fillWidth: true
                        implicitHeight: modelColumn.implicitHeight + 28
                        radius: MobileTheme.radius(16)
                        color: MobileTheme.surface
                        border.width: chosen ? 1 : 0
                        border.color: MobileTheme.accent
                        ColumnLayout {
                            id: modelColumn
                            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: 16 }
                            spacing: 2
                            Text {
                                text: modelRow.modelData.name + (modelRow.chosen ? "  \u{F012C}" : "")
                                color: MobileTheme.foreground
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 16
                            }
                            Text {
                                Layout.fillWidth: true
                                text: modelRow.modelData.detail + " · " + (modelRow.modelData.size / 1e9).toFixed(1) + " GB"
                                      + (modelRow.modelData.installed ? "" : root.installing === modelRow.modelData.id
                                         ? " · downloading " + Math.round(root.progress * 100) + "%" : " · tap to download")
                                color: MobileTheme.secondary
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 13
                                wrapMode: Text.WordWrap
                            }
                        }
                        TapHandler {
                            enabled: root.installing === ""
                            onTapped: {
                                if (modelRow.modelData.installed) {
                                    if (!modelRow.chosen) root.run("settings", root.helper(["set", "model", modelRow.modelData.id]));
                                } else {
                                    root.installing = modelRow.modelData.id;
                                    installer.command = root.helper(["model", "install", modelRow.modelData.id]);
                                    installer.running = true;
                                }
                            }
                        }
                    }
                }
                Text {
                    Layout.topMargin: 8
                    text: "VOICE"
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 12
                    font.letterSpacing: 1.2
                }
                TouchButton {
                    Layout.fillWidth: true
                    visible: root.status.voice !== true
                    label: root.installing === "voice" ? (root.installState || "Installing") + "…"
                                                       : "\u{F057E}  Install the voice (about 460 MB)"
                    onClicked: {
                        if (root.installing !== "") return;
                        root.installing = "voice";
                        installer.command = root.helper(["voice", "install"]);
                        installer.running = true;
                    }
                }
                // Kokoro's voices, all in the one install: tap one to hear it
                // and use it.
                Repeater {
                    model: root.status.voice === true ? root.status.voices || [] : []
                    delegate: Rectangle {
                        id: voiceRow
                        required property var modelData
                        readonly property bool chosen: root.settings.voice === modelData.id
                        Layout.fillWidth: true
                        implicitHeight: 52
                        radius: MobileTheme.radius(16)
                        color: voiceTap.pressed ? MobileTheme.selection : MobileTheme.surface
                        border.width: chosen ? 1 : 0
                        border.color: MobileTheme.accent
                        Text {
                            anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
                            text: voiceRow.modelData.name + (voiceRow.chosen ? "  \u{F012C}" : "")
                            color: MobileTheme.foreground
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 16
                        }
                        TapHandler {
                            id: voiceTap
                            onTapped: root.pickVoice(voiceRow.modelData.id)
                        }
                    }
                }
                Toggle {
                    label: "Speak replies"
                    detail: root.status.voice === true ? "Read every reply aloud. Replies to a spoken question are always read aloud."
                                                       : "Download a voice first."
                    enabled: root.status.voice === true
                    checked: root.settings.speak === true
                    onToggled: root.run("settings", root.helper(["set", "speak", checked ? "false" : "true"]))
                }
                Text {
                    Layout.topMargin: 8
                    text: "MEMORY"
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 12
                    font.letterSpacing: 1.2
                }
                Toggle {
                    label: "Learn from chats"
                    detail: "After each message, Wisp notes lasting facts you mention about yourself. They are listed in Memory."
                    checked: root.settings.auto_memory === true
                    onToggled: root.run("settings", root.helper(["set", "auto_memory", checked ? "false" : "true"]))
                }
                Text {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    text: "Wisp runs on this phone with llama.cpp and keeps chats, memories and pictures here. Nothing is sent anywhere."
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 13
                    wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    visible: root.error !== ""
                    text: root.error
                    color: MobileTheme.danger
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 13
                    wrapMode: Text.WordWrap
                }
            }
        }

        // A picture to ask about: the camera, or one of the newest photos.
        MenuOverlay {
            open: root.pickerOpen
            onDismissed: root.pickerOpen = false
            ColumnLayout {
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 8 }
                spacing: 8
                TouchButton { Layout.fillWidth: true; label: "\u{F0100}  Take a photo"; onClicked: root.takePhoto() }
                GridView {
                    id: photoGrid
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(Math.ceil(count / 3) * cellHeight, window.height * 0.5)
                    visible: count > 0
                    clip: true
                    cellWidth: width / 3
                    cellHeight: cellWidth
                    boundsBehavior: Flickable.StopAtBounds
                    model: root.recentPhotos
                    delegate: Item {
                        required property string modelData
                        width: photoGrid.cellWidth
                        height: photoGrid.cellHeight
                        Image {
                            anchors.fill: parent
                            anchors.margins: 3
                            source: "file://" + parent.modelData
                            sourceSize.width: 240
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                        }
                        TapHandler { onTapped: { root.picture = parent.modelData; root.pickerOpen = false; } }
                    }
                }
                TouchButton { Layout.fillWidth: true; label: "Cancel"; onClicked: root.pickerOpen = false }
            }
        }
        // Long-press: a chat (delete) or a message (copy, read aloud).
        MenuOverlay {
            open: root.menuFor !== null
            onDismissed: root.menuFor = null
            ColumnLayout {
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 8 }
                spacing: 8
                TouchButton {
                    Layout.fillWidth: true
                    visible: root.menuFor !== null && root.menuFor.message !== undefined
                    label: "\u{F018F}  Copy"
                    onClicked: { root.copy(root.menuFor.message.content); root.menuFor = null; }
                }
                TouchButton {
                    Layout.fillWidth: true
                    visible: root.menuFor !== null && root.menuFor.message !== undefined && root.status.voice === true
                    label: "\u{F057E}  Read aloud"
                    onClicked: { root.speak(root.menuFor.message.content); root.menuFor = null; }
                }
                TouchButton {
                    Layout.fillWidth: true
                    visible: root.menuFor !== null && root.menuFor.chat !== undefined
                    label: "\u{F01B4}  Delete this chat"
                    onClicked: { root.run("delete", root.helper(["delete", String(root.menuFor.chat.id)])); root.menuFor = null; }
                }
                TouchButton { Layout.fillWidth: true; label: "Cancel"; onClicked: root.menuFor = null }
            }
        }
        // The microphone, asked for once.
        MenuOverlay {
            open: root.askMic
            onDismissed: root.askMic = false
            ColumnLayout {
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 8 }
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    text: "Let Wisp use the microphone? What you say is turned into text on the phone; the sound is not kept."
                    color: MobileTheme.foreground
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 16
                    wrapMode: Text.WordWrap
                }
                TouchButton {
                    Layout.fillWidth: true
                    label: "Allow"
                    onClicked: { root.askMic = false; root.run("grant", [root.bin("omarchy-mobile-app"), "grant", "wisp", "microphone"]); }
                }
                TouchButton { Layout.fillWidth: true; label: "Not now"; onClicked: root.askMic = false }
            }
        }
    }
}
