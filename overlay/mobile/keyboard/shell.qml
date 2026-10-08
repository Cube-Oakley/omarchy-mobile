// The on-screen keyboard: a Quickshell window in the theme's colours, with
// Gboard's layout (long-press hints on the letters, a ?123 page, a second
// page with Esc, Tab, Ctrl and arrows) and swipe typing. Keys and words go
// out through omarchy-mobile-vkbd (keyboard/vkbd.c), which also decodes swipes.
//
// keyboard.sh writes "show", "hide" or "toggle" (and a counter) to
// $XDG_RUNTIME_DIR/omarchy-mobile-keyboard.cmd; this window watches it.
// Characters are typed when the finger lifts, so a press can still become a
// swipe or a long press; each finger is followed on its own, for typing
// with two thumbs. The microphone at the strip's right end is voice typing
// (omarchy-mobile-dictate, on the phone): the strip shows the words as they
// are said, and each phrase is typed when it ends.
import QtQuick
import QtQuick.Shapes
import Quickshell
import Quickshell.Io
import Quickshell.Wayland
import OmarchyMobile

ShellRoot {
    id: root
    property bool shown: false
    property string page: "letters"     // letters | symbols | more
    property int shift: 0               // 0 off, 1 the next letter, 2 locked
    property bool ctrl: false           // one-shot, from the "more" page
    property var suggestions: []
    // The word the last swipe typed, with the space before it if one was
    // added: a suggestion or backspace replaces it while nothing else came.
    property string swiped: ""
    property bool spaceBeforeSwipe: false
    property string lastTyped: ""
    // Learning: the letters of the word being tapped, and a swiped word,
    // sent to vkbd ("learn") once the user moves on, not if they took it back.
    property string tappedWord: ""
    property string pendingLearn: ""
    // Voice typing: listening, the phrase being said, and a message.
    property bool dictating: false
    // The microphone hears (the helper's "listening"): until then, about
    // half a second after the tap, what is said is not heard.
    property bool micLive: false
    property string spoken: ""
    property string dictationMessage: ""
    readonly property real micW: 56
    readonly property string runtime: Quickshell.env("XDG_RUNTIME_DIR") || "/tmp"
    readonly property string home: Quickshell.env("HOME")
    readonly property int keysHeight: {
        const h = MobileTheme.state.device ? MobileTheme.state.device.keyboardHeight : undefined;
        return typeof h === "number" && h >= 160 && h <= 500 ? h : 280;
    }
    readonly property int stripHeight: 44
    // The navigation strip (the shell's, 24 px) lies over the bottom: keys
    // end above it, so none is half under the home gesture.
    readonly property int navGap: 24
    // Over an Android app the keyboard reserves no space: Android's input
    // method there (waydroid/keyboard-bridge) takes the same height inside
    // Android, so the app lays itself out above the keys as on any Android
    // phone, and its window never changes size (a size change is a display
    // hotplug for Waydroid, and a shrunk Android task stays shrunk).
    // The bridge's own window never takes the focus (hypr-mobile.lua).
    readonly property bool overAndroid: String((ToplevelManager.activeToplevel
        && ToplevelManager.activeToplevel.appId) || "").startsWith("waydroid.")

    // ---- layouts: label, text typed, long-press hint, more alternatives,
    // width in keys, and an action for keys that type nothing.
    function row(chars, hints) {
        return chars.split("").map((c, i) => ({ label: c, out: c, hint: hints ? hints[i] : "" }));
    }
    readonly property var layouts: ({
        letters: [
            row("qwertyuiop", "1234567890"),
            row("asdfghjkl", "@#$_&-+()"),
            [{ label: "⇧", action: "shift", w: 1.5 }].concat(row("zxcvbnm", "*\"':;!?"))
                .concat([{ label: "⌫", action: "back", w: 1.5 }]),
            [{ label: "?123", action: "symbols", w: 1.5 },
             { label: ",", out: ",", alts: [",", ";", ":", "'", "\""] },
             { label: "", action: "space", w: 5 },
             { label: ".", out: ".", alts: [".", "!", "?", "-", "/", "@", "#", "&"] },
             { label: "⏎", action: "enter", w: 1.5 }]
        ],
        symbols: [
            row("1234567890"),
            row("@#$_&-+()/"),
            [{ label: "=\\<", action: "more", w: 1.5 }].concat(row("*\"':;!?"))
                .concat([{ label: "⌫", action: "back", w: 1.5 }]),
            [{ label: "ABC", action: "letters", w: 1.5 }, { label: ",", out: "," },
             { label: "", action: "space", w: 5 }, { label: ".", out: "." },
             { label: "⏎", action: "enter", w: 1.5 }]
        ],
        more: [
            row("~`|\\{}[]<>"),
            [{ label: "esc", action: "esc" }, { label: "⇥", action: "tab" }, { label: "ctrl", action: "ctrl" }]
                .concat(row("%^=")).concat([{ label: "←", action: "left" }, { label: "↓", action: "down" },
                                            { label: "↑", action: "up" }, { label: "→", action: "right" }]),
            [{ label: "?123", action: "symbols", w: 1.5 }].concat(row("$&*()'\""))
                .concat([{ label: "⌫", action: "back", w: 1.5 }]),
            [{ label: "ABC", action: "letters", w: 1.5 }, { label: ",", out: "," },
             { label: "", action: "space", w: 5 }, { label: ".", out: "." },
             { label: "⏎", action: "enter", w: 1.5 }]
        ]
    })
    readonly property var codes: ({ back: 14, enter: 28, tab: 15, esc: 1, space: 57,
                                    left: 105, right: 106, up: 103, down: 108 })

    // Keys with their places, in pixels; letter rows are 10 keys wide, the
    // second row indented half a key.
    readonly property real keyW: panel.width / 10
    readonly property real rowH: keysHeight / 4
    readonly property var keys: {
        const out = [];
        const rows = layouts[page];
        for (let r = 0; r < rows.length; r++) {
            const units = rows[r].reduce((sum, k) => sum + (k.w || 1), 0);
            let x = (10 - units) / 2 * keyW;
            for (const k of rows[r]) {
                const w = (k.w || 1) * keyW;
                out.push(Object.assign({ x: x, y: stripHeight + r * rowH, width: w, height: rowH, row: r }, k));
                x += w;
            }
        }
        return out;
    }
    function keyAt(x, y) {
        for (const k of keys)
            if (x >= k.x && x < k.x + k.width && y >= k.y && y < k.y + k.height) return k;
        // Between rows or past an edge: the nearest key in the touched row.
        if (y < stripHeight) return null;
        const r = Math.min(3, Math.floor((y - stripHeight) / rowH));
        let best = null, dist = 1e9;
        for (const k of keys) {
            if (k.row !== r) continue;
            const d = Math.abs(x - (k.x + k.width / 2));
            if (d < dist) { dist = d; best = k; }
        }
        return best;
    }
    function isLetter(k) { return !!k && !k.action && /^[a-z]$/.test(k.out); }
    // Letter key centres for the decoder, in key widths from the letters' top left.
    function sendKeyCentres() {
        if (page !== "letters") return;
        send("keys " + keys.filter(isLetter).map(k =>
            k.out + ":" + ((k.x + k.width / 2) / keyW).toFixed(3) + "," +
            ((k.y - stripHeight + k.height / 2) / keyW).toFixed(3)).join(" "));
    }
    onKeysChanged: sendKeyCentres()

    // ---- output
    Process {
        id: vkbd
        command: [root.home + "/.local/bin/omarchy-mobile-vkbd",
                  root.home + "/.local/share/omarchy-mobile/keyboard/en.txt",
                  root.home + "/.local/share/omarchy-mobile/keyboard/user.txt"]
        running: true
        stdinEnabled: true
        stdout: SplitParser {
            onRead: line => {
                if (!line.startsWith("words")) return;
                root.commitSwipe(line.split(" ").slice(1).filter(w => w.length > 0));
            }
        }
        onRunningChanged: if (running) root.sendKeyCentres()
        onExited: restart.start()
    }
    Timer { id: restart; interval: 1000; onTriggered: vkbd.running = true }
    function send(line) { if (vkbd.running) vkbd.write(line + "\n"); }

    function typeText(text) {
        if (ctrl) { send("mods ctrl"); ctrl = false; }
        send("text " + text);
        lastTyped = (lastTyped + text).slice(-2);
    }
    function tapCode(name) {
        if (ctrl) { send("mods ctrl"); ctrl = false; }
        send("tap " + codes[name]);
        lastTyped = name === "space" ? (lastTyped + " ").slice(-2) : name === "enter" ? "\n" : "";
    }
    function learnPending() {
        if (pendingLearn !== "") send("learn " + pendingLearn.toLowerCase());
        pendingLearn = "";
    }
    // A tapped word ends at anything but a letter or an apostrophe.
    function endTappedWord() {
        if (tappedWord.length >= 2) send("learn " + tappedWord.toLowerCase());
        tappedWord = "";
    }
    // A letter the user typed: Shift once applies to it alone. Right after
    // a swiped word a new word starts, so it gets its space first.
    function typeKey(text) {
        learnPending();
        if (swiped !== "" && /^[A-Za-z0-9]$/.test(text)) {
            send("tap " + codes.space);
            lastTyped = " ";
        }
        swiped = "";
        suggestions = [];
        if (/^[A-Za-z']$/.test(text)) tappedWord += text;
        else endTappedWord();
        if (/^[a-z]$/.test(text) && shift > 0) {
            text = text.toUpperCase();
            if (shift === 1) shift = 0;
        }
        typeText(text);
    }
    // After a sentence ends, the next letter is a capital.
    function autoShift() {
        if (shift === 0 && (/[.!?] $/.test(lastTyped) || lastTyped === "\n")) shift = 1;
    }
    function action(name) {
        switch (name) {
        case "shift": shift = shift === 0 ? 1 : 0; return;
        case "symbols": case "letters": case "more": page = name; return;
        case "ctrl": ctrl = !ctrl; return;
        case "back":
            tappedWord = tappedWord.slice(0, -1);
            if (swiped !== "") {
                pendingLearn = "";
                for (let i = 0; i < swiped.length + (spaceBeforeSwipe ? 1 : 0); i++) send("tap " + codes.back);
                swiped = "";
                suggestions = [];
                lastTyped = "";
                return;
            }
            send("tap " + codes.back);
            lastTyped = "";
            return;
        case "space":
            learnPending();
            endTappedWord();
            swiped = "";
            tapCode("space");
            autoShift();
            return;
        case "enter":
            learnPending();
            endTappedWord();
            swiped = "";
            suggestions = [];
            tapCode("enter");
            autoShift();
            return;
        default:
            if (codes[name] !== undefined) tapCode(name);
        }
    }
    function commitSwipe(words) {
        if (words.length === 0) return;
        learnPending();
        endTappedWord();
        const word = applyCase(words[0]);
        spaceBeforeSwipe = swiped !== "" || /[^\s]$/.test(lastTyped);
        if (spaceBeforeSwipe) send("tap " + codes.space);
        typeText(word);
        swiped = word;
        pendingLearn = word;
        suggestions = words.map(applyCase);
        if (shift === 1) shift = 0;
    }
    function applyCase(word) {
        if (shift === 2) return word.toUpperCase();
        if (shift === 1) return word.charAt(0).toUpperCase() + word.slice(1);
        return word;
    }
    function useSuggestion(word) {
        if (swiped === "" || word === swiped) return;
        for (let i = 0; i < swiped.length; i++) send("tap " + codes.back);
        typeText(word);
        swiped = word;
        pendingLearn = word;
    }

    // ---- voice typing
    Process {
        id: dictation
        command: [root.home + "/.local/bin/omarchy-mobile-dictate", "listen"]
        stdinEnabled: true
        stdout: SplitParser {
            onRead: line => {
                let event;
                try { event = JSON.parse(line); } catch (e) { return; }
                if (event.state === "listening") root.micLive = true;
                else if (event.line !== undefined) root.typeSpoken(event.line);
                else if (event.partial !== undefined) root.spoken = event.partial;
                else if (event.error) {
                    root.dictationNote(event.error);
                    // Not installed yet: Settings has it.
                    if (event.setup) {
                        root.setShown(false);
                        Quickshell.execDetached([root.home + "/.local/bin/omarchy-mobile-settings", "keyboard"]);
                    }
                }
            }
        }
        onExited: { root.dictating = false; root.micLive = false; root.spoken = ""; }
    }
    Timer { id: noteTimer; interval: 4000; onTriggered: root.dictationMessage = "" }
    function dictationNote(text) { dictationMessage = text; noteTimer.restart(); }
    function toggleDictation() {
        if (dictating) {
            // What was said so far still comes, then the process ends.
            dictation.write("stop\n");
            return;
        }
        if (dictation.running) return;
        learnPending();
        endTappedWord();
        swiped = "";
        suggestions = [];
        dictationMessage = "";
        spoken = "";
        dictating = true;
        micLive = false;
        dictation.running = true;
    }
    // A phrase: after a word or a comma it goes on the sentence, so its
    // capital (the model starts every phrase with one) is dropped, but not
    // from "I".
    function typeSpoken(text) {
        text = text.trim();
        if (text === "") return;
        const before = lastTyped.replace(/ +$/, "");
        if (/[A-Za-z0-9,;:]$/.test(before) && !/^I(\b|')/.test(text))
            text = text.charAt(0).toLowerCase() + text.slice(1);
        if (lastTyped !== "" && !/\s$/.test(lastTyped)) text = " " + text;
        typeText(text);
        spoken = "";
        // A sentence ended: the next letter typed is a capital.
        shift = /[.!?]$/.test(text) ? 1 : 0;
    }

    // ---- show and hide
    function setShown(on) {
        if (on && !shown) { shift = 0; page = "letters"; lastTyped = ""; swiped = ""; suggestions = []; }
        if (!on && dictating) dictation.write("stop\n");
        shown = on;
    }
    // Commands carry the time they were written: one from before this
    // window started (more than a few seconds) is old and not replayed.
    readonly property real startedMs: Date.now()
    FileView {
        path: root.runtime + "/omarchy-mobile-keyboard.cmd"
        watchChanges: true
        onFileChanged: reload()
        onLoaded: {
            const parts = text().trim().split(" ");
            if (Number(parts[1] || 0) < root.startedMs - 5000) return;
            const verb = parts[0];
            if (verb === "show") root.setShown(true);
            else if (verb === "hide") root.setShown(false);
            else if (verb === "toggle") root.setShown(!root.shown);
        }
    }
    IpcHandler {
        target: "keyboard"
        function show(): void { root.setShown(true); }
        function hide(): void { root.setShown(false); }
        function toggle(): void { root.setShown(!root.shown); }
        function state(): string { return root.shown ? "shown" : "hidden"; }
    }

    PanelWindow {
        id: panel
        visible: root.shown
        anchors { left: true; right: true; bottom: true }
        implicitHeight: root.stripHeight + root.keysHeight + root.navGap
        exclusiveZone: root.overAndroid ? 0 : implicitHeight
        color: MobileTheme.background
        WlrLayershell.namespace: "omarchy-mobile-keyboard"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.None

        // Suggestions after a swipe; tap one to put it in place of the word
        // (the touch area below takes the tap).
        Row {
            id: strip
            x: 0; y: 0
            width: parent.width - root.micW; height: root.stripHeight
            visible: !root.dictating && root.dictationMessage === ""
            Repeater {
                model: root.suggestions
                delegate: Item {
                    required property string modelData
                    required property int index
                    width: strip.width / Math.max(1, root.suggestions.length); height: strip.height
                    Text {
                        anchors.centerIn: parent
                        text: modelData
                        color: modelData === root.swiped ? MobileTheme.accent : MobileTheme.foreground
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 17
                        font.bold: modelData === root.swiped
                    }
                    Rectangle {
                        visible: index > 0
                        width: 1; height: parent.height * 0.5
                        anchors.verticalCenter: parent.verticalCenter
                        color: MobileTheme.muted
                    }
                }
            }
        }

        // Listening: the phrase being said (its end, if long), or a message.
        Text {
            x: 12; y: 0
            width: panel.width - root.micW - 24; height: root.stripHeight
            visible: !strip.visible
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: implicitWidth > width ? Text.AlignRight : Text.AlignLeft
            clip: true
            text: root.dictationMessage !== "" ? root.dictationMessage : root.spoken !== "" ? root.spoken
                : root.micLive ? "Listening…" : "Starting…"
            color: root.dictationMessage !== "" || root.spoken === "" ? MobileTheme.secondary : MobileTheme.foreground
            font.family: MobileTheme.fontFamily
            font.pixelSize: 16
        }
        // The microphone; lit while starting, with a pulse once it hears.
        Item {
            x: panel.width - root.micW; y: 0
            width: root.micW; height: root.stripHeight
            Rectangle {
                anchors.centerIn: parent
                width: 34; height: 34; radius: 17
                color: MobileTheme.accent
                visible: root.dictating
                opacity: root.micLive ? 1 : 0.45
                SequentialAnimation on opacity {
                    running: root.dictating && root.micLive
                    loops: Animation.Infinite
                    NumberAnimation { from: 1; to: 0.55; duration: 700; easing.type: Easing.InOutSine }
                    NumberAnimation { from: 0.55; to: 1; duration: 700; easing.type: Easing.InOutSine }
                }
            }
            Text {
                anchors.centerIn: parent
                text: "\u{f036c}"
                color: root.dictating ? MobileTheme.background : MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 22
            }
        }

        Repeater {
            model: root.keys
            delegate: Rectangle {
                id: keyCap
                required property var modelData
                readonly property bool down: touch.pressedKeys.indexOf(modelData) >= 0
                readonly property bool lit: (modelData.action === "shift" && root.shift > 0)
                                            || (modelData.action === "ctrl" && root.ctrl)
                x: modelData.x + 3; y: modelData.y + 4
                width: modelData.width - 6; height: modelData.height - 8
                radius: MobileTheme.radius(8)
                color: down ? MobileTheme.selection
                     : lit ? MobileTheme.accent
                     : modelData.action ? MobileTheme.muted : MobileTheme.surface
                Text {
                    anchors.centerIn: parent
                    text: modelData.action === "shift" ? (root.shift === 2 ? "⇪" : "⇧")
                        : root.shift > 0 && /^[a-z]$/.test(modelData.label) ? modelData.label.toUpperCase()
                        : modelData.label
                    color: keyCap.lit ? MobileTheme.background : MobileTheme.foreground
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: modelData.label.length > 1 ? 15 : 21
                }
                Text {
                    visible: !!modelData.hint
                    anchors { top: parent.top; right: parent.right; topMargin: 2; rightMargin: 5 }
                    text: modelData.hint || ""
                    color: MobileTheme.secondary
                    opacity: 0.7
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 11
                }
            }
        }

        // The pressed key, larger, above the finger.
        Rectangle {
            id: bubble
            readonly property var key: touch.bubbleKey
            visible: !!key && !touch.swiping && !touch.altsShown && !key.action
            width: key ? key.width * 1.1 : 0; height: root.rowH * 1.1
            x: key ? Math.max(0, Math.min(panel.width - width, key.x + key.width / 2 - width / 2)) : 0
            y: key ? Math.max(0, key.y - height + 6) : 0
            radius: MobileTheme.radius(10)
            color: MobileTheme.surface
            border.color: MobileTheme.accent
            Text {
                anchors.centerIn: parent
                text: !bubble.key ? "" : touch.longPressed && bubble.key.hint ? bubble.key.hint
                    : root.shift > 0 && /^[a-z]$/.test(bubble.key.label) ? bubble.key.label.toUpperCase() : bubble.key.label
                color: MobileTheme.foreground
                font.family: MobileTheme.fontFamily
                font.pixelSize: 28
            }
        }

        // A long press on "," or ".": the alternatives in a row above the key.
        Rectangle {
            id: altsRow
            visible: touch.altsShown
            readonly property var key: touch.bubbleKey
            readonly property var alts: key && key.alts ? key.alts : []
            readonly property real cell: root.keyW * 0.9
            width: alts.length * cell; height: root.rowH * 0.9
            x: key ? Math.max(0, Math.min(panel.width - width, key.x + key.width / 2 - width / 2)) : 0
            y: key ? Math.max(0, key.y - height) : 0
            radius: MobileTheme.radius(10)
            color: MobileTheme.surface
            border.color: MobileTheme.accent
            Row {
                Repeater {
                    model: altsRow.alts
                    delegate: Rectangle {
                        required property string modelData
                        required property int index
                        width: altsRow.cell; height: altsRow.height
                        radius: MobileTheme.radius(8)
                        color: index === touch.altIndex ? MobileTheme.accent : "transparent"
                        Text {
                            anchors.centerIn: parent
                            text: modelData
                            color: index === touch.altIndex ? MobileTheme.background : MobileTheme.foreground
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 22
                        }
                    }
                }
            }
        }

        // The swipe's trail.
        Shape {
            anchors.fill: parent
            visible: touch.swiping
            ShapePath {
                strokeColor: MobileTheme.accent
                strokeWidth: 6
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                PathPolyline { path: touch.trail }
            }
        }

        MultiPointTouchArea {
            id: touch
            anchors.fill: parent
            maximumTouchPoints: 4
            mouseEnabled: true
            // Per finger: the key it went down on and its path.
            property var fingers: ({})
            property var pressedKeys: []
            property var bubbleKey: null
            property bool longPressed: false
            property bool altsShown: false
            property int altIndex: 0
            property bool swiping: false
            property var trail: []
            property int repeatId: -1

            function refresh() {
                const keys = [];
                for (const id in fingers) if (fingers[id].key && !fingers[id].swipe) keys.push(fingers[id].key);
                pressedKeys = keys;
            }
            onPressed: points => {
                for (const p of points) {
                    // The suggestion strip: picked on release.
                    if (p.y < root.stripHeight) { fingers[p.pointId] = { strip: true, x0: p.x }; continue; }
                    const k = root.keyAt(p.x, p.y);
                    fingers[p.pointId] = { key: k, x0: p.x, y0: p.y, path: [Qt.point(p.x, p.y)], swipe: false };
                    bubbleKey = k;
                    longPressed = false;
                    altsShown = false;
                    longPress.restart();
                    if (k && k.action === "back") { repeatId = p.pointId; repeatDelay.restart(); }
                }
                refresh();
            }
            onUpdated: points => {
                for (const p of points) {
                    const f = fingers[p.pointId];
                    if (!f || f.strip) continue;
                    f.path.push(Qt.point(p.x, p.y));
                    if (altsShown) {
                        altIndex = Math.max(0, Math.min(altsRow.alts.length - 1,
                                                         Math.floor((p.x - altsRow.x) / altsRow.cell)));
                        continue;
                    }
                    // A swipe: one finger, from a letter, more than half a key
                    // away and over another letter.
                    if (!f.swipe && Object.keys(fingers).length === 1 && root.page === "letters" && root.isLetter(f.key)
                            && Math.hypot(p.x - f.x0, p.y - f.y0) > root.keyW * 0.6) {
                        const now = root.keyAt(p.x, p.y);
                        if (root.isLetter(now) && now !== f.key) {
                            f.swipe = true;
                            swiping = true;
                            longPress.stop();
                        }
                    }
                    if (f.swipe) trail = f.path.slice();
                }
                refresh();
            }
            onReleased: points => {
                for (const p of points) {
                    const f = fingers[p.pointId];
                    delete fingers[p.pointId];
                    if (p.pointId === repeatId) { repeatId = -1; repeatDelay.stop(); repeat.stop(); if (repeat.fired) { repeat.fired = false; continue; } }
                    if (!f) continue;
                    if (f.strip) {
                        if (p.y >= root.stripHeight) continue;
                        if (f.x0 >= panel.width - root.micW && p.x >= panel.width - root.micW - 12) {
                            root.toggleDictation();
                            continue;
                        }
                        const n = root.suggestions.length;
                        if (n > 0 && !root.dictating && p.x < strip.width)
                            root.useSuggestion(root.suggestions[Math.max(0, Math.min(n - 1, Math.floor(p.x / (strip.width / n))))]);
                        continue;
                    }
                    if (f.swipe) {
                        f.path.push(Qt.point(p.x, p.y));
                        root.send("swipe " + f.path.map(q => (q.x / root.keyW).toFixed(2) + ","
                                                        + ((q.y - root.stripHeight) / root.keyW).toFixed(2)).join(" "));
                        swiping = false;
                        trail = [];
                    } else if (altsShown && f.key === bubbleKey) {
                        root.typeKey(altsRow.alts[altIndex]);
                    } else if (f.key) {
                        const k = f.key;
                        if (k.action) root.action(k.action);
                        else root.typeKey(longPressed && k === bubbleKey && k.hint ? k.hint : k.out);
                    }
                }
                longPress.stop();
                if (Object.keys(fingers).length === 0) { bubbleKey = null; altsShown = false; longPressed = false; }
                refresh();
            }
            onCanceled: points => {
                fingers = {};
                swiping = false; trail = []; bubbleKey = null; altsShown = false;
                repeatDelay.stop(); repeat.stop(); repeatId = -1;
                refresh();
            }
            Timer {
                id: longPress
                interval: 350
                onTriggered: {
                    const k = touch.bubbleKey;
                    if (!k) return;
                    if (k.action === "shift") { root.shift = 2; touch.fingers = {}; touch.bubbleKey = null; touch.refresh(); return; }
                    if (k.alts) { touch.altsShown = true; touch.altIndex = 0; }
                    else if (k.hint) touch.longPressed = true;
                }
            }
            Timer { id: repeatDelay; interval: 400; onTriggered: { repeat.fired = true; repeat.start(); root.action("back"); } }
            Timer { id: repeat; interval: 70; repeat: true; property bool fired: false; onTriggered: root.action("back") }
        }
    }
}
