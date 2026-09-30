import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import Quickshell.Wayland

// The shell's side of phone calls. It follows omarchy-mobile-phone watch,
// wakes the screen and rings for an incoming call, and offers answer and
// decline over everything else. Once answered, the Phone app has the call.
// Ringing follows the alert slider: Silent (or Do Not Disturb) shows the call
// without sound or vibration, Vibrate only vibrates, Ring rings and vibrates
// when haptics are on. During a call on the earpiece, the proximity sensor
// blacks out the screen and swallows touches while it reads near, so a cheek
// cannot end the call.
Scope {
    id: phone
    property bool quiet: false
    property bool vibrateOnly: false
    property bool haptics: true
    property var calls: []
    property var audio: ({})
    property bool near: false
    property string answering: ""
    property string handedOff: ""
    property string silenced: ""
    property string ringtone: ""
    property real now: Date.now() / 1000
    signal openPhone()
    signal arrived()
    signal buzz()
    readonly property var ringingCall: calls.find(c => c.state === "ringing-in" || c.state === "waiting") || null
    readonly property var incoming: ringingCall && ringingCall.id !== answering ? ringingCall : null
    readonly property var ongoing: calls.find(c => ["active", "dialing", "ringing-out", "held"].indexOf(c.state) >= 0) || null
    // A waiting call (one already in progress) gets a short tone, not the ring.
    readonly property bool ringing: incoming !== null && incoming.state === "ringing-in" && incoming.id !== silenced
    // Only a call held to the ear claims the sensor (its infrared dot shows).
    readonly property bool earpieceCall: ongoing !== null && audio.speaker !== true
    readonly property string elapsed: {
        if (!ongoing) return "";
        if (ongoing.state !== "active") return ongoing.state === "held" ? "On hold" : "Calling";
        const s = Math.max(0, Math.floor(now - (ongoing.answered || ongoing.since || now)));
        const m = Math.floor(s / 60), r = s % 60;
        return m + ":" + (r < 10 ? "0" : "") + r;
    }

    function bin(name) { return Quickshell.env("HOME") + "/.local/bin/" + name; }
    function request(args) {
        if (helper.running) { queued = queued.concat([args]); return; }
        helper.command = [bin("omarchy-mobile-phone")].concat(args);
        helper.running = true;
    }
    property var queued: []
    // Take the id first: marking the call answered hides the screen by
    // clearing incoming, so incoming.id is gone after that.
    function answer() {
        if (!incoming) return;
        const id = incoming.id;
        answering = id;
        handedOff = "";
        request(["accept", id]);
    }
    function decline() {
        if (!incoming) return;
        const id = incoming.id;
        silenced = id;
        request(["hangup", id]);
    }
    function silence() { if (incoming) silenced = incoming.id; }
    function take(line) {
        let data = null;
        try { data = JSON.parse(line); } catch (e) { return; }
        const before = incoming ? incoming.id : "";
        calls = data.calls || [];
        audio = data.audio || {};
        if (!calls.some(c => c.id === answering)) answering = "";
        if (answering && handedOff !== answering && calls.some(c => c.id === answering && c.state === "active")) {
            handedOff = answering;
            // Let the incoming layer unmap before handing off to the shell,
            // which restores Phone to the mobile foreground workspace.
            handoff.restart();
        }
        if (!calls.some(c => c.id === silenced)) silenced = "";
        if (incoming && incoming.id !== before) {
            Quickshell.execDetached([bin("omarchy-mobile-display"), "wake"]);
            phone.arrived();
        }
    }
    onRingingChanged: {
        if (ringing && !quiet && !vibrateOnly && ringtone) ringer.running = true;
        else ringer.running = false;
    }
    onQuietChanged: if (quiet) ringer.running = false
    onVibrateOnlyChanged: if (vibrateOnly) ringer.running = false

    Process {
        id: watcher
        command: [phone.bin("omarchy-mobile-phone"), "watch"]
        running: true
        stdout: SplitParser { onRead: line => phone.take(line) }
        // Exit 3: no ModemManager on this system, so nothing to follow.
        onExited: (code, status) => { phone.calls = []; if (code !== 3) watchRetry.start(); }
    }
    Timer { id: watchRetry; interval: 5000; onTriggered: watcher.running = true }
    Timer {
        id: handoff
        interval: 150
        onTriggered: if (phone.answering && phone.calls.some(c => c.id === phone.answering && c.state === "active")) phone.openPhone()
    }
    Process {
        id: proximity
        command: ["stdbuf", "-oL", "monitor-sensor", "--proximity"]
        running: phone.earpieceCall
        stdout: SplitParser {
            onRead: line => {
                const match = /(?:Proximity value changed: |near: )([01])/.exec(line);
                if (match) phone.near = match[1] === "1";
            }
        }
        onRunningChanged: if (!running) phone.near = false
    }
    PanelWindow {
        id: cover
        visible: phone.near && phone.earpieceCall
        anchors { top: true; bottom: true; left: true; right: true }
        exclusiveZone: 0
        exclusionMode: ExclusionMode.Ignore
        color: "black"
        WlrLayershell.namespace: "omarchy-mobile-call-cover"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.None
        // Every touch lands here and goes nowhere.
        MouseArea { anchors.fill: parent }
    }
    Process {
        id: tone
        command: [phone.bin("omarchy-mobile-phone"), "ringtone"]
        running: true
        stdout: StdioCollector {
            onStreamFinished: {
                try { phone.ringtone = JSON.parse(this.text).path || ""; } catch (e) {}
            }
        }
    }
    Process {
        id: ringer
        command: ["pw-play", "--media-role", "Ringtone", phone.ringtone]
        // One loop of the ringtone per run, until the call is answered.
        onExited: if (phone.ringing && !phone.quiet && !phone.vibrateOnly) ringAgain.start()
    }
    Timer { id: ringAgain; interval: 10; onTriggered: if (phone.ringing && !phone.quiet && !phone.vibrateOnly) ringer.running = true }
    Timer {
        interval: 1800; repeat: true; triggeredOnStart: true
        running: phone.ringing && !phone.quiet && (phone.vibrateOnly || phone.haptics)
        onTriggered: phone.buzz()
    }
    Timer {
        interval: 1000; repeat: true; running: phone.ongoing !== null || phone.incoming !== null
        onTriggered: phone.now = Date.now() / 1000
    }
    Process {
        id: helper
        stdout: StdioCollector {}
        onExited: (code, status) => {
            if (helper.command[1] === "accept") {
                let accepted = code === 0;
                try { accepted = accepted && JSON.parse(stdout.text).ok !== false; }
                catch (e) { accepted = false; }
                if (!accepted) {
                    phone.answering = "";
                    phone.handedOff = "";
                    handoff.stop();
                }
            }
            if (!phone.queued.length) return;
            const next = phone.queued[0];
            phone.queued = phone.queued.slice(1);
            phone.request(next);
        }
    }

    PanelWindow {
        id: screen
        visible: phone.incoming !== null
        anchors { top: true; bottom: true; left: true; right: true }
        exclusiveZone: 0
        exclusionMode: ExclusionMode.Ignore
        color: MobileTheme.background
        WlrLayershell.namespace: "omarchy-mobile-call"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.None

        ColumnLayout {
            anchors.fill: parent
            anchors.topMargin: 72
            anchors.bottomMargin: 56
            anchors.leftMargin: 24
            anchors.rightMargin: 24
            spacing: 12
            Text {
                Layout.fillWidth: true
                text: phone.incoming && phone.incoming.state === "waiting" ? "CALL WAITING" : "INCOMING CALL"
                color: MobileTheme.accent
                font.family: MobileTheme.fontFamily
                font.pixelSize: 12
                font.letterSpacing: 2.4
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
            }
            Item { Layout.preferredHeight: 36 }
            Item {
                Layout.alignment: Qt.AlignHCenter
                implicitWidth: 200; implicitHeight: 200
                // Two rings pulse out of the caller in the theme's green.
                Repeater {
                    model: 2
                    delegate: Rectangle {
                        required property int index
                        anchors.centerIn: parent
                        width: 120; height: 120
                        radius: MobileTheme.radius(60)
                        color: "transparent"
                        border.width: 2
                        border.color: MobileTheme.success
                        opacity: 0
                        SequentialAnimation on scale {
                            running: screen.visible
                            loops: Animation.Infinite
                            PauseAnimation { duration: index * 800 }
                            NumberAnimation { from: 1; to: 1.65; duration: 1600; easing.type: Easing.OutCubic }
                        }
                        SequentialAnimation on opacity {
                            running: screen.visible
                            loops: Animation.Infinite
                            PauseAnimation { duration: index * 800 }
                            NumberAnimation { from: 0.9; to: 0; duration: 1600; easing.type: Easing.OutCubic }
                        }
                    }
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: 124; height: 124
                    radius: MobileTheme.radius(62)
                    color: "transparent"
                    border.width: 2
                    border.color: MobileTheme.success
                    Avatar {
                        anchors.centerIn: parent
                        size: 112
                        photo: phone.incoming ? (phone.incoming.photo || "") : ""
                        initials: phone.incoming ? (phone.incoming.initials || "") : ""
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                Layout.topMargin: 8
                text: phone.incoming ? (phone.incoming.name || phone.incoming.display || phone.incoming.number || "Unknown number") : ""
                textFormat: Text.PlainText
                color: MobileTheme.foreground
                font.family: MobileTheme.fontFamily
                font.pixelSize: 32
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                fontSizeMode: Text.HorizontalFit
                minimumPixelSize: 18
            }
            Text {
                Layout.fillWidth: true
                visible: !!(phone.incoming && phone.incoming.name)
                text: phone.incoming ? (phone.incoming.display || phone.incoming.number || "") : ""
                textFormat: Text.PlainText
                color: MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 16
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                Layout.fillWidth: true
                text: phone.quiet ? "Silent" : phone.vibrateOnly ? "Vibrate" : phone.ringing ? "Ringing" : "Silenced"
                color: MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 14
                horizontalAlignment: Text.AlignHCenter
            }
            Item { Layout.fillHeight: true }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                CallButton {
                    glyph: "\u{F03F5}"
                    label: "Decline"
                    tint: MobileTheme.danger
                    onClicked: phone.decline()
                }
                Item { Layout.fillWidth: true }
                CallButton {
                    glyph: "\u{F03F2}"
                    label: "Answer"
                    tint: MobileTheme.success
                    onClicked: phone.answer()
                }
                Item { Layout.fillWidth: true }
            }
            Text {
                Layout.fillWidth: true
                Layout.topMargin: 16
                visible: phone.ringing && !phone.quiet
                text: "Silence"
                color: silenceTap.pressed ? MobileTheme.accent : MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 15
                horizontalAlignment: Text.AlignHCenter
                TapHandler { id: silenceTap; margin: 16; onTapped: phone.silence() }
            }
        }
    }

    component CallButton: Item {
        id: button
        property string glyph: ""
        property string label: ""
        property color tint: MobileTheme.surface
        signal clicked()
        implicitWidth: 96
        implicitHeight: 124
        Rectangle {
            id: face
            width: 88; height: 88
            anchors.horizontalCenter: parent.horizontalCenter
            radius: MobileTheme.radius(44)
            color: tap.pressed ? Qt.darker(button.tint, 1.25) : button.tint
            Text {
                anchors.centerIn: parent
                text: button.glyph
                color: MobileTheme.background
                font.family: MobileTheme.fontFamily
                font.pixelSize: 38
            }
        }
        Text {
            anchors.top: face.bottom
            anchors.topMargin: 10
            anchors.horizontalCenter: parent.horizontalCenter
            text: button.label
            color: MobileTheme.foreground
            font.family: MobileTheme.fontFamily
            font.pixelSize: 14
        }
        TapHandler { id: tap; onTapped: button.clicked() }
    }
}
