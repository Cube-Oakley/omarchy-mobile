import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import OmarchyMobile

// Phone: keypad, recent calls and the call screen. Calls go through
// omarchy-mobile-phone (ModemManager). Every colour comes from MobileTheme,
// so the app follows the current Omarchy theme, including its red and green.
ShellRoot {
    id: root
    property bool ready: false
    property bool allowed: false
    property var requests: []
    property var modem: ({})
    property var calls: []
    property var history: []
    property var audio: ({})
    property string number: ""
    property string tab: "keypad"      // keypad, recents, contacts
    property var people: []
    property string peopleQuery: ""
    property var choosing: null        // a contact with several numbers
    property var recentMenu: null      // a recent call, long-pressed
    readonly property var shownPeople: {
        const q = peopleQuery.trim().toLowerCase();
        const digits = q.replace(/[^0-9]/g, "");
        return people.filter(p => (p.phones || []).length && (!q || (p.name || "").toLowerCase().indexOf(q) >= 0
            || (digits.length >= 2 && (p.phones || []).some(n => (n.value || "").replace(/[^0-9]/g, "").indexOf(digits) >= 0))));
    }
    property bool tones: false
    property string sent: ""
    property string message: ""
    property string pendingDial: ""
    property var ended: null
    property real now: Date.now() / 1000
    property var queue: []
    readonly property var call: pickCall(calls)
    readonly property var shown: call || (pendingDial ? { id: "", number: pendingDial, direction: "outgoing", state: "dialing", since: now, answered: 0 } : ended)
    readonly property bool inCall: shown !== null && shown !== undefined

    function bin(name) { return Quickshell.env("HOME") + "/.local/bin/" + name; }
    function phone(args) { return [bin("omarchy-mobile-phone")].concat(args); }
    function run(kind, args) {
        if (action.running) { queue = queue.concat([[kind, args]]); return; }
        action.kind = kind;
        action.command = args;
        action.running = true;
    }
    function next() {
        if (!queue.length || action.running) return;
        const item = queue[0];
        queue = queue.slice(1);
        run(item[0], item[1]);
    }
    function refresh() {
        if (!status.running) status.running = true;
    }
    function pickCall(list) {
        const order = ["ringing-in", "waiting", "active", "dialing", "ringing-out", "held", "unknown"];
        for (let i = 0; i < order.length; i++) {
            const found = (list || []).find(c => c.state === order[i]);
            if (found) return found;
        }
        return (list || []).length ? list[0] : null;
    }
    function take(line) {
        let data = null;
        try { data = JSON.parse(line); } catch (e) { return; }
        const had = root.call;
        root.calls = data.calls || [];
        root.audio = data.audio || {};
        if (root.call) { root.pendingDial = ""; root.ended = null; }
        else if (had && !root.pendingDial) { root.ended = Object.assign({}, had, { state: "terminated" }); endedHold.restart(); }
        if (!root.call) { root.tones = false; root.sent = ""; }
        if (data.ended) root.refresh();
        if (data.ok === false && data.error) root.message = data.error;
    }
    function digits(n) { return String(n || "").replace(/[^0-9]/g, ""); }
    // The helper formats numbers (display); a number being dialled has none yet.
    function shownNumber(c) { return c ? (c.display || typing(String(c.number || "")) || "Unknown number") : ""; }
    function typing(n) {
        if (!/^[0-9]+$/.test(n)) return n;
        if (n.length > 11 || (n.length === 11 && n[0] !== "1")) return n;
        const lead = n.length === 11 ? "1 " : "";
        const t = n.length === 11 ? n.slice(1) : n;
        if (t.length <= 3) return lead + t;
        if (t.length <= 7 && !lead) return t.slice(0, 3) + "-" + t.slice(3);
        return lead + "(" + t.slice(0, 3) + ") " + t.slice(3, 6) + (t.length > 6 ? "-" + t.slice(6) : "");
    }
    function clock(seconds) {
        const s = Math.max(0, Math.floor(seconds));
        const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60), r = s % 60;
        const two = v => (v < 10 ? "0" : "") + v;
        return (h ? h + ":" + two(m) : m) + ":" + two(r);
    }
    function stateText(c) {
        if (!c) return "";
        if (c.state === "ringing-in") return "Incoming call";
        if (c.state === "waiting") return "Call waiting";
        if (c.state === "dialing" || c.state === "unknown") return "Calling…";
        if (c.state === "ringing-out") return "Ringing…";
        if (c.state === "held") return "On hold";
        if (c.state === "terminated") return "Call ended";
        if (c.state === "active") return clock(now - (c.answered || c.since || now));
        return c.state;
    }
    function ago(epoch) {
        const then = new Date(epoch * 1000), today = new Date();
        const minutes = Math.floor((Date.now() / 1000 - epoch) / 60);
        if (minutes < 1) return "Now";
        if (minutes < 60) return minutes + " min";
        if (then.toDateString() === today.toDateString())
            return then.toLocaleTimeString(Qt.locale(), "h:mm ap");
        const yesterday = new Date(today.getTime() - 86400000);
        if (then.toDateString() === yesterday.toDateString()) return "Yesterday";
        if (Date.now() / 1000 - epoch < 6 * 86400) return then.toLocaleDateString(Qt.locale(), "ddd");
        return then.toLocaleDateString(Qt.locale(), "d MMM");
    }
    function recentLine(item) {
        if (item.result === "missed") return "Missed";
        if (item.result === "declined") return "Declined";
        if (item.result === "failed") return "Didn't connect";
        if (item.result === "cancelled") return "Cancelled";
        const way = item.direction === "incoming" ? "Incoming" : "Outgoing";
        return way + " · " + clock(item.duration || 0);
    }
    function recentGlyph(item) {
        return item.result === "missed" || item.result === "declined" ? "\u{F03FA}"
             : item.direction === "incoming" ? "\u{F03F7}" : "\u{F03FB}";
    }
    function press(key) {
        message = "";
        if (call && tones) {
            sent = (sent + key).slice(-24);
            run("dtmf", phone(["dtmf", call.id, key]));
            return;
        }
        if (number.length < 32) number += key;
    }
    function dial() {
        message = "";
        if (!number) {
            const last = history.find(item => item.direction === "outgoing" && item.number);
            if (last) number = last.number;
            return;
        }
        if (modem.present === false) { message = "No modem is available"; return; }
        pendingDial = number;
        run("dial", phone(["dial", number]));
        number = "";
    }
    function callBack(n) {
        if (!n) return;
        number = digits(n).length ? String(n) : "";
        tab = "keypad";
        dial();
    }
    function hangup() {
        if (call) run("hangup", phone(["hangup", call.id]));
        else if (pendingDial) run("hangup", phone(["hangup-all"]));
    }
    function keyboard(action) {
        keyboardTool.command = [bin("omarchy-mobile-keyboard"), action];
        keyboardTool.running = true;
    }
    function toggleAudio(what) {
        const on = what === "mute" ? audio.mute : audio.speaker;
        run("audio", phone(["audio", what, on ? "off" : "on"]));
    }
    function signalBars(percent) {
        const bars = ["▁", "▃", "▅", "▇"];
        const lit = percent >= 75 ? 4 : percent >= 50 ? 3 : percent >= 25 ? 2 : percent > 0 ? 1 : 0;
        return { lit: bars.slice(0, lit).join(""), dim: bars.slice(lit).join("") };
    }

    Component.onCompleted: run("grants", [bin("omarchy-mobile-app"), "status", "phone"])
    // Other apps open Phone with a number to call ("tel:..."); it is filled
    // in, not dialled.
    function begin() {
        const arg = Quickshell.env("OMARCHY_MOBILE_APP_ARG") || "";
        if (arg.startsWith("tel:")) number = arg.slice(4).replace(/[^0-9+*#]/g, "").slice(0, 32);
        refresh();
        run("people", [bin("omarchy-mobile-contacts"), "list"]);
    }
    function callPerson(person) {
        const phones = person.phones || [];
        if (phones.length === 1) callBack(phones[0].value);
        else if (phones.length > 1) choosing = person;
    }

    Process {
        id: action
        property string kind: ""
        stdout: StdioCollector {}
        stderr: StdioCollector {}
        onExited: {
            let result = {};
            try { result = JSON.parse(stdout.text); }
            catch (e) {
                const err = (stderr.text || "").trim().split("\n").pop();
                result = { ok: false, error: err || "The phone helper did not answer" };
            }
            if (action.kind === "grants") {
                const items = result.permissions || [];
                let granted = items.length > 0;
                for (let i = 0; i < items.length; i++) if (!items[i].granted) granted = false;
                root.requests = items;
                root.allowed = granted;
                root.ready = result.ok !== false;
                if (result.ok === false) root.message = result.error || "Phone is not installed";
                if (granted) root.begin();
            } else if (result.ok === false) {
                root.message = result.error || "That didn't work";
                if (action.kind === "dial") root.pendingDial = "";
            } else if (action.kind === "grant") {
                if ((result.permissions || []).every(p => p.granted)) {
                    root.allowed = true;
                    root.begin();
                }
            } else if (action.kind === "people") {
                root.people = result.contacts || [];
            } else if (action.kind === "audio") {
                root.audio = result.audio || root.audio;
            } else if (action.kind === "history") {
                root.history = [];
            }
            Qt.callLater(root.next);
        }
    }
    Process {
        id: status
        command: root.phone(["status"])
        stdout: StdioCollector {}
        onExited: {
            try {
                const result = JSON.parse(stdout.text);
                if (result.ok === false) { root.message = result.error || ""; return; }
                root.modem = result.modem || {};
                root.history = result.history || [];
                if (!watcher.running) { root.calls = result.calls || []; root.audio = result.audio || {}; }
            } catch (e) {}
        }
    }
    Process {
        id: watcher
        command: root.phone(["watch"])
        running: root.allowed
        stdout: SplitParser { onRead: line => root.take(line) }
        onExited: if (root.allowed) watchRetry.start()
    }
    Timer { id: watchRetry; interval: 3000; onTriggered: watcher.running = true }
    Process { id: keyboardTool }
    Timer { id: endedHold; interval: 1800; onTriggered: root.ended = null }
    Timer {
        interval: 1000; repeat: true; running: root.inCall
        onTriggered: root.now = Date.now() / 1000
    }
    // Operator and signal change slowly; the calls themselves are live.
    Timer {
        interval: 20000; repeat: true; running: root.allowed && !root.inCall
        onTriggered: { root.now = Date.now() / 1000; root.refresh(); }
    }

    component Glyph: Text {
        color: MobileTheme.foreground
        font.family: MobileTheme.fontFamily
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    // A round-or-square action per the theme's corner setting.
    component RoundAction: Rectangle {
        id: act
        property string glyph: ""
        property string label: ""
        property color tint: MobileTheme.surface
        property color ink: MobileTheme.foreground
        property bool on: false
        property int size: 72
        signal clicked()
        implicitWidth: size
        implicitHeight: size + (label ? 26 : 0)
        color: "transparent"
        Rectangle {
            id: face
            width: act.size; height: act.size
            anchors.horizontalCenter: parent.horizontalCenter
            radius: MobileTheme.radius(act.size / 2)
            color: actTap.pressed ? MobileTheme.muted : act.on ? MobileTheme.accent : act.tint
            Glyph {
                anchors.centerIn: parent
                text: act.glyph
                color: act.on ? MobileTheme.background : act.ink
                font.pixelSize: act.size * 0.42
            }
        }
        Text {
            visible: act.label.length > 0
            anchors.top: face.bottom
            anchors.topMargin: 6
            anchors.horizontalCenter: parent.horizontalCenter
            text: act.label
            color: MobileTheme.secondary
            font.family: MobileTheme.fontFamily
            font.pixelSize: 12
        }
        TapHandler { id: actTap; onTapped: act.clicked() }
    }
    component Key: Rectangle {
        id: key
        property string digit: ""
        property string letters: ""
        signal pressed(string value)
        signal held(string value)
        implicitHeight: 68
        radius: MobileTheme.radius(14)
        color: keyTap.pressed ? MobileTheme.selection : MobileTheme.surface
        Column {
            anchors.centerIn: parent
            spacing: 0
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: key.digit
                color: MobileTheme.foreground
                font.family: MobileTheme.fontFamily
                font.pixelSize: 28
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: key.letters
                color: MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 10
                font.letterSpacing: 1.6
                height: key.letters ? implicitHeight : 0
            }
        }
        TapHandler {
            id: keyTap
            longPressThreshold: 0.6
            onTapped: key.pressed(key.digit)
            onLongPressed: key.held(key.digit)
        }
    }
    component Keypad: GridLayout {
        id: pad
        signal pressed(string value)
        signal held(string value)
        columns: 3
        rowSpacing: 10
        columnSpacing: 10
        Repeater {
            model: [["1", ""], ["2", "ABC"], ["3", "DEF"], ["4", "GHI"], ["5", "JKL"], ["6", "MNO"],
                    ["7", "PQRS"], ["8", "TUV"], ["9", "WXYZ"], ["*", ""], ["0", "+"], ["#", ""]]
            delegate: Key {
                required property var modelData
                Layout.fillWidth: true
                digit: modelData[0]
                letters: modelData[1]
                onPressed: value => pad.pressed(value)
                onHeld: value => pad.held(value)
            }
        }
    }

    AppWindow {
        compact: true
        pageMargin: 16
        kicker: ""
        appTitle: "Phone"
        heading: root.inCall ? "" : root.tab === "recents" ? "Recents" : root.tab === "contacts" ? "Contacts" : "Phone"
        backVisible: root.allowed && !root.inCall && root.tab !== "keypad"
        onBackClicked: {
            if (root.choosing || root.recentMenu) { root.choosing = null; root.recentMenu = null; }
            else if (root.tones) root.tones = false;
            else if (root.tab !== "keypad") { root.tab = "keypad"; root.keyboard("hide"); }
            else Qt.quit();
        }

        GrantPage {
            anchors.fill: parent
            visible: root.ready && !root.allowed
            requests: root.requests
            onAllowed: {
                for (let i = 0; i < root.requests.length; i++)
                    root.run("grant", [root.bin("omarchy-mobile-app"), "grant", "phone", root.requests[i].id]);
            }
            onDenied: Qt.quit()
        }

        // The call screen: incoming, calling, connected or just ended.
        ColumnLayout {
            anchors.fill: parent
            visible: root.allowed && root.inCall
            spacing: 10
            Text {
                Layout.fillWidth: true
                text: [root.modem.operator, root.modem.tech ? root.modem.tech.toUpperCase() : ""].filter(s => s).join(" · ")
                color: MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 13
                font.letterSpacing: 1.2
                horizontalAlignment: Text.AlignHCenter
            }
            Item { Layout.preferredHeight: root.tones ? 0 : 24 }
            Rectangle {
                visible: !root.tones
                Layout.alignment: Qt.AlignHCenter
                implicitWidth: 120; implicitHeight: 120
                radius: MobileTheme.radius(60)
                color: "transparent"
                border.width: 2
                border.color: root.shown && root.shown.state === "ringing-in" ? MobileTheme.success : MobileTheme.accent
                Avatar {
                    anchors.centerIn: parent
                    size: 108
                    photo: root.shown ? (root.shown.photo || "") : ""
                    initials: root.shown ? (root.shown.initials || "") : ""
                }
            }
            Text {
                Layout.fillWidth: true
                Layout.topMargin: 12
                text: root.shown && root.shown.name ? root.shown.name : root.shownNumber(root.shown)
                textFormat: Text.PlainText
                color: MobileTheme.foreground
                font.family: MobileTheme.fontFamily
                font.pixelSize: 30
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                fontSizeMode: Text.HorizontalFit
                minimumPixelSize: 18
            }
            Text {
                Layout.fillWidth: true
                visible: !!(root.shown && root.shown.name)
                text: root.shownNumber(root.shown)
                textFormat: Text.PlainText
                color: MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 15
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                Layout.fillWidth: true
                text: root.stateText(root.shown)
                color: root.shown && root.shown.state === "terminated" ? MobileTheme.danger : MobileTheme.accent
                font.family: MobileTheme.fontFamily
                font.pixelSize: 18
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                Layout.fillWidth: true
                visible: root.call !== null && root.call.state !== "ringing-in" && (root.audio.available === false || !!root.audio.error)
                text: root.audio.error || "Call audio isn't connected on this phone yet"
                color: MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 12
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                Layout.fillWidth: true
                visible: root.tones
                text: root.sent || " "
                color: MobileTheme.foreground
                font.family: MobileTheme.fontFamily
                font.pixelSize: 26
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideLeft
            }
            Item { Layout.fillHeight: true }
            Keypad {
                visible: root.tones
                Layout.fillWidth: true
                onPressed: value => root.press(value)
                onHeld: value => root.press(value)
            }
            RowLayout {
                visible: root.call !== null && root.call.state !== "ringing-in" && root.call.state !== "waiting"
                Layout.fillWidth: true
                Layout.bottomMargin: 18
                spacing: 0
                Item { Layout.fillWidth: true }
                RoundAction {
                    glyph: root.audio.mute ? "\u{F036D}" : "\u{F036C}"
                    label: "Mute"
                    on: root.audio.mute === true
                    opacity: root.audio.running ? 1 : 0.45
                    onClicked: if (root.audio.running) root.toggleAudio("mute")
                }
                Item { Layout.fillWidth: true }
                RoundAction {
                    glyph: "\u{F061C}"
                    label: "Keypad"
                    on: root.tones
                    onClicked: root.tones = !root.tones
                }
                Item { Layout.fillWidth: true }
                RoundAction {
                    glyph: "\u{F057E}"
                    label: "Speaker"
                    on: root.audio.speaker === true
                    opacity: root.audio.running && root.audio.speaker_available !== false ? 1 : 0.45
                    onClicked: if (root.audio.running && root.audio.speaker_available !== false) root.toggleAudio("speaker")
                }
                Item { Layout.fillWidth: true }
            }
            // Incoming: the theme's red declines, its green answers.
            RowLayout {
                visible: root.call !== null && (root.call.state === "ringing-in" || root.call.state === "waiting")
                Layout.fillWidth: true
                Layout.bottomMargin: 24
                Item { Layout.fillWidth: true }
                RoundAction {
                    size: 80
                    glyph: "\u{F03F5}"
                    label: "Decline"
                    tint: MobileTheme.danger
                    ink: MobileTheme.background
                    onClicked: root.hangup()
                }
                Item { Layout.fillWidth: true }
                RoundAction {
                    size: 80
                    glyph: "\u{F03F2}"
                    label: "Answer"
                    tint: MobileTheme.success
                    ink: MobileTheme.background
                    onClicked: root.run("accept", root.phone(["accept", root.call.id]))
                }
                Item { Layout.fillWidth: true }
            }
            RoundAction {
                visible: root.call === null ? root.pendingDial !== "" : root.call.state !== "ringing-in" && root.call.state !== "waiting"
                Layout.alignment: Qt.AlignHCenter
                Layout.bottomMargin: 12
                size: 80
                glyph: "\u{F03F5}"
                tint: MobileTheme.danger
                ink: MobileTheme.background
                onClicked: root.hangup()
            }
            Text {
                Layout.fillWidth: true
                visible: root.message.length > 0
                text: root.message
                color: MobileTheme.danger
                font.family: MobileTheme.fontFamily
                font.pixelSize: 13
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }
        }

        // Keypad and recent calls.
        ColumnLayout {
            anchors.fill: parent
            visible: root.allowed && !root.inCall
            spacing: 10
            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: root.modem.present === false ? "No modem"
                        : [root.modem.operator || "Searching", root.modem.tech ? root.modem.tech.toUpperCase() : ""].filter(s => s).join(" · ")
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }
                Text {
                    readonly property var bars: root.signalBars(root.modem.signal || 0)
                    textFormat: Text.StyledText
                    text: '<font color="' + MobileTheme.accent + '">' + bars.lit + '</font><font color="' + MobileTheme.muted + '">' + bars.dim + '</font>'
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 15
                }
            }

            // Keypad tab.
            ColumnLayout {
                visible: root.tab === "keypad"
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 10
                Item { Layout.fillHeight: true }
                Text {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 56
                    text: root.number ? root.typing(root.number) : ""
                    textFormat: Text.PlainText
                    color: MobileTheme.foreground
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 36
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: 18
                }
                Text {
                    Layout.fillWidth: true
                    visible: root.message.length > 0
                    text: root.message
                    color: MobileTheme.danger
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 13
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                }
                Keypad {
                    Layout.fillWidth: true
                    onPressed: value => root.press(value)
                    onHeld: value => root.press(value === "0" ? "+" : value)
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    Item { Layout.fillWidth: true; Layout.preferredWidth: 1 }
                    RoundAction {
                        size: 76
                        glyph: "\u{F03F2}"
                        tint: MobileTheme.success
                        ink: MobileTheme.background
                        onClicked: root.dial()
                    }
                    Item {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        implicitHeight: 76
                        Glyph {
                            anchors.centerIn: parent
                            visible: root.number.length > 0
                            text: "\u{F0B5C}"
                            font.pixelSize: 30
                            color: backTap.pressed ? MobileTheme.accent : MobileTheme.secondary
                            TapHandler {
                                id: backTap
                                margin: 18
                                onTapped: root.number = root.number.slice(0, -1)
                                onLongPressed: root.number = ""
                            }
                        }
                    }
                }
            }

            // Recents tab.
            ListView {
                id: recentList
                visible: root.tab === "recents"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 6
                model: root.history
                footer: Item {
                    width: recentList.width
                    height: root.history.length ? 72 : 0
                    TouchButton {
                        visible: root.history.length > 0
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        label: clearArmed.running ? "Tap again to clear" : "Clear recents"
                        onClicked: {
                            if (clearArmed.running) { clearArmed.stop(); root.run("history", root.phone(["history", "clear"])); }
                            else clearArmed.start();
                        }
                        Timer { id: clearArmed; interval: 3000 }
                    }
                }
                delegate: Rectangle {
                    required property var modelData
                    width: recentList.width
                    implicitHeight: 64
                    radius: MobileTheme.radius(14)
                    color: rowTap.pressed ? MobileTheme.selection : MobileTheme.surface
                    readonly property bool missed: modelData.result === "missed"
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 6
                        spacing: 12
                        Avatar {
                            size: 42
                            photo: modelData.photo || ""
                            initials: modelData.initials || ""
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                Layout.fillWidth: true
                                text: modelData.name || modelData.display || modelData.number || "Unknown number"
                                textFormat: Text.PlainText
                                color: missed ? MobileTheme.danger : MobileTheme.foreground
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 16
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: root.recentGlyph(modelData) + " " + root.recentLine(modelData) + " · " + root.ago(modelData.start)
                                color: MobileTheme.secondary
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }
                        }
                        RoundAction {
                            size: 48
                            glyph: "\u{F03F2}"
                            tint: "transparent"
                            ink: MobileTheme.success
                            onClicked: root.callBack(modelData.number)
                        }
                    }
                    TapHandler {
                        id: rowTap
                        longPressThreshold: 0.5
                        onTapped: { root.number = modelData.number || ""; root.tab = "keypad"; }
                        onLongPressed: root.recentMenu = modelData
                    }
                }
                Text {
                    anchors.centerIn: parent
                    visible: root.history.length === 0
                    text: "No recent calls"
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 15
                }
            }

            // Contacts tab: people with a number, one tap to call.
            ColumnLayout {
                visible: root.tab === "contacts"
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 8
                TouchTextField {
                    Layout.fillWidth: true
                    placeholderText: "\u{F0349}  Search contacts"
                    onTextChanged: root.peopleQuery = text
                    onEditingRequested: { editing = true; forceActiveFocus(); root.keyboard("show"); }
                    onEditingChanged: if (!editing) root.keyboard("hide")
                }
                ListView {
                    id: peopleList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 4
                    model: root.shownPeople
                    delegate: Rectangle {
                        id: personRow
                        required property var modelData
                        width: peopleList.width
                        implicitHeight: 62
                        radius: MobileTheme.radius(14)
                        color: personTap.pressed ? MobileTheme.selection : "transparent"
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 6
                            spacing: 12
                            Avatar { size: 44; photo: personRow.modelData.photo || ""; initials: personRow.modelData.initials || "" }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    Layout.fillWidth: true
                                    text: (personRow.modelData.favorite ? "\u{F04CE} " : "") + personRow.modelData.name
                                    textFormat: Text.PlainText
                                    color: MobileTheme.foreground
                                    font.family: MobileTheme.fontFamily
                                    font.pixelSize: 16
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    readonly property var phones: personRow.modelData.phones || []
                                    text: phones.length ? phones[0].value + (phones.length > 1 ? "  +" + (phones.length - 1) : "") : ""
                                    textFormat: Text.PlainText
                                    color: MobileTheme.secondary
                                    font.family: MobileTheme.fontFamily
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                }
                            }
                            RoundAction {
                                size: 44
                                glyph: "\u{F03F2}"
                                tint: "transparent"
                                ink: MobileTheme.success
                                onClicked: root.callPerson(personRow.modelData)
                            }
                        }
                        TapHandler { id: personTap; onTapped: root.callPerson(personRow.modelData) }
                    }
                    Column {
                        anchors.centerIn: parent
                        visible: root.shownPeople.length === 0
                        spacing: 12
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: root.people.length ? "No matching contacts" : "No contacts yet"
                            color: MobileTheme.secondary
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 15
                        }
                        TouchButton {
                            anchors.horizontalCenter: parent.horizontalCenter
                            implicitWidth: 220
                            label: "Open Contacts"
                            onClicked: Quickshell.execDetached([root.bin("omarchy-mobile-app"), "launch", "contacts"])
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                TouchButton {
                    Layout.fillWidth: true
                    label: "\u{F061C}  Keypad"
                    selected: root.tab === "keypad"
                    onClicked: root.tab = "keypad"
                }
                TouchButton {
                    Layout.fillWidth: true
                    readonly property int missed: root.history.filter(item => item.result === "missed" && Date.now() / 1000 - item.start < 86400).length
                    label: "\u{F02DA}  Recents" + (missed ? "  " + missed : "")
                    selected: root.tab === "recents"
                    onClicked: { root.tab = "recents"; root.refresh(); }
                }
                TouchButton {
                    Layout.fillWidth: true
                    label: "\u{F000E}  Contacts"
                    selected: root.tab === "contacts"
                    onClicked: { root.tab = "contacts"; root.run("people", [root.bin("omarchy-mobile-contacts"), "list"]); }
                }
            }
        }

        MenuOverlay {
            open: root.choosing !== null
            onDismissed: root.choosing = null
            ColumnLayout {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 8
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    text: root.choosing ? "Call " + root.choosing.name : ""
                    textFormat: Text.PlainText
                    color: MobileTheme.foreground
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 18
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                }
                Repeater {
                    model: root.choosing ? root.choosing.phones : []
                    delegate: TouchButton {
                        required property var modelData
                        Layout.fillWidth: true
                        label: "\u{F03F2}  " + modelData.value + (modelData.type ? "  ·  " + modelData.type : "")
                        onClicked: { const n = modelData.value; root.choosing = null; root.callBack(n); }
                    }
                }
                TouchButton { Layout.fillWidth: true; label: "Cancel"; onClicked: root.choosing = null }
            }
        }

        MenuOverlay {
            open: root.recentMenu !== null
            onDismissed: root.recentMenu = null
            ColumnLayout {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 8
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    text: root.recentMenu ? (root.recentMenu.name || root.recentMenu.display || root.recentMenu.number) : ""
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
                    label: "\u{F03F2}  Call"
                    onClicked: { const n = root.recentMenu.number; root.recentMenu = null; root.callBack(n); }
                }
                TouchButton {
                    Layout.fillWidth: true
                    label: "\u{F0369}  Send a text"
                    onClicked: { Quickshell.execDetached([root.bin("omarchy-mobile-app"), "launch", "messages", "sms:" + root.recentMenu.number]); root.recentMenu = null; }
                }
                TouchButton {
                    Layout.fillWidth: true
                    label: root.recentMenu && root.recentMenu.contact ? "\u{F0004}  View contact" : "\u{F0014}  Add to contacts"
                    onClicked: {
                        const item = root.recentMenu;
                        Quickshell.execDetached([root.bin("omarchy-mobile-app"), "launch", "contacts",
                                                 item.contact ? "uid:" + item.contact : "new:" + item.number]);
                        root.recentMenu = null;
                    }
                }
                TouchButton { Layout.fillWidth: true; label: "Cancel"; onClicked: root.recentMenu = null }
            }
        }
    }
}
