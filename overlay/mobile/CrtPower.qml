import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Wayland

// Electron-beam screen power, the AOSP/Lineage CRT: the picture squashes into
// a bright line, the line retracts to the center, and wake plays that in reverse.
// The panel itself is switched by omarchy-mobile-display after close / before open.
PanelWindow {
    id: crt
    property string mode: ""
    property string token: ""
    property real progress: 0
    property bool playing: false
    property double wakeAt: 0
    property bool capturing: false
    // Frames drawn since the open asked for the panel; -1 when not waiting.
    property int litFrames: -1
    readonly property string shot: Quickshell.env("XDG_RUNTIME_DIR") + "/omarchy-mobile-crt.jpg"
    readonly property var phase: phases.sample(mode === "on" ? "on" : "off", progress)
    readonly property bool veil: playing && (mode === "on" || mode === "off")
    signal finished(string token)
    // An open has played: the screen is lit again.
    signal opened()

    anchors { top: true; bottom: true; left: true; right: true }
    exclusionMode: ExclusionMode.Ignore
    exclusiveZone: 0
    visible: mode !== ""
    color: playing && (mode === "on" || mode === "off") ? "transparent" : "black"
    mask: Region { width: crt.mode === "" ? 0 : crt.width; height: crt.mode === "" ? 0 : crt.height }
    WlrLayershell.namespace: "omarchy-mobile-crt"
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.keyboardFocus: WlrKeyboardFocus.None

    CrtPhases { id: phases }
    property var markQueue: []
    function mark(kind, id) {
        markQueue = markQueue.concat([{kind: kind, token: id}]);
        flushMark();
    }
    function flushMark() {
        if (markProc.running || markQueue.length === 0) return;
        const next = markQueue[0];
        markQueue = markQueue.slice(1);
        markProc.command = ["sh", "-c", "printf '%s %s\\n' \"$1\" \"$2\" > \"$XDG_RUNTIME_DIR/omarchy-mobile-crt.state\"", "crt-mark", next.kind, next.token];
        markProc.running = true;
    }
    Process {
        id: markProc
        onExited: crt.flushMark()
    }
    Process {
        id: capture
        onExited: (code, status) => {
            crt.capturing = false;
            if (code !== 0) {
                frame.source = "";
                crt.begin();
                return;
            }
            frame.source = "";
            frame.source = "file://" + crt.shot;
            captureWait.ticks = 0;
            captureWait.start();
        }
    }
    Process {
        id: clearShot
        command: ["rm", "-f", crt.shot]
    }
    Timer {
        id: captureWait
        interval: 30
        repeat: true
        property int ticks: 0
        onTriggered: {
            ticks++;
            if (frame.status === Image.Ready || frame.status === Image.Error || ticks > 12)
                crt.begin();
        }
    }
    function cover(id) {
        anim.stop();
        litFrames = -1;
        litWait.stop();
        captureWait.stop();
        capturing = false;
        playing = false;
        progress = 0;
        frame.source = "";
        mode = "parked";
        token = id;
        mark("covered", id);
        console.log("MOBILE_CRT covered");
    }
    function play(which, id) {
        const now = Date.now();
        // Wake asks twice, once from suspend cleanup and once from the power
        // key. The second request must settle so the caller returns, without
        // starting the open animation again.
        if (which === "on" && wakeAt > 0 && now - wakeAt < 2500) {
            mark("settled", id);
            console.log("MOBILE_CRT skip duplicate on");
            return;
        }
        if (playing) return;
        litFrames = -1;
        litWait.stop();
        token = id;
        progress = 0;
        mode = which === "on" ? "on" : "off";
        if (which === "on") {
            wakeAt = now;
            Quickshell.execDetached(["hyprctl", "eval", "hl.dispatch(hl.dsp.dpms({ action = \"enable\" }))"]);
            // The panel takes a few hundred milliseconds to light, longer
            // after a suspend than the whole open. The compositor draws no
            // frames for a dark output, so the open starts on the second
            // frame after the enable (or after 1.5 s).
            litFrames = 0;
            litWait.restart();
            return;
        }
        begin();
    }
    function lit() {
        if (litFrames < 0) return;
        litFrames = -1;
        litWait.stop();
        begin();
    }
    Timer { id: litWait; interval: 1500; onTriggered: crt.lit() }
    // Keeps frames coming while the open waits, so their arrival shows the panel is lit.
    Item {
        id: litProbe
        width: 1; height: 1; opacity: 0
        NumberAnimation on rotation { from: 0; to: 360; duration: 1000; loops: Animation.Infinite; running: crt.litFrames >= 0 }
    }
    Connections {
        target: crt.litFrames >= 0 ? litProbe.Window.window : null
        function onFrameSwapped() {
            if (++crt.litFrames >= 2) crt.lit();
        }
    }
    function begin() {
        captureWait.stop();
        capturing = false;
        if (token === "") return;
        playing = true;
        if (mode !== "on") mode = "off";
        progress = 0;
        mark("playing", token);
        anim.duration = mode === "on" ? 420 : 520;
        Qt.callLater(() => anim.restart());
        console.log("MOBILE_CRT " + mode);
    }
    function complete() {
        if (!playing) return;
        playing = false;
        const id = token;
        const opening = mode === "on";
        if (mode === "off") {
            frame.source = "";
            clearShot.running = true;
            mode = "parked";
        } else {
            mode = "";
        }
        progress = 0;
        mark("settled", id);
        finished(id);
        if (opening) opened();
        console.log("MOBILE_CRT settled " + id);
    }
    NumberAnimation {
        id: anim
        target: crt
        property: "progress"
        from: 0
        to: 1
        easing.type: Easing.Linear
        onFinished: if (crt.progress > 0.98) crt.complete()
    }

    Item {
        id: frameHost
        anchors.fill: parent
        visible: frame.source !== ""
        transform: Scale {
            origin.x: frameHost.width / 2
            origin.y: frameHost.height / 2
            xScale: Math.max(0.001, crt.phase.collapse * (0.9 + 0.1 * crt.phase.stretch))
            yScale: Math.max(0.001, crt.phase.stretch)
        }
        Image {
            id: frame
            anchors.fill: parent
            cache: false
            asynchronous: true
            fillMode: Image.Stretch
            smooth: true
        }
        Rectangle {
            anchors.fill: parent
            color: "white"
            opacity: (1 - crt.phase.stretch) * 0.34
        }
    }
    Item {
        anchors.fill: parent
        visible: crt.veil
        Rectangle {
            anchors.top: parent.top
            width: parent.width
            height: Math.max(0, (parent.height - parent.height * crt.phase.stretch) / 2)
            color: "black"
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: Math.max(0, (parent.height - parent.height * crt.phase.stretch) / 2)
            color: "black"
        }
        Rectangle {
            anchors.left: parent.left
            width: Math.max(0, (parent.width - parent.width * crt.phase.collapse) / 2)
            height: parent.height
            color: "black"
        }
        Rectangle {
            anchors.right: parent.right
            width: Math.max(0, (parent.width - parent.width * crt.phase.collapse) / 2)
            height: parent.height
            color: "black"
        }
    }
    Item {
        anchors.centerIn: parent
        width: parent.width * Math.max(crt.phase.collapse, 0.008)
        height: 18
        visible: crt.playing && crt.phase.beam > 0.04
        opacity: crt.phase.beam
        Rectangle {
            anchors.centerIn: parent
            width: parent.width
            height: 16
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#00000000" }
                GradientStop { position: 0.5; color: "#d8fff8" }
                GradientStop { position: 1.0; color: "#00000000" }
            }
        }
        Rectangle {
            anchors.centerIn: parent
            width: parent.width
            height: 2
            color: "white"
        }
    }
    Rectangle {
        anchors.centerIn: parent
        width: 8
        height: 3
        radius: 1
        color: "white"
        visible: crt.mode === "off" && crt.progress > 0.9
        opacity: crt.progress > 0.9 ? 1 - (crt.progress - 0.9) / 0.1 : 0
    }
}
