import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import Quickshell.Wayland

// The lock screen, on Wayland's session lock: the compositor shows nothing
// else, except the surfaces hypr-mobile.lua lets above it (the always-on
// display, the status bar, the screen animation and volume as pictures; the
// call screens and the power menu with touch). It locks when the screen goes
// dark, if a PIN or password is set, and on start. It unlocks with a finger on
// the sensor's circle or with the PIN or password.
//
// The device's fingerprint service supplies the credential, the fingers and
// the sensor (Pixel: /run/omarchy-fingerprint/fingerprint.sock; see
// devices/pixel7pro/fingerprint). Without the service there is no credential
// to check, so a phone that has never had one set never locks. The last
// credential seen is kept (STATE), so a phone with one locks at boot, before
// the service is up, and waits for it to check the PIN.
Scope {
    id: lock
    property var prefs: ({})
    property bool screenOn: true
    // From the end of a close to the next open (the shell's screenDark).
    property bool dark: false
    property bool ambient: false
    signal keyboardRequested(bool show)

    readonly property string servicePath: "/run/omarchy-fingerprint/fingerprint.sock"
    readonly property string statePath: (Quickshell.env("XDG_STATE_HOME") || Quickshell.env("HOME") + "/.local/state")
        + "/omarchy-mobile/lock.json"

    property bool locked: false
    property bool serviceUp: false
    property string credential: "none"
    property int pinLength: 0
    property var fingers: []
    property var sensor: null
    property var panel: [1440, 3120]
    // The service refuses fingers until the PIN or password has been checked
    // after a restart or for 72 hours (pinReason restart or timeout), and
    // during the HAL's lockout (timed or permanent).
    property bool pinRequired: false
    property string pinReason: "none"
    property string lockout: "none"
    property bool cacheLoaded: false
    property bool statusSeen: false

    // The PIN or password page, over the clock.
    property bool bouncer: false
    property string entry: ""
    property bool checking: false
    property bool throttled: false
    property string message: ""
    property string fingerPhase: "idle"
    property bool lockedOut: false
    property bool authRunning: false
    property int authErrors: 0

    readonly property bool enabled: prefs.lockScreen !== false && credential !== "none"
    readonly property bool useFinger: prefs.fingerprintUnlock !== false && fingers.length > 0
        && sensor !== null && !lockedOut && !pinRequired && lockout === "none"
    readonly property string credentialName: credential === "pin" ? "PIN" : "password"
    // Why the PIN or password is needed instead of a finger, for the clock page.
    readonly property string pinNote: prefs.fingerprintUnlock === false || fingers.length === 0 ? ""
        : pinRequired && pinReason === "restart" ? credentialName + " required after restart"
        : pinRequired ? credentialName + " required for extra security"
        : lockout !== "none" || lockedOut ? "Too many attempts. Enter your " + credentialName + "."
        : ""
    readonly property bool haptics: prefs.fingerprintHaptics !== false
    // A finger on the sensor's mark in the always-on display, waiting for the
    // screen to light ({x, y} in panel pixels), and whether its down was sent.
    property var pendingFinger: null
    property bool ambientDown: false
    property bool authStarted: false
    readonly property bool lit: screenOn && !ambient
    readonly property bool wantAuth: locked && !bouncer && useFinger && serviceUp
        && (lit || pendingFinger !== null)

    function lockNow() {
        if (!enabled) return;
        bouncer = false;
        entry = "";
        message = "";
        fingerPhase = "idle";
        lockedOut = false;
        authErrors = 0;
        locked = true;
        sessionFile.setText("locked\n");
    }
    function unlock() {
        sessionFile.setText("unlocked\n");
        if (authRunning) send("cancel");
        authRunning = false;
        authStarted = false;
        pendingFinger = null;
        ambientDown = false;
        locked = false;
        bouncer = false;
        entry = "";
        message = "";
        fingerPhase = "idle";
        keyboardRequested(false);
    }

    function send(command) {
        if (!service || !service.connected) return false;
        service.write(command + "\n");
        service.flush();
        return true;
    }
    function hex(text) {
        const bytes = unescape(encodeURIComponent(text));
        let out = "";
        for (let i = 0; i < bytes.length; i++) out += ("0" + bytes.charCodeAt(i).toString(16)).slice(-2);
        return out;
    }
    function buzz(pattern) {
        if (!haptics) return;
        if (pattern === "success") MobileStatus.control(["vibrate", "25", "60"]);
        else MobileStatus.control(["vibrate", "60", "90"]);
    }

    // Authentication runs while the clock page is lit and a finger can unlock,
    // or as soon as a finger touches the sensor in the always-on display.
    onWantAuthChanged: Qt.callLater(syncAuth)
    onLitChanged: sendPending()

    // From the always-on display: the screen lights at once (unlock), and the
    // finger goes to the sensor once authentication runs and the panel has
    // left its low-power mode (it cannot light the circle in it).
    function ambientFinger(x, y) {
        if (!locked || !useFinger) return;
        pendingFinger = {x: x, y: y};
        message = "";
        Quickshell.execDetached([Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-display", "unlock"]);
        Qt.callLater(syncAuth);
    }
    function ambientFingerUp() {
        if (ambientDown) send("up");
        ambientDown = false;
        pendingFinger = null;
        if (fingerPhase === "pressed") fingerPhase = "idle";
    }
    function sendPending() {
        if (pendingFinger !== null && !ambientDown && authStarted && lit) {
            ambientDown = send("down " + pendingFinger.x + " " + pendingFinger.y);
        }
    }
    function syncAuth() {
        if (wantAuth && !authRunning && !rearm.running) {
            if (send("auth")) authRunning = true;
        } else if (!wantAuth && authRunning) {
            send("cancel");
        }
    }

    WlSessionLock {
        id: session
        // This scope's first child, as the scope is the shell's first: on a
        // reload Quickshell pairs a ShellRoot's or Scope's children with the
        // old ones by position (reloadableId counts only inside windows and
        // Variants). Paired, the new lock keeps the compositor's. Unpaired
        // (something added above it), it asks for a second lock, is refused,
        // and Quickshell aborts ("Tried to show lockscreen surfaces without
        // active lock"). A locked session is locked again before the reload
        // reaches it (lockNow on completion, sessionFile): one that starts
        // unlocked ends the compositor's lock.
        reloadableId: "omarchy-mobile-lock"
        locked: lock.locked

        WlSessionLockSurface {
            id: surface
            // Black while the screen is dark: the lock comes up as the screen
            // goes off, and its first frames must not show (a flash after the
            // close). Its content returns as the screen lights, under the open.
            color: lock.dark ? "black" : MobileTheme.background
            readonly property real unit: Math.min(width, height / 1.6) / 30
            readonly property real sensorX: lock.sensor ? lock.sensor.x * width / lock.panel[0] : width / 2
            readonly property real sensorY: lock.sensor ? lock.sensor.y * height / lock.panel[1] : height * 0.75
            readonly property real sensorR: lock.sensor ? lock.sensor.r * width / lock.panel[0] : unit * 2.5

            Item {
                anchors.fill: parent
                visible: !lock.dark

                Image {
                    anchors.fill: parent
                    source: MobileTheme.state.wallpaper ? MobileTheme.state.wallpaper.url : ""
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    opacity: 0.35
                }
                Rectangle {
                    anchors.fill: parent
                    gradient: Gradient {
                        GradientStop { position: 0; color: "#00000000" }
                        GradientStop { position: 1; color: "#cc000000" }
                    }
                }

                SystemClock { id: clock; precision: SystemClock.Minutes }

                // The clock page: time, date, the sensor, and the way to the PIN.
                Item {
                    id: glance
                    anchors.fill: parent
                    opacity: lock.bouncer ? 0 : 1
                    visible: opacity > 0
                    Behavior on opacity { NumberAnimation { duration: 180 } }

                    ColumnLayout {
                        id: glanceTime
                        anchors { top: parent.top; topMargin: parent.height * 0.14; horizontalCenter: parent.horizontalCenter }
                        spacing: surface.unit * 0.4
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: Qt.formatTime(clock.date, "h:mm")
                            color: MobileTheme.foreground
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: surface.unit * 5.5
                            font.bold: true
                        }
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: Qt.formatDate(clock.date, "dddd, MMMM d")
                            color: MobileTheme.secondary
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: surface.unit * 1.2
                        }
                    }

                    // What is playing, with its controls, as on Android's lock
                    // screen (the player itself opens only once unlocked).
                    MediaCard {
                        anchors { top: glanceTime.bottom; topMargin: surface.unit * 2.5; horizontalCenter: parent.horizontalCenter }
                        width: Math.min(parent.width - surface.unit * 4, surface.unit * 34)
                        compact: true
                        active: glance.visible
                    }

                    // Swipe up anywhere off the sensor for the PIN or password
                    // (behind everything else on the page).
                    MouseArea {
                        anchors.fill: parent
                        z: -1
                        property real startY: 0
                        onPressed: mouse => startY = mouse.y
                        onReleased: mouse => { if (mouse.y - startY < -surface.unit * 4) lock.bouncer = true; }
                    }

                    Text {
                        anchors { horizontalCenter: parent.horizontalCenter; bottom: fingerprint.visible ? fingerprint.top : hintLine.top; bottomMargin: surface.unit * 1.2 }
                        width: parent.width - surface.unit * 4
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        text: lock.message !== "" ? lock.message : lock.pinNote
                        visible: text !== ""
                        color: lock.fingerPhase === "failed" ? MobileTheme.danger : MobileTheme.foreground
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: surface.unit * 1.1
                    }

                    FingerprintRing {
                        id: fingerprint
                        visible: lock.useFinger && lock.serviceUp
                        radius: surface.sensorR
                        phase: lock.fingerPhase
                        x: surface.sensorX - width / 2
                        y: surface.sensorY - height / 2
                    }
                    // Touches on the circle go to the service, which lights it and
                    // reads the finger; they never reach the swipe.
                    MouseArea {
                        visible: fingerprint.visible && lock.authRunning
                        width: surface.sensorR * 3
                        height: width
                        x: surface.sensorX - width / 2
                        y: surface.sensorY - height / 2
                        property bool down: false
                        onPressed: mouse => {
                            const px = Math.round((x + mouse.x) * lock.panel[0] / surface.width);
                            const py = Math.round((y + mouse.y) * lock.panel[1] / surface.height);
                            down = lock.send("down " + px + " " + py);
                            lock.message = "";
                        }
                        onReleased: if (down) { down = false; lock.send("up"); if (lock.fingerPhase === "pressed") lock.fingerPhase = "idle"; }
                        onCanceled: if (down) { down = false; lock.send("up"); if (lock.fingerPhase === "pressed") lock.fingerPhase = "idle"; }
                    }

                    Text {
                        id: hintLine
                        anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: surface.unit * 2.2 }
                        text: !lock.serviceUp && lock.credential !== "none" ? "Unlock service is starting…"
                            : lock.credential === "pin" ? "Swipe up for PIN" : "Swipe up for password"
                        color: MobileTheme.secondary
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: surface.unit * 1
                        TapHandler { onTapped: lock.bouncer = true }
                    }
                }

                // The PIN or password page.
                Item {
                    id: bouncerPage
                    anchors.fill: parent
                    opacity: lock.bouncer ? 1 : 0
                    visible: opacity > 0
                    Behavior on opacity { NumberAnimation { duration: 180 } }
                    onVisibleChanged: {
                        if (visible && lock.credential === "password") { password.forceActiveFocus(); lock.keyboardRequested(true); }
                        if (!visible) lock.keyboardRequested(false);
                    }

                    // A swipe down on the background goes back to the clock (behind
                    // the keypad, so keys never reach it).
                    MouseArea {
                        anchors.fill: parent
                        z: -1
                        property real startY: 0
                        onPressed: mouse => startY = mouse.y
                        onReleased: mouse => { if (mouse.y - startY > surface.unit * 4) lock.bouncer = false; }
                    }

                    ColumnLayout {
                        anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: parent.height * (lock.credential === "pin" ? 0.16 : 0.2) }
                        width: Math.min(parent.width - surface.unit * 4, surface.unit * 22)
                        spacing: surface.unit * 1.2

                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: lock.credential === "pin" ? "Enter PIN" : "Enter password"
                            color: MobileTheme.foreground
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: surface.unit * 1.5
                            font.bold: true
                        }
                        // The PIN's digits so far.
                        Row {
                            id: dots
                            visible: lock.credential === "pin"
                            Layout.alignment: Qt.AlignHCenter
                            spacing: surface.unit * 0.9
                            transform: Translate { id: dotsOffset }
                            Repeater {
                                model: Math.max(lock.pinLength, lock.entry.length, 4)
                                Rectangle {
                                    width: surface.unit * 1.1
                                    height: width
                                    radius: width / 2
                                    color: index < lock.entry.length ? MobileTheme.foreground : "transparent"
                                    border.width: 2
                                    border.color: MobileTheme.foreground
                                }
                            }
                        }
                        SequentialAnimation {
                            id: dotsShake
                            NumberAnimation { target: dotsOffset; property: "x"; to: -surface.unit; duration: 50 }
                            NumberAnimation { target: dotsOffset; property: "x"; to: surface.unit; duration: 80 }
                            NumberAnimation { target: dotsOffset; property: "x"; to: -surface.unit * 0.5; duration: 70 }
                            NumberAnimation { target: dotsOffset; property: "x"; to: 0; duration: 60 }
                        }
                        Rectangle {
                            visible: lock.credential === "password"
                            Layout.fillWidth: true
                            implicitHeight: surface.unit * 3.4
                            radius: MobileTheme.radius(12)
                            color: MobileTheme.surface
                            border.width: 1
                            border.color: MobileTheme.muted
                            TextInput {
                                id: password
                                anchors { fill: parent; margins: surface.unit }
                                verticalAlignment: TextInput.AlignVCenter
                                echoMode: TextInput.Password
                                color: MobileTheme.foreground
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: surface.unit * 1.3
                                enabled: !lock.checking && !lock.throttled
                                text: lock.entry
                                onTextEdited: lock.entry = text
                                onAccepted: lock.submit()
                            }
                        }
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            text: lock.checking ? "Checking…" : lock.message
                            color: lock.checking ? MobileTheme.secondary : MobileTheme.danger
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: surface.unit * 1
                            Layout.preferredHeight: surface.unit * 2.6
                        }

                        Text {
                            visible: lock.credential === "password"
                            Layout.alignment: Qt.AlignHCenter
                            text: "Unlock"
                            color: MobileTheme.accent
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: surface.unit * 1.3
                            TapHandler { onTapped: lock.submit() }
                        }
                    }

                    // The keypad, low on the screen where a thumb reaches.
                    Grid {
                        id: keypad
                        visible: lock.credential === "pin"
                        readonly property real key: Math.min((parent.width - surface.unit * 4) / 3.5, surface.unit * 5.6)
                        anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: surface.unit * 3 }
                        columns: 3
                        rowSpacing: key * 0.18
                        columnSpacing: key * 0.3
                        Repeater {
                            model: ["1", "2", "3", "4", "5", "6", "7", "8", "9", "back", "0", "enter"]
                            Rectangle {
                                required property string modelData
                                width: keypad.key
                                height: keypad.key
                                radius: width / 2
                                color: key.pressed ? MobileTheme.selection : (modelData.length === 1 ? MobileTheme.surface : "transparent")
                                opacity: lock.checking || lock.throttled ? 0.4 : 1
                                Text {
                                    anchors.centerIn: parent
                                    text: parent.modelData === "back" ? "\u232b" : parent.modelData === "enter" ? "\u2713" : parent.modelData
                                    color: MobileTheme.foreground
                                    font.family: MobileTheme.fontFamily
                                    font.pixelSize: keypad.key * (parent.modelData.length === 1 ? 0.42 : 0.36)
                                }
                                MouseArea {
                                    id: key
                                    anchors.fill: parent
                                    enabled: !lock.checking && !lock.throttled
                                    onClicked: {
                                        if (parent.modelData === "back") lock.entry = lock.entry.slice(0, -1);
                                        else if (parent.modelData === "enter") lock.submit();
                                        else if (lock.entry.length < 16) {
                                            lock.entry += parent.modelData;
                                            lock.message = "";
                                            if (lock.pinLength > 0 && lock.entry.length === lock.pinLength) lock.submit();
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // After an error (not a cancel) authentication is armed again, slower each
    // time, so a failing sensor cannot spin.
    Timer { id: rearm; interval: 1000 * Math.min(30, 1 << Math.min(lock.authErrors, 5)); onTriggered: lock.syncAuth() }

    function submit() {
        if (checking || throttled || entry.length === 0) return;
        if (!send("verify " + hex(entry))) {
            message = "Unlock service is starting…";
            return;
        }
        checking = true;
    }
    function wrong(text) {
        message = text;
        entry = "";
        buzz("fail");
        dotsShake.restart();
    }
    function hint(info) {
        return {
            PARTIAL: "Cover the whole sensor",
            INSUFFICIENT: "Press a little firmer",
            SENSOR_DIRTY: "Clean the screen over the sensor",
            TOO_FAST: "Hold your finger still a moment",
            TOO_SLOW: "Lift and try again",
            IMMOBILE: "Move your finger slightly",
            LIFT_TOO_SOON: "Hold until it reads",
        }[info] || "";
    }

    function handle(line) {
        let e;
        try { e = JSON.parse(line); } catch (x) { return; }
        switch (e.event) {
        case "status":
            statusSeen = true;
            pinRequired = e.pinRequired === true;
            pinReason = e.pinReason || "none";
            lockout = e.lockout || "none";
            if (lockout === "none") lockedOut = false;
            credential = e.credential;
            pinLength = e.length;
            fingers = e.fingers || [];
            sensor = e.sensor || null;
            panel = e.panel || panel;
            saveState();
            if (locked && credential === "none") unlock();
            break;
        case "started":
            if (e.operation === "auth") {
                authErrors = 0;
                authStarted = true;
                sendPending();
            }
            break;
        case "lit":
            fingerPhase = "pressed";
            break;
        case "acquired":
            if (hint(e.info)) message = hint(e.info);
            break;
        case "match":
            authRunning = false;
            authStarted = false;
            fingerPhase = "matched";
            message = "";
            buzz("success");
            unlockSoon.restart();
            break;
        case "nomatch":
            fingerPhase = "failed";
            message = "Not recognized";
            buzz("fail");
            idleAgain.restart();
            break;
        case "lockout":
            authRunning = false;
            authStarted = false;
            lockedOut = true;
            fingerPhase = "idle";
            message = credential === "pin" ? "Too many attempts. Enter your PIN." : "Too many attempts. Enter your password.";
            bouncer = true;
            break;
        case "error":
            authRunning = false;
            authStarted = false;
            // Refused until the PIN or password: the status that follows it
            // arms authentication again.
            if (e.error === "PIN_REQUIRED" || e.error === "LOCKOUT" || e.error === "NO_FINGERS")
                break;
            if (e.error !== "CANCELED") {
                authErrors++;
                rearm.restart();
            } else {
                Qt.callLater(syncAuth);
            }
            break;
        case "verified":
            checking = false;
            if (e.ok) {
                buzz("success");
                unlock();
            } else if (e.throttled) {
                throttled = true;
                wrong("Too many attempts. Try again in 30 seconds.");
                unthrottle.restart();
            } else {
                wrong(credential === "pin" ? "Wrong PIN" : "Wrong password");
            }
            break;
        }
    }

    Timer { id: unlockSoon; interval: 260; onTriggered: lock.unlock() }
    Timer { id: idleAgain; interval: 700; onTriggered: if (lock.fingerPhase === "failed") lock.fingerPhase = "idle" }
    Timer { id: unthrottle; interval: 30000; onTriggered: { lock.throttled = false; lock.message = ""; } }

    // The service's socket, a new one for each try: Quickshell keeps a socket
    // whose connection failed and never tries it again (taking it down waits
    // for a disconnection that cannot come). So while the service is away
    // (not started yet at boot, or restarting) a fresh socket every 2 s.
    property var service: null
    Component {
        id: socketComponent
        Socket {
            path: lock.servicePath
            connected: true
            parser: SplitParser { onRead: line => lock.handle(line) }
            onConnectedChanged: {
                lock.serviceUp = connected;
                lock.authRunning = false;
                lock.authStarted = false;
                if (connected) Qt.callLater(lock.syncAuth);
            }
        }
    }
    function reconnect() {
        if (service) service.destroy();
        service = socketComponent.createObject(lock);
    }
    Timer {
        interval: 2000
        repeat: true
        triggeredOnStart: true
        running: !lock.serviceUp
        onTriggered: lock.reconnect()
    }

    // The last credential and sensor seen, for locking before the service is
    // up. Read whole on start (blockLoading): a preloaded file's loaded
    // signal can come before a handler is attached.
    FileView {
        id: stateFile
        path: lock.statePath
        atomicWrites: true
        blockLoading: true
        printErrors: false
    }
    Component.onCompleted: {
        stateFile.reload();
        stateFile.waitForJob();
        try {
            const saved = JSON.parse(stateFile.text());
            if (!statusSeen) {
                credential = saved.credential || "none";
                pinLength = saved.length || 0;
                fingers = saved.fingers || [];
                sensor = saved.sensor || null;
                panel = saved.panel || panel;
            }
        } catch (x) {}
        cacheLoaded = true;
        if (statusSeen) saveState();
        sessionFile.reload();
        sessionFile.waitForJob();
        if (sessionFile.text().trim() !== "unlocked") lockNow();
    }
    // Whether this session is unlocked, in the runtime directory: a shell that
    // starts again (a reload, a crash) keeps an unlocked phone unlocked rather
    // than locking it in the user's hands. A boot has no file, and locks.
    FileView {
        id: sessionFile
        path: (Quickshell.env("XDG_RUNTIME_DIR") || "/tmp") + "/omarchy-mobile-lock-session"
        blockLoading: true
        printErrors: false
    }
    function saveState() {
        if (!cacheLoaded) return;
        const text = JSON.stringify({credential: credential, length: pinLength, fingers: fingers,
                                     sensor: sensor, panel: panel});
        if (text !== stateFile.text()) stateFile.setText(text);
    }
}
