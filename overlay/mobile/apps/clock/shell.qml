import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import OmarchyMobile

// Clock: alarms, world clocks, timers and the stopwatch. omarchy-mobile-clock
// keeps them all; the shell rings alarms and timers (Alarms.qml), so they go
// off with this app closed and the phone asleep. Launched with "alarms",
// "world", "timer" or "stopwatch", it opens on that tab.
ShellRoot {
    id: root
    readonly property var tabs: ["alarms", "world", "timer", "stopwatch"]
    property string tab: tabs.indexOf(Quickshell.env("OMARCHY_MOBILE_APP_ARG") || "") >= 0
        ? Quickshell.env("OMARCHY_MOBILE_APP_ARG") : "alarms"
    property var store: ({alarms: [], timers: [], zones: [], stopwatch: {running: false, elapsed: 0, laps: []}})
    property string message: ""
    property double now: Date.now()
    // The alarm being edited (a copy), or null.
    property var draft: null
    property bool confirmDelete: false
    // World clock: picking a zone to add.
    property bool addingZone: false
    property var zoneResults: []
    // Timer: the digits typed for a new one (hhmmss, right-aligned), and
    // whether the keypad shows with timers running.
    property string timerDigits: ""
    property bool newTimer: false
    property var queue: []
    readonly property string home: Quickshell.env("HOME")
    readonly property var dayNames: ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]
    readonly property var dayLetters: ["M", "T", "W", "T", "F", "S", "S"]

    function bin(name) { return home + "/.local/bin/" + name; }
    function run(args, input) {
        if (tool.running) { queue = queue.concat([[args, input]]); return; }
        tool.input = input || "";
        tool.command = [bin("omarchy-mobile-clock")].concat(args);
        tool.running = true;
    }
    function next() {
        if (!queue.length || tool.running) return;
        const item = queue[0];
        queue = queue.slice(1);
        run(item[0], item[1]);
    }
    function refresh() { run(["status"]); }
    function keyboard(action) {
        keyboardTool.command = [bin("omarchy-mobile-keyboard"), action];
        keyboardTool.running = true;
    }
    function pad(n) { return (n < 10 ? "0" : "") + n; }
    function hhmm(hour, minute) { return pad(hour) + ":" + pad(minute); }
    // Seconds as h:mm:ss or mm:ss; with hundredths for the stopwatch.
    function duration(seconds, hundredths) {
        const whole = Math.max(0, seconds);
        const s = Math.floor(whole);
        const h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60, r = s % 60;
        let text = h > 0 ? h + ":" + pad(m) + ":" + pad(r) : pad(m) + ":" + pad(r);
        if (hundredths) text += "." + pad(Math.floor((whole - s) * 100));
        return text;
    }
    function daysText(alarm) {
        const days = alarm.days || [];
        if (days.length === 7) return "Every day";
        if (days.length === 5 && days.every(d => d < 5)) return "Weekdays";
        if (days.length === 2 && days[0] === 5 && days[1] === 6) return "Weekends";
        if (days.length) return days.map(d => dayNames[d]).join(", ");
        if (!alarm.next) return "Once";
        const at = new Date(alarm.next * 1000);
        const today = new Date(now);
        return at.toDateString() === today.toDateString() ? "Today" : "Tomorrow";
    }
    function untilText(epoch) {
        const minutes = Math.max(0, Math.ceil((epoch * 1000 - now) / 60000));
        const h = Math.floor(minutes / 60), m = minutes % 60;
        if (h >= 24) return Math.floor(h / 24) + " d " + (h % 24) + " h";
        return h > 0 ? h + " h " + m + " min" : m + " min";
    }
    function newAlarm() {
        const at = new Date(now + 3600000);
        openDraft({id: "", hour: at.getHours(), minute: 0, days: [], label: "", vibrate: true, enabled: true});
    }
    function editAlarm(alarm) { openDraft(JSON.parse(JSON.stringify(alarm))); }
    // The wheels and the label are set once as an alarm opens; after that
    // they are the user's (a binding would be broken by the first touch).
    function openDraft(alarm) {
        draft = alarm;
        confirmDelete = false;
        hourWheel.currentIndex = alarm.hour;
        minuteWheel.currentIndex = alarm.minute;
        labelField.text = alarm.label || "";
    }
    function saveAlarm() {
        const copy = {hour: draft.hour, minute: draft.minute, days: draft.days, label: labelField.text.trim(),
                      vibrate: draft.vibrate, enabled: true};
        if (draft.id) copy.id = draft.id;
        run(["alarm-save"], JSON.stringify(copy));
        draft = null;
        labelField.editing = false;
        keyboard("hide");
    }
    function toggleDay(day) {
        const days = draft.days.slice();
        const at = days.indexOf(day);
        if (at >= 0) days.splice(at, 1); else days.push(day);
        days.sort();
        draft = Object.assign({}, draft, {days: days});
    }
    function zoneTime(offset) {
        const at = new Date(now + offset * 1000);
        return pad(at.getUTCHours()) + ":" + pad(at.getUTCMinutes());
    }
    function zoneNote(offset) {
        const local = store.local_offset || 0;
        const hours = (offset - local) / 3600;
        const here = new Date(now + local * 1000), there = new Date(now + offset * 1000);
        const day = there.getUTCDate() === here.getUTCDate() ? "Today"
            : (there.getTime() > here.getTime() ? "Tomorrow" : "Yesterday");
        if (hours === 0) return day + " · same time";
        const amount = Math.abs(hours) % 1 === 0 ? Math.abs(hours) : Math.abs(hours).toFixed(1);
        return day + " · " + amount + " h " + (hours > 0 ? "ahead" : "behind");
    }
    function addZone(name) {
        const names = (store.zones || []).map(z => z.zone).filter(z => z !== name).concat([name]);
        run(["zones-set"], JSON.stringify(names));
        addingZone = false;
        zoneField.text = "";
        zoneField.editing = false;
        keyboard("hide");
    }
    function removeZone(name) {
        run(["zones-set"], JSON.stringify((store.zones || []).map(z => z.zone).filter(z => z !== name)));
    }
    function timerSeconds() {
        const digits = ("000000" + timerDigits).slice(-6);
        return Number(digits.slice(0, 2)) * 3600 + Number(digits.slice(2, 4)) * 60 + Number(digits.slice(4, 6));
    }
    function timerLeft(timer) {
        if (timer.state === "running") return timer.end - now / 1000;
        if (timer.state === "ringing") return 0;
        return timer.left !== undefined ? timer.left : timer.duration;
    }
    // Timer and stopwatch changes show at once, as they would on the phone's
    // own clock; the helper's answer a moment later replaces the guess.
    function startTimer(seconds) {
        if (seconds <= 0) return;
        now = Date.now();
        const copy = JSON.parse(JSON.stringify(store));
        copy.timers = (copy.timers || []).concat([{id: "", label: "", duration: seconds, state: "running",
                                                   end: now / 1000 + seconds}]);
        store = copy;
        run(["timer-start", String(seconds)]);
        timerDigits = "";
        newTimer = false;
    }
    function timerAction(timer, action) {
        now = Date.now();
        const at = now / 1000;
        const left = timerLeft(timer);
        const change = {
            "timer-pause": {state: "paused", left: left, end: undefined},
            "timer-resume": {state: "running", end: at + left},
            "timer-reset": {state: "stopped", left: timer.duration, end: undefined},
            "timer-add": timer.state === "running" ? {end: timer.end + 60}
                       : timer.state === "ringing" ? {state: "running", end: at + 60} : {left: left + 60}
        }[action];
        const copy = JSON.parse(JSON.stringify(store));
        copy.timers = (copy.timers || []).map(t => t.id === timer.id ? Object.assign(t, change) : t);
        store = copy;
        run(action === "timer-add" ? [action, timer.id, "60"] : [action, timer.id]);
    }
    function deleteTimer(id) {
        store = Object.assign({}, store, {timers: (store.timers || []).filter(t => t.id !== id)});
        run(["timer-delete", id]);
    }
    function stopwatchAction(action) {
        now = Date.now();
        const at = now / 1000;
        const copy = JSON.parse(JSON.stringify(store));
        const w = copy.stopwatch;
        const total = stopwatchTotal();
        if (action === "start") Object.assign(w, {running: true, started: at});
        else if (action === "pause") Object.assign(w, {running: false, elapsed: total});
        else if (action === "lap") w.laps = (w.laps || []).concat([total]);
        else Object.assign(w, {running: false, started: 0, elapsed: 0, laps: []});
        store = copy;
        run(["stopwatch", action]);
    }
    function stopwatchTotal() {
        const w = store.stopwatch || {};
        return (w.elapsed || 0) + (w.running ? now / 1000 - w.started : 0);
    }
    function goBack() {
        if (labelField.editing || zoneField.editing) {
            labelField.editing = false;
            zoneField.editing = false;
            keyboard("hide");
            return;
        }
        if (draft) { draft = null; return; }
        if (addingZone) { addingZone = false; return; }
        if (newTimer && (store.timers || []).length) { newTimer = false; return; }
        Qt.quit();
    }
    Component.onCompleted: refresh()

    Process {
        id: tool
        property string input: ""
        stdinEnabled: true
        stdout: StdioCollector {}
        onStarted: { if (input) write(input); stdinEnabled = false; }
        onExited: {
            stdinEnabled = true;
            try {
                const result = JSON.parse(stdout.text);
                if (result.ok === false) root.message = result.error || "That did not work";
                else if (result.zones && !result.alarms) root.zoneResults = result.zones;
                else {
                    // The helper's times are a moment newer than the last
                    // tick: against that tick a 4 s timer read 4.1 s and
                    // showed 5 for a second.
                    root.now = Date.now();
                    root.store = result;
                    root.message = "";
                }
            } catch (e) { root.message = "Clock is not available"; }
            root.next();
        }
    }
    Process { id: keyboardTool }
    // The displayed time: smooth for the stopwatch on screen, each second
    // otherwise. Status again each minute keeps zone offsets and "next" true.
    Timer {
        interval: root.tab === "stopwatch" && root.store.stopwatch && root.store.stopwatch.running ? 40
                  : root.tab === "timer" && (root.store.timers || []).some(t => t.state === "running") ? 200 : 1000
        repeat: true
        running: true
        onTriggered: root.now = Date.now()
    }
    Timer { interval: 60000; repeat: true; running: true; onTriggered: root.refresh() }
    // The shell's watcher changes the store too (a ringing timer, a one-time
    // alarm turning off); follow it.
    FileView {
        path: (Quickshell.env("XDG_DATA_HOME") || root.home + "/.local/share") + "/omarchy-mobile/clock.json"
        watchChanges: true
        printErrors: false
        onFileChanged: root.refresh()
    }

    AppWindow {
        compact: true
        pageMargin: 16
        kicker: ""
        appTitle: "Clock"
        heading: root.draft ? (root.draft.id ? "Edit alarm" : "New alarm")
               : root.addingZone ? "Add a city"
               : ({alarms: "Alarms", world: "World clock", timer: "Timer", stopwatch: "Stopwatch"})[root.tab]
        backVisible: root.draft !== null || root.addingZone || (root.newTimer && (root.store.timers || []).length > 0)
        onBackClicked: root.goBack()

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            Text {
                Layout.fillWidth: true
                visible: root.message.length > 0
                text: root.message
                color: MobileTheme.danger
                font.family: MobileTheme.fontFamily
                font.pixelSize: 14
                wrapMode: Text.WordWrap
            }

            // Pages.
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                // Alarms: the list.
                Item {
                    anchors.fill: parent
                    visible: root.tab === "alarms" && !root.draft
                    Text {
                        id: nextLine
                        width: parent.width
                        visible: !!root.store.next_alarm
                        text: root.store.next_alarm ? "Next alarm in " + root.untilText(root.store.next_alarm) : ""
                        color: MobileTheme.secondary
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 14
                        bottomPadding: 12
                    }
                    ListView {
                        anchors { top: nextLine.visible ? nextLine.bottom : parent.top; left: parent.left; right: parent.right; bottom: parent.bottom }
                        clip: true
                        spacing: 10
                        model: root.store.alarms || []
                        bottomMargin: 96
                        delegate: Rectangle {
                            id: card
                            required property var modelData
                            width: ListView.view.width
                            height: 96
                            radius: MobileTheme.radius(18)
                            color: cardTap.pressed ? MobileTheme.selection : MobileTheme.surface
                            TapHandler { id: cardTap; onTapped: root.editAlarm(card.modelData) }
                            ColumnLayout {
                                anchors { left: parent.left; leftMargin: 20; verticalCenter: parent.verticalCenter; right: toggle.left; rightMargin: 12 }
                                spacing: 2
                                Text {
                                    text: root.hhmm(card.modelData.hour, card.modelData.minute)
                                    color: card.modelData.enabled ? MobileTheme.foreground : MobileTheme.muted
                                    font.family: MobileTheme.fontFamily
                                    font.pixelSize: 40
                                    font.weight: Font.Light
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: root.daysText(card.modelData) + (card.modelData.label ? " · " + card.modelData.label : "")
                                    textFormat: Text.PlainText
                                    color: card.modelData.enabled ? MobileTheme.secondary : MobileTheme.muted
                                    font.family: MobileTheme.fontFamily
                                    font.pixelSize: 14
                                    elide: Text.ElideRight
                                }
                            }
                            Toggle {
                                id: toggle
                                anchors { right: parent.right; rightMargin: 16; verticalCenter: parent.verticalCenter }
                                on: card.modelData.enabled
                                onToggled: root.run(["alarm-enable", card.modelData.id, on ? "on" : "off"])
                            }
                        }
                    }
                    Text {
                        anchors.centerIn: parent
                        visible: !(root.store.alarms || []).length
                        text: "\u{F0020}\n\nNo alarms yet"
                        horizontalAlignment: Text.AlignHCenter
                        color: MobileTheme.secondary
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 18
                    }
                    AddButton {
                        anchors { right: parent.right; bottom: parent.bottom; bottomMargin: 16 }
                        glyph: "\u{F0415}"
                        onClicked: root.newAlarm()
                    }
                }

                // Alarms: the editor.
                Flickable {
                    anchors.fill: parent
                    visible: root.draft !== null
                    contentHeight: editor.height
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ColumnLayout {
                        id: editor
                        width: parent.width
                        spacing: 18
                        RowLayout {
                            Layout.alignment: Qt.AlignHCenter
                            spacing: 8
                            Wheel {
                                id: hourWheel
                                size: 24
                                onPicked: index => { if (root.draft) root.draft = Object.assign({}, root.draft, {hour: index}); }
                            }
                            Text {
                                text: ":"
                                color: MobileTheme.foreground
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 56
                                font.weight: Font.Light
                            }
                            Wheel {
                                id: minuteWheel
                                size: 60
                                onPicked: index => { if (root.draft) root.draft = Object.assign({}, root.draft, {minute: index}); }
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: root.draft ? "Rings " + (root.draft.days.length ? root.daysText(root.draft).toLowerCase() : "once") : ""
                            color: MobileTheme.secondary
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 14
                            horizontalAlignment: Text.AlignHCenter
                        }
                        RowLayout {
                            Layout.alignment: Qt.AlignHCenter
                            spacing: 8
                            Repeater {
                                model: 7
                                Rectangle {
                                    id: chip
                                    required property int index
                                    readonly property bool on: root.draft !== null && root.draft.days.indexOf(index) >= 0
                                    width: 44; height: 44
                                    radius: MobileTheme.radius(22)
                                    color: on ? MobileTheme.accent : MobileTheme.surface
                                    border.width: 1
                                    border.color: on ? MobileTheme.accent : MobileTheme.muted
                                    Text {
                                        anchors.centerIn: parent
                                        text: root.dayLetters[chip.index]
                                        color: chip.on ? MobileTheme.background : MobileTheme.foreground
                                        font.family: MobileTheme.fontFamily
                                        font.pixelSize: 16
                                        font.bold: chip.on
                                    }
                                    TapHandler { onTapped: root.toggleDay(chip.index) }
                                }
                            }
                        }
                        TouchTextField {
                            id: labelField
                            Layout.fillWidth: true
                            placeholderText: "Label"
                            maximumLength: 60
                            onEditingRequested: { editing = true; forceActiveFocus(); root.keyboard("show"); }
                            onAccepted: { editing = false; root.keyboard("hide"); }
                        }
                        SettingsRow {
                            Layout.fillWidth: true
                            label: "Vibrate"
                            value: root.draft && root.draft.vibrate ? "On" : "Off"
                            selected: root.draft !== null && root.draft.vibrate
                            onClicked: root.draft = Object.assign({}, root.draft, {vibrate: !root.draft.vibrate})
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12
                            TouchButton {
                                Layout.fillWidth: true
                                visible: root.draft !== null && root.draft.id !== ""
                                label: root.confirmDelete ? "Delete it" : "Delete"
                                onClicked: {
                                    if (!root.confirmDelete) { root.confirmDelete = true; return; }
                                    root.run(["alarm-delete", root.draft.id]);
                                    root.draft = null;
                                }
                            }
                            TouchButton {
                                Layout.fillWidth: true
                                label: "Save"
                                selected: true
                                onClicked: root.saveAlarm()
                            }
                        }
                    }
                }

                // World clock.
                Item {
                    anchors.fill: parent
                    visible: root.tab === "world" && !root.addingZone
                    ColumnLayout {
                        id: localClock
                        width: parent.width
                        spacing: 0
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: Qt.formatDateTime(new Date(root.now), "HH:mm")
                            color: MobileTheme.foreground
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 72
                            font.weight: Font.Light
                        }
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.bottomMargin: 16
                            text: Qt.formatDateTime(new Date(root.now), "dddd, MMMM d")
                            color: MobileTheme.secondary
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 16
                        }
                    }
                    ListView {
                        anchors { top: localClock.bottom; left: parent.left; right: parent.right; bottom: parent.bottom }
                        clip: true
                        spacing: 10
                        bottomMargin: 96
                        model: root.store.zones || []
                        delegate: Rectangle {
                            id: zoneCard
                            required property var modelData
                            width: ListView.view.width
                            height: 76
                            radius: MobileTheme.radius(18)
                            color: MobileTheme.surface
                            ColumnLayout {
                                anchors { left: parent.left; leftMargin: 20; verticalCenter: parent.verticalCenter; right: zoneTimeText.left; rightMargin: 8 }
                                spacing: 2
                                Text {
                                    Layout.fillWidth: true
                                    text: zoneCard.modelData.city
                                    color: MobileTheme.foreground
                                    font.family: MobileTheme.fontFamily
                                    font.pixelSize: 18
                                    elide: Text.ElideRight
                                }
                                Text {
                                    text: root.zoneNote(zoneCard.modelData.offset)
                                    color: MobileTheme.secondary
                                    font.family: MobileTheme.fontFamily
                                    font.pixelSize: 13
                                }
                            }
                            Text {
                                id: zoneTimeText
                                anchors { right: removeButton.left; rightMargin: 8; verticalCenter: parent.verticalCenter }
                                text: root.zoneTime(zoneCard.modelData.offset)
                                color: MobileTheme.foreground
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 32
                                font.weight: Font.Light
                            }
                            Text {
                                id: removeButton
                                anchors { right: parent.right; rightMargin: 14; verticalCenter: parent.verticalCenter }
                                text: "\u{F0156}"
                                color: MobileTheme.secondary
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 20
                                TapHandler { margin: 12; onTapped: root.removeZone(zoneCard.modelData.zone) }
                            }
                        }
                    }
                    AddButton {
                        anchors { right: parent.right; bottom: parent.bottom; bottomMargin: 16 }
                        glyph: "\u{F0415}"
                        onClicked: { root.addingZone = true; root.zoneResults = []; }
                    }
                }

                // World clock: adding a city.
                ColumnLayout {
                    anchors.fill: parent
                    visible: root.addingZone
                    spacing: 12
                    TouchTextField {
                        id: zoneField
                        Layout.fillWidth: true
                        placeholderText: "\u{F0349}  City or region"
                        onEditingRequested: { editing = true; forceActiveFocus(); root.keyboard("show"); }
                        onTextChanged: zoneSearch.restart()
                    }
                    Timer {
                        id: zoneSearch
                        interval: 250
                        onTriggered: if (zoneField.text.trim().length >= 2) root.run(["zones-search", zoneField.text.trim()])
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: root.zoneResults
                        delegate: Rectangle {
                            id: result
                            required property var modelData
                            width: ListView.view.width
                            height: 60
                            color: resultTap.pressed ? MobileTheme.selection : "transparent"
                            TapHandler { id: resultTap; onTapped: root.addZone(result.modelData.zone) }
                            Text {
                                anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter; right: resultTime.left }
                                text: result.modelData.city + "  <font color='" + MobileTheme.secondary + "'>" + result.modelData.region + "</font>"
                                color: MobileTheme.foreground
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 17
                                elide: Text.ElideRight
                            }
                            Text {
                                id: resultTime
                                anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
                                text: root.zoneTime(result.modelData.offset)
                                color: MobileTheme.secondary
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 17
                            }
                        }
                    }
                }

                // Timer: the keypad for a new one.
                ColumnLayout {
                    anchors.fill: parent
                    visible: root.tab === "timer" && (root.newTimer || !(root.store.timers || []).length)
                    spacing: 14
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: 8
                        readonly property string digits: ("000000" + root.timerDigits).slice(-6)
                        text: digits.slice(0, 2) + "<font size='3'>h</font> " + digits.slice(2, 4) + "<font size='3'>m</font> "
                              + digits.slice(4, 6) + "<font size='3'>s</font>"
                        textFormat: Text.StyledText
                        color: root.timerDigits.length ? MobileTheme.foreground : MobileTheme.muted
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 52
                        font.weight: Font.Light
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 8
                        Repeater {
                            model: [60, 180, 300, 600, 900, 1800]
                            Rectangle {
                                id: preset
                                required property int modelData
                                width: (parent.width - 5 * 8) / 6
                                height: 40
                                radius: MobileTheme.radius(20)
                                color: presetTap.pressed ? MobileTheme.selection : MobileTheme.surface
                                Text {
                                    anchors.centerIn: parent
                                    text: preset.modelData / 60 + "m"
                                    color: MobileTheme.foreground
                                    font.family: MobileTheme.fontFamily
                                    font.pixelSize: 15
                                }
                                TapHandler { id: presetTap; onTapped: root.startTimer(preset.modelData) }
                            }
                        }
                    }
                    Grid {
                        Layout.alignment: Qt.AlignHCenter
                        columns: 3
                        rowSpacing: 10
                        columnSpacing: 22
                        Repeater {
                            model: ["1", "2", "3", "4", "5", "6", "7", "8", "9", "00", "0", "back"]
                            Rectangle {
                                id: key
                                required property string modelData
                                width: 76; height: 64
                                radius: MobileTheme.radius(32)
                                color: keyTap.pressed ? MobileTheme.selection : (modelData === "back" ? "transparent" : MobileTheme.surface)
                                Text {
                                    anchors.centerIn: parent
                                    text: key.modelData === "back" ? "\u{F0B5C}" : key.modelData
                                    color: MobileTheme.foreground
                                    font.family: MobileTheme.fontFamily
                                    font.pixelSize: 26
                                }
                                TapHandler {
                                    id: keyTap
                                    onTapped: {
                                        if (key.modelData === "back") root.timerDigits = root.timerDigits.slice(0, -1);
                                        else root.timerDigits = (root.timerDigits + key.modelData).replace(/^0+/, "").slice(0, 6);
                                    }
                                }
                            }
                        }
                    }
                    Item { Layout.fillHeight: true }
                    TouchButton {
                        Layout.fillWidth: true
                        Layout.bottomMargin: 8
                        label: "Start"
                        selected: true
                        visible: root.timerDigits.length > 0
                        onClicked: root.startTimer(root.timerSeconds())
                    }
                }

                // Timer: the timers.
                Item {
                    anchors.fill: parent
                    visible: root.tab === "timer" && !root.newTimer && (root.store.timers || []).length > 0
                    ListView {
                        anchors.fill: parent
                        clip: true
                        spacing: 12
                        bottomMargin: 96
                        model: root.store.timers || []
                        delegate: Rectangle {
                            id: timerCard
                            required property var modelData
                            readonly property real remaining: root.timerLeft(modelData)
                            readonly property bool running: modelData.state === "running"
                            width: ListView.view.width
                            height: 168
                            radius: MobileTheme.radius(18)
                            color: MobileTheme.surface
                            // How much is left, as a bar along the top.
                            Rectangle {
                                anchors { left: parent.left; top: parent.top; margins: 0 }
                                height: 4
                                radius: 2
                                width: parent.width * Math.max(0, Math.min(1, timerCard.remaining / Math.max(1, timerCard.modelData.duration)))
                                color: timerCard.modelData.state === "ringing" ? MobileTheme.danger : MobileTheme.accent
                            }
                            ColumnLayout {
                                anchors { fill: parent; margins: 16 }
                                spacing: 6
                                RowLayout {
                                    Layout.fillWidth: true
                                    Text {
                                        Layout.fillWidth: true
                                        text: (timerCard.modelData.label || root.duration(timerCard.modelData.duration) + " timer")
                                              + (timerCard.modelData.state === "paused" ? " · paused" : "")
                                        textFormat: Text.PlainText
                                        color: MobileTheme.secondary
                                        font.family: MobileTheme.fontFamily
                                        font.pixelSize: 14
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        text: "\u{F0156}"
                                        color: MobileTheme.secondary
                                        font.family: MobileTheme.fontFamily
                                        font.pixelSize: 20
                                        TapHandler { margin: 12; onTapped: root.deleteTimer(timerCard.modelData.id) }
                                    }
                                }
                                Text {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: timerCard.modelData.state === "ringing" ? "Time's up" : root.duration(Math.ceil(timerCard.remaining))
                                    color: timerCard.modelData.state === "paused" ? MobileTheme.secondary : MobileTheme.foreground
                                    font.family: MobileTheme.fontFamily
                                    font.pixelSize: 48
                                    font.weight: Font.Light
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 10
                                    TouchButton {
                                        Layout.fillWidth: true
                                        implicitHeight: 44
                                        label: "+1:00"
                                        onClicked: root.timerAction(timerCard.modelData, "timer-add")
                                    }
                                    TouchButton {
                                        Layout.fillWidth: true
                                        implicitHeight: 44
                                        label: "Reset"
                                        onClicked: root.timerAction(timerCard.modelData, "timer-reset")
                                    }
                                    TouchButton {
                                        Layout.fillWidth: true
                                        implicitHeight: 44
                                        selected: true
                                        label: timerCard.running ? "Pause" : timerCard.modelData.state === "ringing" ? "Stop" : "Start"
                                        onClicked: {
                                            const t = timerCard.modelData;
                                            root.timerAction(t, t.state === "running" ? "timer-pause"
                                                             : t.state === "ringing" ? "timer-reset" : "timer-resume");
                                        }
                                    }
                                }
                            }
                        }
                    }
                    AddButton {
                        anchors { right: parent.right; bottom: parent.bottom; bottomMargin: 16 }
                        glyph: "\u{F0415}"
                        onClicked: { root.timerDigits = ""; root.newTimer = true; }
                    }
                }

                // Stopwatch.
                ColumnLayout {
                    id: stopwatchPage
                    anchors.fill: parent
                    visible: root.tab === "stopwatch"
                    spacing: 16
                    readonly property var watch: root.store.stopwatch || ({running: false, elapsed: 0, laps: []})
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: 32
                        text: root.duration(root.stopwatchTotal(), true)
                        color: MobileTheme.foreground
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 60
                        font.weight: Font.Light
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        TouchButton {
                            Layout.fillWidth: true
                            label: stopwatchPage.watch.running ? "Lap" : "Reset"
                            enabled: stopwatchPage.watch.running || root.stopwatchTotal() > 0
                            opacity: enabled ? 1 : 0.4
                            onClicked: root.stopwatchAction(stopwatchPage.watch.running ? "lap" : "reset")
                        }
                        TouchButton {
                            Layout.fillWidth: true
                            selected: true
                            label: stopwatchPage.watch.running ? "Pause" : (root.stopwatchTotal() > 0 ? "Resume" : "Start")
                            onClicked: root.stopwatchAction(stopwatchPage.watch.running ? "pause" : "start")
                        }
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        // Newest first, as on Android.
                        model: (stopwatchPage.watch.laps || []).map((total, i, all) => ({n: i + 1, total: total, lap: total - (i ? all[i - 1] : 0)})).reverse()
                        delegate: RowLayout {
                            required property var modelData
                            width: ListView.view.width
                            height: 44
                            Text {
                                Layout.preferredWidth: 80
                                text: "Lap " + modelData.n
                                color: MobileTheme.secondary
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 16
                            }
                            Text {
                                Layout.fillWidth: true
                                text: root.duration(modelData.lap, true)
                                color: MobileTheme.foreground
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 18
                            }
                            Text {
                                text: root.duration(modelData.total, true)
                                color: MobileTheme.secondary
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 16
                            }
                        }
                    }
                }
            }

            // Tabs.
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 8
                visible: !root.draft && !root.addingZone
                spacing: 0
                Repeater {
                    model: [{id: "alarms", glyph: "\u{F0020}", name: "Alarms"}, {id: "world", glyph: "\u{F01E7}", name: "Clock"},
                            {id: "timer", glyph: "\u{F051F}", name: "Timer"}, {id: "stopwatch", glyph: "\u{F051B}", name: "Stopwatch"}]
                    Item {
                        id: tabItem
                        required property var modelData
                        readonly property bool current: root.tab === modelData.id
                        Layout.fillWidth: true
                        implicitHeight: 60
                        Rectangle {
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: 2
                            width: 56; height: 30
                            radius: MobileTheme.radius(15)
                            color: tabItem.current ? MobileTheme.selection : "transparent"
                            Text {
                                anchors.centerIn: parent
                                text: tabItem.modelData.glyph
                                color: tabItem.current ? MobileTheme.accent : MobileTheme.secondary
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 20
                            }
                        }
                        Text {
                            anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 4 }
                            text: tabItem.modelData.name
                            color: tabItem.current ? MobileTheme.foreground : MobileTheme.secondary
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 12
                            font.bold: tabItem.current
                        }
                        TapHandler { onTapped: { root.tab = tabItem.modelData.id; root.newTimer = false; } }
                    }
                }
            }
        }
    }

    // A wheel of numbers (hours, minutes) that snaps to one and wraps.
    component Wheel: Tumbler {
        id: wheel
        property int size: 60
        signal picked(int index)
        model: size
        visibleItemCount: 5
        implicitWidth: 96
        implicitHeight: 260
        wrap: true
        onMovingChanged: if (!moving) wheel.picked(currentIndex)
        delegate: Text {
            required property int index
            text: root.pad(index)
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: index === wheel.currentIndex ? MobileTheme.foreground : MobileTheme.secondary
            opacity: 1.0 - Math.min(0.8, Math.abs(Tumbler.displacement) / 2.5)
            font.family: MobileTheme.fontFamily
            font.pixelSize: index === wheel.currentIndex ? 56 : 36
            font.weight: Font.Light
        }
        Rectangle {
            anchors.centerIn: parent
            width: parent.width
            height: parent.height / wheel.visibleItemCount
            color: "transparent"
            border.width: 1
            border.color: MobileTheme.muted
            radius: MobileTheme.radius(12)
        }
    }

    component Toggle: Rectangle {
        id: sw
        property bool on: false
        signal toggled()
        width: 56; height: 32
        radius: MobileTheme.radius(16)
        color: on ? MobileTheme.accent : MobileTheme.muted
        Rectangle {
            width: 24; height: 24
            radius: MobileTheme.radius(12)
            y: 4
            x: sw.on ? sw.width - width - 4 : 4
            color: sw.on ? MobileTheme.background : MobileTheme.foreground
            Behavior on x { NumberAnimation { duration: 120 } }
        }
        TapHandler { margin: 10; onTapped: { sw.on = !sw.on; sw.toggled(); } }
    }

    component AddButton: Rectangle {
        id: fab
        property string glyph: ""
        signal clicked()
        width: 64; height: 64
        radius: MobileTheme.radius(20)
        color: fabTap.pressed ? Qt.darker(MobileTheme.accent, 1.2) : MobileTheme.accent
        Text {
            anchors.centerIn: parent
            text: fab.glyph
            color: MobileTheme.background
            font.family: MobileTheme.fontFamily
            font.pixelSize: 30
        }
        TapHandler { id: fabTap; onTapped: fab.clicked() }
    }
}
