import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import Quickshell.Wayland

// Settings → Security: the screen lock's PIN or password and the
// fingerprints, through the device's fingerprint service (Pixel:
// /run/omarchy-fingerprint/fingerprint.sock, see
// devices/pixel7pro/fingerprint), which keeps the PIN or password with the
// secure hardware (gatekeeper). Changing either asks for the current one
// first; this page keeps it in memory only while it is open. Adding a
// finger opens a page over the whole screen, so its target sits on the
// sensor and no shell gesture can start under a finger.
Item {
    id: page
    readonly property string servicePath: "/run/omarchy-fingerprint/fingerprint.sock"
    readonly property string controls: Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-controls"

    // The service's status.
    property bool serviceUp: false
    property string credential: "none"
    property int pinLength: 0
    property var fingers: []
    property var sensor: null
    property var panel: [1440, 3120]

    // Preferences (omarchy-mobile-prefs).
    property bool fingerprintUnlock: true
    property bool fingerprintHaptics: true
    property bool fingerprintAmbient: false

    // main, verify, choose, new, confirm, fingers
    property string step: "main"
    property string after: ""
    property string secret: ""
    property bool secretKnown: false
    property string kind: "pin"
    property string first: ""
    property string message: ""
    property bool busy: false
    property int removing: -1
    readonly property bool wantsKeyboard: visible && ((step === "verify" && credential === "password")
        || ((step === "new" || step === "confirm") && kind === "password"))

    function credentialName(name) { return name === "pin" ? "PIN" : name === "password" ? "Password" : "None"; }
    function hex(text) {
        const bytes = unescape(encodeURIComponent(text));
        let out = "";
        for (let i = 0; i < bytes.length; i++) out += ("0" + bytes.charCodeAt(i).toString(16)).slice(-2);
        return out;
    }
    function send(command) {
        if (!service || !service.connected) { message = "The fingerprint service is not running."; return false; }
        service.write(command + "\n");
        service.flush();
        return true;
    }
    function buzz(ms) {
        if (fingerprintHaptics) Quickshell.execDetached([controls, "vibrate", String(ms), "70"]);
    }

    // Steps.
    function back() {
        message = "";
        if (step === "confirm") { step = "new"; return; }
        if (step === "new") { step = credential === "none" ? "main" : "choose"; return; }
        step = "main";
    }
    function forget() {
        secret = "";
        secretKnown = false;
        first = "";
        step = "main";
        message = "";
        enrollment.close();
    }
    onVisibleChanged: if (!visible) { forget(); reconnect(); }
    // The current PIN or password, asked once while the page is open.
    function withSecret(next) {
        message = "";
        if (credential === "none" || secretKnown) { step = next; return; }
        after = next;
        step = "verify";
    }
    function openLock() { withSecret("choose"); }
    function openFingers() {
        if (credential === "none") {
            message = "Set a PIN or password first: it unlocks the phone when a finger is not recognized.";
            return;
        }
        withSecret("fingers");
    }

    function submitted(value) {
        message = "";
        if (step === "verify") {
            busy = true;
            if (send("verify " + hex(value))) pendingSecret = value;
            else busy = false;
        } else if (step === "new") {
            if (kind === "pin" && value.length < 4) { message = "Use at least 4 digits."; pinEntry.shake(); return; }
            if (kind === "password" && value.length < 4) { message = "Use at least 4 characters."; return; }
            first = value;
            step = "confirm";
        } else if (step === "confirm") {
            if (value !== first) {
                message = kind === "pin" ? "The PINs don't match. Choose it again." : "The passwords don't match. Choose it again.";
                first = "";
                step = "new";
                return;
            }
            busy = true;
            if (!send("setcred " + kind + " " + (credential === "none" ? "-" : hex(secret)) + " " + hex(value))) busy = false;
            else pendingSecret = value;
        }
        pinEntry.clear();
    }
    property string pendingSecret: ""

    function handle(line) {
        let e;
        try { e = JSON.parse(line); } catch (x) { return; }
        switch (e.event) {
        case "status":
            credential = e.credential;
            pinLength = e.length;
            fingers = e.fingers || [];
            sensor = e.sensor || null;
            panel = e.panel || panel;
            break;
        case "verified":
            busy = false;
            if (e.ok) {
                secret = pendingSecret;
                secretKnown = true;
                step = after;
            } else {
                message = e.throttled ? "Too many attempts. Try again in 30 seconds."
                    : credential === "pin" ? "Wrong PIN" : "Wrong password";
                pinEntry.shake();
            }
            pendingSecret = "";
            break;
        case "credential":
            busy = false;
            if (e.ok) {
                secret = pendingSecret;
                secretKnown = true;
                first = "";
                step = "main";
                message = (e.kind === "pin" ? "PIN" : "Password") + " set.";
            } else {
                message = e.error === "THROTTLED" ? "Too many attempts. Try again in 30 seconds."
                    : "It could not be set (" + e.error + ").";
                step = "main";
            }
            pendingSecret = "";
            break;
        default:
            enrollment.handle(e);
        }
    }

    // The service's socket while the page is open, a new one for each try:
    // Quickshell never tries a socket whose connection failed again (see
    // LockScreen.qml), so while the service is away, a fresh one every 2 s.
    property var service: null
    Component {
        id: socketComponent
        Socket {
            path: page.servicePath
            connected: true
            parser: SplitParser { onRead: line => page.handle(line) }
            onConnectedChanged: page.serviceUp = connected
        }
    }
    function reconnect() {
        if (service) service.destroy();
        service = page.visible ? socketComponent.createObject(page) : null;
        if (!service) serviceUp = false;
    }
    Timer {
        interval: 2000
        repeat: true
        triggeredOnStart: true
        running: page.visible && !page.serviceUp
        onTriggered: page.reconnect()
    }

    // Preferences.
    function applyPrefs(text) {
        try {
            const prefs = JSON.parse(text);
            if (prefs.error) return;
            fingerprintUnlock = prefs.fingerprintUnlock !== false;
            fingerprintHaptics = prefs.fingerprintHaptics !== false;
            fingerprintAmbient = prefs.fingerprintAmbient === true;
        } catch (x) {}
    }
    function setPref(key, value) {
        page[key] = value;
        const change = {};
        change[key] = value;
        prefsSave.payload = JSON.stringify(change) + "\n";
        prefsSave.running = true;
    }
    Component.onCompleted: prefsLoad.running = true
    Process {
        id: prefsLoad
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-prefs"]
        stdout: StdioCollector {}
        onExited: page.applyPrefs(stdout.text)
    }
    Process {
        id: prefsSave
        stdinEnabled: true
        property string payload: ""
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-prefs", "set"]
        stdout: StdioCollector {}
        onStarted: { write(prefsSave.payload); prefsSave.payload = ""; stdinEnabled = false; }
        onExited: { stdinEnabled = true; page.applyPrefs(stdout.text); }
    }

    // The overview.
    Flickable {
        anchors.fill: parent
        visible: page.step === "main"
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        contentHeight: overview.height
        Column {
            id: overview
            width: parent.width
            spacing: 12
            Text {
                width: parent.width
                text: "Security"
                color: MobileTheme.accent
                font.family: MobileTheme.fontFamily
                font.pixelSize: 36
                font.bold: true
            }
            SettingsRow {
                width: parent.width
                label: "Screen lock"
                value: page.serviceUp ? page.credentialName(page.credential) : "Unavailable"
                enabled: page.serviceUp
                onClicked: page.openLock()
            }
            SettingsRow {
                width: parent.width
                label: "Fingerprints"
                value: page.fingers.length === 0 ? "None" : page.fingers.length === 1 ? "1 finger" : page.fingers.length + " fingers"
                enabled: page.serviceUp && page.sensor !== null
                onClicked: page.openFingers()
            }
            SettingsRow {
                width: parent.width
                label: "Unlock with fingerprint"
                value: page.fingerprintUnlock ? "On" : "Off"
                selected: page.fingerprintUnlock
                onClicked: page.setPref("fingerprintUnlock", !page.fingerprintUnlock)
            }
            SettingsRow {
                width: parent.width
                label: "Vibrate on result"
                value: page.fingerprintHaptics ? "On" : "Off"
                selected: page.fingerprintHaptics
                onClicked: page.setPref("fingerprintHaptics", !page.fingerprintHaptics)
            }
            SettingsRow {
                width: parent.width
                label: "Fingerprint on always-on display"
                value: page.fingerprintAmbient ? "On" : "Off"
                selected: page.fingerprintAmbient
                onClicked: page.setPref("fingerprintAmbient", !page.fingerprintAmbient)
            }
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                visible: page.message !== ""
                text: page.message
                color: MobileTheme.foreground
                font.family: MobileTheme.fontFamily
                font.pixelSize: 15
            }
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                topPadding: 8
                text: !page.serviceUp
                    ? "The fingerprint service is not running, so the screen lock cannot be changed now."
                    : "The PIN or password unlocks the phone when a finger is not recognized, and after too many tries. "
                      + "With the fingerprint on the always-on display, a finger on its mark unlocks the phone from there; "
                      + "touch then stays on under the clock, which uses a little more battery."
                color: MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 14
            }
        }
    }

    // The current PIN or password, a new one, or its confirmation.
    ColumnLayout {
        anchors.fill: parent
        visible: page.step === "verify" || page.step === "new" || page.step === "confirm"
        spacing: 16
        onVisibleChanged: if (visible) { pinEntry.clear(); Qt.callLater(pinEntry.focusField); }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: page.step === "verify" ? (page.credential === "pin" ? "Enter your PIN" : "Enter your password")
                : page.step === "new" ? (page.kind === "pin" ? "Choose a PIN" : "Choose a password")
                : (page.kind === "pin" ? "Enter the PIN again" : "Enter the password again")
            color: MobileTheme.foreground
            font.family: MobileTheme.fontFamily
            font.pixelSize: 24
            font.bold: true
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: page.message !== "" ? page.message
                : page.step === "new" ? (page.kind === "pin" ? "4 to 16 digits." : "At least 4 characters.")
                : page.step === "verify" ? "To change the screen lock or fingerprints." : ""
            color: page.message !== "" ? MobileTheme.danger : MobileTheme.secondary
            font.family: MobileTheme.fontFamily
            font.pixelSize: 15
            Layout.preferredHeight: 40
        }
        Item { Layout.fillHeight: true }
        PinEntry {
            id: pinEntry
            Layout.fillWidth: true
            kind: page.step === "verify" ? page.credential : page.kind
            length: page.step === "verify" ? page.pinLength : page.step === "confirm" ? page.first.length : 0
            busy: page.busy
            onSubmitted: value => page.submitted(value)
        }
        Item { Layout.preferredHeight: 8 }
    }

    // PIN or password.
    Column {
        anchors.fill: parent
        visible: page.step === "choose"
        spacing: 12
        Text {
            width: parent.width
            text: "Screen lock"
            color: MobileTheme.foreground
            font.family: MobileTheme.fontFamily
            font.pixelSize: 24
            font.bold: true
        }
        SettingsRow {
            width: parent.width
            label: "PIN"
            value: page.credential === "pin" ? "Current" : ""
            onClicked: { page.kind = "pin"; page.step = "new"; }
        }
        SettingsRow {
            width: parent.width
            label: "Password"
            value: page.credential === "password" ? "Current" : ""
            onClicked: { page.kind = "password"; page.step = "new"; }
        }
    }

    // The fingers.
    Flickable {
        anchors.fill: parent
        visible: page.step === "fingers"
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        contentHeight: fingerList.height
        Column {
            id: fingerList
            width: parent.width
            spacing: 12
            Text {
                width: parent.width
                text: "Fingerprints"
                color: MobileTheme.foreground
                font.family: MobileTheme.fontFamily
                font.pixelSize: 24
                font.bold: true
            }
            Repeater {
                model: page.fingers
                SettingsRow {
                    required property int index
                    required property var modelData
                    width: fingerList.width
                    label: "Fingerprint " + (index + 1)
                    value: page.removing === modelData ? "Tap again to remove" : "Remove"
                    selected: page.removing === modelData
                    onClicked: {
                        if (page.removing === modelData) {
                            page.removing = -1;
                            page.send("remove " + modelData);
                        } else {
                            page.removing = modelData;
                        }
                    }
                }
            }
            SettingsRow {
                width: parent.width
                label: "Add a fingerprint"
                visible: page.fingers.length < 4
                onClicked: enrollment.open()
            }
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                topPadding: 8
                text: page.message !== "" ? page.message
                    : "Up to 4 fingers. Adding one takes about 20 touches on the sensor: the middle of your finger first, then its tip and edges."
                color: MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 14
            }
        }
    }

    // Adding a finger, over the whole screen.
    PanelWindow {
        id: enrollment
        property bool shown: false
        property bool running: false
        property bool done: false
        property bool fingerDown: false
        property int taps: 0
        property int left: 0
        property string hint: ""
        property string error: ""
        readonly property real fraction: taps + left > 0 ? taps / (taps + left) : 0
        readonly property string stage: taps === 0 ? "Put your finger on the sensor, and lift it when it buzzes."
            : fraction < 0.5 ? "Lift and touch again, moving your finger a little each time."
            : fraction < 0.75 ? "Now the tip of your finger."
            : fraction < 0.875 ? "Now the left edge of your finger."
            : "Now the right edge of your finger."
        readonly property real sensorX: page.sensor ? page.sensor.x * width / page.panel[0] : width / 2
        readonly property real sensorY: page.sensor ? page.sensor.y * height / page.panel[1] : height * 0.75
        readonly property real sensorR: page.sensor ? page.sensor.r * width / page.panel[0] : 40

        function open() {
            page.message = "";
            taps = 0;
            left = 0;
            hint = "";
            error = "";
            done = false;
            running = false;
            shown = true;
            if (page.send("enroll " + (page.credential === "none" ? "-" : page.hex(page.secret)))) running = true;
        }
        function close() {
            if (running) page.send("cancel");
            if (fingerDown) page.send("up");
            fingerDown = false;
            running = false;
            shown = false;
        }
        function handle(e) {
            if (!shown) return;
            switch (e.event) {
            case "acquired":
                hint = ({
                    PARTIAL: "Cover the whole sensor.",
                    INSUFFICIENT: "Press a little firmer.",
                    SENSOR_DIRTY: "Clean the screen over the sensor.",
                    TOO_FAST: "Hold until it buzzes.",
                    LIFT_TOO_SOON: "Hold until it buzzes.",
                    IMMOBILE: "Move your finger a little between touches.",
                })[e.info] || (e.info === "GOOD" ? "" : hint);
                break;
            case "progress":
                taps = e.done;
                left = e.left;
                hint = "";
                page.buzz(30);
                break;
            case "enrolled":
                running = false;
                done = true;
                page.buzz(60);
                break;
            case "error":
                running = false;
                if (e.error !== "CANCELED")
                    error = e.error === "NO_SPACE" ? "There is no room for another fingerprint."
                        : e.error === "WRONG_CREDENTIAL" ? "The PIN or password was not accepted."
                        : e.error === "TIMEOUT" ? "It took too long. Try again."
                        : "The sensor stopped (" + e.error + "). Try again.";
                break;
            }
        }

        visible: shown
        anchors { top: true; bottom: true; left: true; right: true }
        exclusionMode: ExclusionMode.Ignore
        exclusiveZone: 0
        color: MobileTheme.background
        WlrLayershell.namespace: "omarchy-mobile-fingerprint-enroll"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.None

        // Everything not on the sensor stops here.
        MouseArea { anchors.fill: parent }

        ColumnLayout {
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 28; topMargin: 72 }
            spacing: 14
            Text {
                Layout.fillWidth: true
                text: enrollment.done ? "Fingerprint added" : "Add a fingerprint"
                color: MobileTheme.foreground
                font.family: MobileTheme.fontFamily
                font.pixelSize: 28
                font.bold: true
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: enrollment.done ? "It can unlock the phone now."
                    : enrollment.error !== "" ? enrollment.error : enrollment.stage
                color: enrollment.error !== "" ? MobileTheme.danger : MobileTheme.foreground
                font.family: MobileTheme.fontFamily
                font.pixelSize: 18
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                visible: !enrollment.done && enrollment.error === ""
                text: enrollment.hint !== "" ? enrollment.hint
                    : enrollment.taps === 0 ? "About 20 touches."
                    : enrollment.left === 1 ? "1 more touch." : enrollment.left + " more touches."
                color: enrollment.hint !== "" ? MobileTheme.accent : MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 16
            }
        }

        // Progress around the sensor, and its mark.
        Canvas {
            id: progressRing
            width: enrollment.sensorR * 3.4
            height: width
            x: enrollment.sensorX - width / 2
            y: enrollment.sensorY - height / 2
            property real value: enrollment.done ? 1 : enrollment.fraction
            Behavior on value { NumberAnimation { duration: 250; easing.type: Easing.OutCubic } }
            onValueChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d");
                const c = width / 2, r = enrollment.sensorR * 1.38;
                ctx.reset();
                ctx.lineWidth = Math.max(4, enrollment.sensorR * 0.1);
                ctx.lineCap = "round";
                ctx.strokeStyle = MobileTheme.muted;
                ctx.beginPath();
                ctx.arc(c, c, r, 0, 2 * Math.PI);
                ctx.stroke();
                if (value > 0) {
                    ctx.strokeStyle = enrollment.done ? MobileTheme.success : MobileTheme.accent;
                    ctx.beginPath();
                    ctx.arc(c, c, r, -Math.PI / 2, -Math.PI / 2 + value * 2 * Math.PI);
                    ctx.stroke();
                }
            }
            Text {
                anchors.centerIn: parent
                text: enrollment.done ? "✓" : "󰈷"	// nf-md-fingerprint
                font.family: MobileTheme.fontFamily
                font.pixelSize: enrollment.sensorR * 1.5
                color: enrollment.done ? MobileTheme.success : MobileTheme.accent
                opacity: enrollment.fingerDown ? 0 : 0.9
            }
        }
        // The sensor: the service lights the circle and reads the finger.
        MouseArea {
            width: enrollment.sensorR * 3
            height: width
            x: enrollment.sensorX - width / 2
            y: enrollment.sensorY - height / 2
            enabled: enrollment.running
            onPressed: mouse => {
                const px = Math.round((x + mouse.x) * page.panel[0] / enrollment.width);
                const py = Math.round((y + mouse.y) * page.panel[1] / enrollment.height);
                enrollment.fingerDown = page.send("down " + px + " " + py);
            }
            onReleased: if (enrollment.fingerDown) { enrollment.fingerDown = false; page.send("up"); }
            onCanceled: if (enrollment.fingerDown) { enrollment.fingerDown = false; page.send("up"); }
        }

        Rectangle {
            anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 40 }
            width: 200
            height: 56
            radius: MobileTheme.radius(28)
            color: closeTap.pressed ? MobileTheme.selection : MobileTheme.surface
            Text {
                anchors.centerIn: parent
                text: enrollment.done ? "Done" : enrollment.error !== "" ? "Close" : "Cancel"
                color: MobileTheme.foreground
                font.family: MobileTheme.fontFamily
                font.pixelSize: 18
            }
            TapHandler { id: closeTap; onTapped: enrollment.close() }
        }
        Text {
            visible: enrollment.error !== ""
            anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 110 }
            text: "Try again"
            color: MobileTheme.accent
            font.family: MobileTheme.fontFamily
            font.pixelSize: 18
            TapHandler { onTapped: enrollment.open() }
        }
    }
}
