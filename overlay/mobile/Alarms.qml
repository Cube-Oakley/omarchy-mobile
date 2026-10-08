import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import Quickshell.Wayland

// The shell's side of alarms and timers (omarchy-mobile-clock). The clock's
// watcher says what rings; this wakes the screen and shows it over
// everything, the lock included, with the alarm tone and vibration, until it
// is snoozed or dismissed. Alarms ring through Do Not Disturb and Silent, as
// on Android: their loudness is the alarm volume group's. The power and
// volume keys snooze (the shell's PowerKey and volume request). During a
// call it keeps out of the way: a banner over the top instead of the full
// screen, and vibration without the tone, which the call would pick up.
Scope {
    id: clock
    property bool inCall: false
    property var items: []
    property var nextAlarm: null
    property int snoozeMinutes: 10
    property string tone: ""
    property real now: Date.now()
    signal arrived()
    signal buzz()
    readonly property bool ringing: items.length > 0
    readonly property var current: ringing ? items[0] : null
    readonly property bool vibrate: items.some(item => item.vibrate !== false)
    readonly property bool isTimer: current !== null && current.kind === "timer"
    readonly property bool sounding: ringing && !inCall && tone !== ""

    function bin(name) { return Quickshell.env("HOME") + "/.local/bin/" + name; }
    property var queued: []
    function request(args) {
        if (helper.running) { queued = queued.concat([args]); return; }
        helper.command = [bin("omarchy-mobile-clock")].concat(args);
        helper.running = true;
    }
    function snooze() {
        if (!current) return;
        if (current.kind === "timer") request(["dismiss", current.key]);
        else request(["snooze", current.key]);
    }
    function dismiss() { if (current) request(["dismiss", current.key]); }
    function addMinute() { if (current && current.kind === "timer") request(["timer-add", current.id, "60"]); }
    function take(line) {
        let data = null;
        try { data = JSON.parse(line); } catch (e) { return; }
        const before = items.map(item => item.key);
        items = data.ringing || [];
        nextAlarm = data.next_alarm || null;
        snoozeMinutes = data.snooze_minutes || 10;
        if (items.some(item => before.indexOf(item.key) < 0)) {
            Quickshell.execDetached([bin("omarchy-mobile-display"), "wake"]);
            clock.arrived();
        }
    }
    onSoundingChanged: ringer.running = sounding

    Process {
        id: watcher
        command: [clock.bin("omarchy-mobile-clock"), "watch"]
        running: true
        stdout: SplitParser { onRead: line => clock.take(line) }
        // A watcher still running for a shell before this one holds the
        // lock (exit 3) until it notices; try again until this one runs.
        onExited: (code, status) => { clock.items = []; watchRetry.start(); }
    }
    Timer { id: watchRetry; interval: 5000; onTriggered: watcher.running = true }
    Process {
        id: toneMaker
        command: [clock.bin("omarchy-mobile-clock"), "tone"]
        running: true
        stdout: StdioCollector {
            onStreamFinished: { try { clock.tone = JSON.parse(this.text).path || ""; } catch (e) {} }
        }
    }
    Process {
        id: ringer
        command: ["pw-play", "--media-role", "Alarm", clock.tone]
        // One loop of the tone per run, for as long as something rings.
        onExited: if (clock.sounding) ringAgain.start()
    }
    Timer { id: ringAgain; interval: 10; onTriggered: if (clock.sounding) ringer.running = true }
    Timer {
        interval: 1600; repeat: true; triggeredOnStart: true
        running: clock.ringing && clock.vibrate
        onTriggered: clock.buzz()
    }
    Timer {
        interval: 1000; repeat: true; running: clock.ringing
        onTriggered: clock.now = Date.now()
    }
    Process {
        id: helper
        stdout: StdioCollector {}
        onExited: {
            if (!clock.queued.length) return;
            const next = clock.queued[0];
            clock.queued = clock.queued.slice(1);
            clock.request(next);
        }
    }

    PanelWindow {
        id: screen
        visible: clock.ringing && !clock.inCall
        anchors { top: true; bottom: true; left: true; right: true }
        exclusiveZone: 0
        exclusionMode: ExclusionMode.Ignore
        color: MobileTheme.background
        WlrLayershell.namespace: "omarchy-mobile-alarm"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.None

        readonly property bool timer: clock.isTimer
        readonly property string overdue: {
            if (!timer || !clock.current) return "";
            const s = Math.max(0, Math.floor(clock.now / 1000 - clock.current.due));
            const m = Math.floor(s / 60), r = s % 60;
            return "−" + m + ":" + (r < 10 ? "0" : "") + r;
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.topMargin: 72
            anchors.bottomMargin: 56
            anchors.leftMargin: 24
            anchors.rightMargin: 24
            spacing: 12
            Text {
                Layout.fillWidth: true
                text: screen.timer ? "TIMER" : "ALARM"
                color: MobileTheme.accent
                font.family: MobileTheme.fontFamily
                font.pixelSize: 12
                font.letterSpacing: 2.4
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
            }
            Item { Layout.preferredHeight: 24 }
            Item {
                Layout.alignment: Qt.AlignHCenter
                implicitWidth: 200; implicitHeight: 200
                // Rings pulse out of the bell in the theme's accent.
                Repeater {
                    model: 2
                    delegate: Rectangle {
                        required property int index
                        anchors.centerIn: parent
                        width: 120; height: 120
                        radius: MobileTheme.radius(60)
                        color: "transparent"
                        border.width: 2
                        border.color: MobileTheme.accent
                        opacity: 0
                        SequentialAnimation on scale {
                            running: screen.visible
                            loops: Animation.Infinite
                            PauseAnimation { duration: index * 700 }
                            NumberAnimation { from: 1; to: 1.65; duration: 1400; easing.type: Easing.OutCubic }
                        }
                        SequentialAnimation on opacity {
                            running: screen.visible
                            loops: Animation.Infinite
                            PauseAnimation { duration: index * 700 }
                            NumberAnimation { from: 0.9; to: 0; duration: 1400; easing.type: Easing.OutCubic }
                        }
                    }
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: 124; height: 124
                    radius: MobileTheme.radius(62)
                    color: MobileTheme.surface
                    border.width: 2
                    border.color: MobileTheme.accent
                    Text {
                        anchors.centerIn: parent
                        text: screen.timer ? "\u{F13AB}" : "\u{F0020}"	// nf-md-timer, nf-md-alarm
                        color: MobileTheme.accent
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 56
                        SequentialAnimation on rotation {
                            running: screen.visible && !screen.timer
                            loops: Animation.Infinite
                            NumberAnimation { to: 12; duration: 80 }
                            NumberAnimation { to: -12; duration: 160 }
                            NumberAnimation { to: 0; duration: 80 }
                            PauseAnimation { duration: 900 }
                        }
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                Layout.topMargin: 8
                text: screen.timer ? "Time's up" : Qt.formatDateTime(new Date(clock.now), "HH:mm")
                color: MobileTheme.foreground
                font.family: MobileTheme.fontFamily
                font.pixelSize: screen.timer ? 44 : 84
                font.weight: Font.Light
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                Layout.fillWidth: true
                visible: text.length > 0
                text: clock.current ? (clock.current.label || (screen.timer ? screen.overdue : "")) : ""
                textFormat: Text.PlainText
                color: MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 20
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                visible: screen.timer && !!(clock.current && clock.current.label)
                text: screen.overdue
                color: MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 16
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                Layout.fillWidth: true
                visible: clock.items.length > 1
                text: "and " + (clock.items.length - 1) + " more"
                color: MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 14
                horizontalAlignment: Text.AlignHCenter
            }
            Item { Layout.fillHeight: true }
            RowLayout {
                Layout.fillWidth: true
                spacing: 16
                AlarmButton {
                    Layout.fillWidth: true
                    label: screen.timer ? "+1:00" : "Snooze · " + clock.snoozeMinutes + " min"
                    tint: MobileTheme.surface
                    ink: MobileTheme.foreground
                    onClicked: screen.timer ? clock.addMinute() : clock.snooze()
                }
                AlarmButton {
                    Layout.fillWidth: true
                    label: screen.timer ? "Stop" : "Dismiss"
                    tint: MobileTheme.accent
                    ink: MobileTheme.background
                    onClicked: clock.dismiss()
                }
            }
        }
    }

    // During a call: the alarm over the top of the call screen.
    PanelWindow {
        id: banner
        visible: clock.ringing && clock.inCall
        anchors { top: true; left: true; right: true }
        implicitHeight: 112
        exclusiveZone: 0
        exclusionMode: ExclusionMode.Ignore
        color: "transparent"
        WlrLayershell.namespace: "omarchy-mobile-alarm"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.None
        Rectangle {
            anchors { fill: parent; margins: 10 }
            radius: MobileTheme.radius(18)
            color: MobileTheme.surface
            border.width: 1
            border.color: MobileTheme.accent
            RowLayout {
                anchors { fill: parent; leftMargin: 16; rightMargin: 12 }
                spacing: 10
                Text {
                    text: clock.isTimer ? "\u{F13AB}" : "\u{F0020}"
                    color: MobileTheme.accent
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 28
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        Layout.fillWidth: true
                        text: clock.isTimer ? "Time's up" : "Alarm · " + Qt.formatDateTime(new Date(clock.now), "HH:mm")
                        color: MobileTheme.foreground
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 16
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: clock.current ? clock.current.label || "" : ""
                        textFormat: Text.PlainText
                        color: MobileTheme.secondary
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 13
                        elide: Text.ElideRight
                    }
                }
                AlarmButton {
                    implicitWidth: 84
                    implicitHeight: 48
                    textSize: 14
                    label: clock.isTimer ? "+1:00" : "Snooze"
                    tint: MobileTheme.selection
                    ink: MobileTheme.foreground
                    onClicked: clock.isTimer ? clock.addMinute() : clock.snooze()
                }
                AlarmButton {
                    implicitWidth: 84
                    implicitHeight: 48
                    textSize: 14
                    label: clock.isTimer ? "Stop" : "Dismiss"
                    tint: MobileTheme.accent
                    ink: MobileTheme.background
                    onClicked: clock.dismiss()
                }
            }
        }
    }

    component AlarmButton: Rectangle {
        id: button
        property string label: ""
        property color tint: MobileTheme.surface
        property color ink: MobileTheme.foreground
        property int textSize: 18
        signal clicked()
        implicitHeight: 72
        radius: MobileTheme.radius(36)
        color: tap.pressed ? Qt.darker(tint, 1.25) : tint
        Text {
            anchors.centerIn: parent
            text: button.label
            color: button.ink
            font.family: MobileTheme.fontFamily
            font.pixelSize: button.textSize
            font.bold: true
        }
        TapHandler { id: tap; onTapped: button.clicked() }
    }
}
