import QtQuick
import Qt.labs.folderlistmodel
import Quickshell
import Quickshell.Io
import OmarchyMobile
import OmarchyCamera

// Omarchy Camera. The engine (OmarchyCamera) uses libcamera's public API
// only, so this app carries to any phone libcamera supports.
ShellRoot {
    id: root
    property bool ready: false
    property bool allowed: false
    property var requests: []
    // Index in photos of the photo shown full screen, or -1.
    property int viewing: -1
    // Capture for another app (OMARCHY_MOBILE_APP_ARG "capture:PATH", as
    // the browser's file picker asks): photos only; each shot is shown with
    // Retake and Use photo, and Use copies it to PATH and closes the camera.
    // The photo also stays in the camera roll. Back without one cancels.
    readonly property string captureFor: {
        const arg = Quickshell.env("OMARCHY_MOBILE_APP_ARG") || "";
        return arg.startsWith("capture:") ? arg.slice(8) : "";
    }
    property string reviewing: ""
    readonly property string latest: camera.lastPhoto.length ? camera.lastPhoto
        : (photos.count > 0 ? String(photos.get(0, "filePath")) : "")
    // How far each known sensor zooms against the phone's main camera, for
    // the lens buttons; other sensors show their model instead.
    readonly property var zooms: ({
        "imx586": 1, "s5k3m5": 3, "imx481": 0.6,  // OnePlus 6
        "s5kgn1": 1, "s5kgm5": 5, "imx386": 0.5     // Pixel 7 Pro
    })
    // The cameras on the side in use, as lens buttons, widest first.
    readonly property var lenses: {
        const all = camera.cameras;
        const current = all[camera.cameraIndex];
        if (!current) return [];
        const list = [];
        for (let i = 0; i < all.length; i++) {
            if (all[i].location !== current.location) continue;
            const zoom = zooms[all[i].model];
            list.push({ index: i, zoom: zoom === undefined ? 1000 + i : zoom,
                        label: zoom === undefined ? all[i].model : zoom + "×" });
        }
        return list.length > 1 ? list.sort((a, b) => a.zoom - b.zoom) : [];
    }

    // One zoom across the lenses on this side: below 1× the ultrawide, 1-5×
    // the main camera, 5× up the telephoto. Between lenses the one in use
    // zooms digitally (the preview scaled, the photo cropped). A pinch that
    // stays past a lens's edge for a moment changes lens while it goes on;
    // a camera takes a moment to start, so the last picture stays, following
    // the pinch, until the new lens's frames come, then fades into them.
    // Video keeps whole lenses only.
    property real zoom: 1
    readonly property var zoomLenses: lenses.filter(lens => lens.zoom < 1000)
    readonly property real lensZoom: {
        const lens = zoomLenses.find(item => item.index === camera.cameraIndex);
        return lens ? lens.zoom : 1;
    }
    readonly property real minZoom: zoomLenses.length ? zoomLenses[0].zoom : 1
    readonly property real maxZoom: (zoomLenses.length ? zoomLenses[zoomLenses.length - 1].zoom : 1) * 4
    readonly property real digital: camera.video ? 1 : Math.max(1, zoom / lensZoom)
    function lensFor(value) {
        let pick = null;
        for (const lens of zoomLenses) if (value >= lens.zoom - 0.001) pick = lens;
        return pick || zoomLenses[0] || null;
    }
    function setZoom(value, switchLens) {
        zoom = Math.max(minZoom, Math.min(maxZoom, value));
        const lens = lensFor(zoom);
        if (switchLens && lens && lens.index !== camera.cameraIndex && !camera.recording) changeLens(lens.index);
    }

    // A lens change behind a still of the last picture.
    property bool frozen: false
    property real frozenZoom: 1
    property int pendingLens: -1
    property bool restarted: false
    property int newFrames: 0
    property double frozeAt: 0
    function changeLens(index) {
        if (!frozen) {
            still.scheduleUpdate();
            frozenZoom = zoom;
            frozeAt = Date.now();
            frozen = true;
            stillFade.stop();
            still.opacity = 1;
        }
        restarted = false;
        newFrames = 0;
        pendingLens = index;
        // After the still has been taken (a frame or two).
        lensDelay.restart();
    }
    function thaw() {
        if (!frozen || stillFade.running) return;
        console.log("CAMERA_LENS " + camera.cameraIndex + " in " + (Date.now() - frozeAt) + " ms" + (newFrames >= 2 ? "" : " (timed out)"));
        stillFade.start();
    }
    Timer { id: lensDelay; interval: 40; onTriggered: camera.cameraIndex = root.pendingLens }
    // Never held longer than this, whatever the camera does.
    Timer { interval: 2500; running: root.frozen; onTriggered: root.thaw() }
    Connections {
        target: camera
        enabled: root.frozen
        function onStateChanged() { if (camera.state !== "preview") root.restarted = true; }
        // The second frame of the new lens: its exposure has settled a little.
        function onFrameReady() { if (root.restarted && camera.state === "preview" && ++root.newFrames >= 2) root.thaw(); }
    }
    // While pinching, a lens past the edge takes over after a moment there.
    Timer {
        id: liveLens
        interval: 160
        onTriggered: {
            const lens = root.lensFor(root.zoom);
            if (lens && lens.index !== camera.cameraIndex && !camera.video && !camera.recording) root.changeLens(lens.index);
        }
    }
    function zoomLabel(value) { return (value < 10 ? value.toFixed(1) : Math.round(value)) + "×"; }

    function bin(name) { return Quickshell.env("HOME") + "/.local/bin/" + name; }
    function run(kind, args) {
        tool.kind = kind;
        tool.command = args;
        tool.running = true;
    }
    function grantNext() {
        const next = root.requests.find(item => !item.granted);
        if (next) run("grant", [bin("omarchy-mobile-app"), "grant", "camera", next.id]);
        else root.allowed = true;
    }
    Component.onCompleted: run("status", [bin("omarchy-mobile-app"), "status", "camera"])

    Process {
        id: tool
        property string kind: ""
        stdout: StdioCollector {}
        onExited: {
            let result = {};
            try { result = JSON.parse(stdout.text); } catch (e) { return; }
            if (tool.kind === "status") {
                const items = result.permissions || [];
                let granted = items.length > 0;
                for (let i = 0; i < items.length; i++) if (!items[i].granted) granted = false;
                root.requests = items;
                root.allowed = granted;
                root.ready = true;
            } else if (tool.kind === "grant") {
                // One permission per call: grant the next until none is left.
                root.requests = result.permissions || [];
                root.grantNext();
            }
        }
    }

    CameraSession {
        id: camera
        // Only while this app is in front: in the background the sensor, the
        // image processor and the GPU would keep working for nothing.
        active: root.allowed && back.focused()
        digitalZoom: root.digital
        // Another side (front, back) starts at its own 1×.
        onCameraChanged: if (root.zoomLenses.length && !root.zoomLenses.some(lens => lens.index === camera.cameraIndex)) root.zoom = 1
        onPhotoSaved: path => { if (root.captureFor) root.reviewing = path; }
        onPhotoFailed: reason => status.show(reason)
        onVideoFailed: reason => status.show(reason)
        onVideoSaved: status.show("Video saved")
        onRecordingChanged: if (recording) recordStart = Date.now()
        property real recordStart: 0
    }

    // Seconds recorded, for the counter.
    property int recorded: 0
    Timer {
        interval: 250
        repeat: true
        running: camera.recording
        triggeredOnStart: true
        onTriggered: root.recorded = Math.floor((Date.now() - camera.recordStart) / 1000)
    }

    // qs ipc call camera capture, or focus 0.5 0.3: for tests and scripts.
    IpcHandler {
        target: "camera"
        function capture(): void { camera.capture(); }
        function focus(x: real, y: real): void { camera.focusAt(x, y); }
        function state(): string { return camera.state + " " + camera.focusState; }
        function lens(index: int): void { camera.cameraIndex = index; }
        function zoom(value: real): string { root.setZoom(value, true); return root.zoom + " " + root.digital + " " + camera.cameraIndex; }
        function cameras(): string { return camera.cameraIndex + " " + JSON.stringify(camera.cameras); }
        function video(on: bool): void { camera.video = on; }
        function record(): void { camera.startRecording(); }
        function stop(): string { camera.stopRecording(); return camera.lastVideo; }
        function recording(): string {
            return camera.canRecord + " " + camera.video + " " + camera.recording + " " + camera.lastVideo;
        }
    }

    FolderListModel {
        id: photos
        folder: "file://" + camera.photoFolder
        nameFilters: ["*.jpg", "*.jpeg", "*.JPG", "*.JPEG"]
        sortField: FolderListModel.Time
        showDirs: false
    }

    FloatingWindow {
        id: win
        title: "Camera"
        implicitWidth: 480
        implicitHeight: 1040
        color: MobileTheme.background
        onClosed: Qt.quit()
        onBackingWindowVisibleChanged: if (!backingWindowVisible) Qt.quit()

        AppBack {
            id: back
            width: 0
            height: 0
            windowTitle: "Camera"
            onBack: {
                if (root.reviewing) root.reviewing = "";
                else if (root.viewing >= 0) root.viewing = -1;
                else Qt.quit();
            }
        }

        GrantPage {
            anchors.fill: parent
            anchors.margins: 24
            visible: root.ready && !root.allowed
            requests: root.requests
            onAllowed: root.grantNext()
            onDenied: Qt.quit()
        }

        Item {
            id: stage
            anchors.fill: parent
            visible: root.allowed

            // The sensor is 4:3, and video 16:9; upright on a portrait
            // screen that is 3:4 or 9:16.
            Rectangle {
                id: frame
                width: parent.width
                height: Math.min(Math.round(width * (camera.video ? 16 / 9 : 4 / 3)),
                                 parent.height - controls.height)
                y: Math.max(0, Math.round((parent.height - height - controls.height) / 2))
                clip: true
                color: "black"

                Item {
                    id: lensView
                    anchors.fill: parent
                    // Hidden behind the still while the lens changes.
                    opacity: root.frozen && !stillFade.running ? 0 : 1
                    Viewfinder {
                        session: camera
                        anchors.centerIn: parent
                        width: camera.rotation % 180 ? parent.height : parent.width
                        height: camera.rotation % 180 ? parent.width : parent.height
                        rotation: camera.rotation
                        scale: root.digital
                    }
                }
                // The last picture before a lens change, zoomed with the pinch.
                ShaderEffectSource {
                    id: still
                    anchors.fill: parent
                    sourceItem: lensView
                    live: false
                    visible: root.frozen
                    scale: root.zoom / root.frozenZoom
                    NumberAnimation {
                        id: stillFade
                        target: still; property: "opacity"; from: 1; to: 0
                        duration: 180; easing.type: Easing.OutQuad
                        onFinished: { root.frozen = false; still.opacity = 1; }
                    }
                }

                PinchHandler {
                    id: pinch
                    target: null
                    enabled: camera.state === "preview" && !camera.recording
                    property real startZoom: 1
                    onActiveChanged: {
                        if (active) { startZoom = root.zoom; return; }
                        liveLens.stop();
                        root.setZoom(root.zoom, true);
                    }
                    onActiveScaleChanged: {
                        if (!active) return;
                        root.setZoom(startZoom * activeScale, false);
                        const lens = root.lensFor(root.zoom);
                        if (lens && lens.index !== (root.frozen ? root.pendingLens : camera.cameraIndex) && !camera.video) {
                            if (!liveLens.running) liveLens.start();
                        } else {
                            liveLens.stop();
                        }
                    }
                }

                Rectangle {
                    id: flash
                    anchors.fill: parent
                    color: "white"
                    opacity: 0
                    NumberAnimation on opacity { id: flashAnim; from: 0.7; to: 0; duration: 220; running: false }
                }

                // Theme accent once focused; dimmed if focus failed.
                Rectangle {
                    id: ring
                    readonly property color tint: camera.focusState === "focused" ? MobileTheme.accent
                        : camera.focusState === "failed" ? MobileTheme.muted : "white"
                    width: 76
                    height: 76
                    radius: 38
                    color: "transparent"
                    border.width: 2
                    border.color: tint
                    visible: camera.focusPoint.x >= 0
                    x: camera.focusPoint.x * frame.width - width / 2
                    y: camera.focusPoint.y * frame.height - height / 2
                    scale: 1
                    NumberAnimation on scale { id: ringAnim; from: 1.35; to: 1; duration: 180; running: false }
                }

                TapHandler {
                    enabled: camera.state === "preview" && camera.canFocus
                    onTapped: point => {
                        // Through the digital zoom, to where that is on the sensor.
                        camera.focusAt(0.5 + (point.position.x / frame.width - 0.5) / root.digital,
                                       0.5 + (point.position.y / frame.height - 0.5) / root.digital);
                        ringAnim.restart();
                    }
                    onLongPressed: camera.resetFocus()
                }

                // One button per camera on this side: 1×, 3×.
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 16
                    spacing: 12
                    visible: root.lenses.length > 0 && !camera.recording
                    Repeater {
                        model: root.lenses
                        Rectangle {
                            required property var modelData
                            readonly property bool current: modelData.index === camera.cameraIndex
                            width: Math.max(44, label.implicitWidth + 20)
                            height: 44
                            radius: 22
                            color: Qt.rgba(0, 0, 0, 0.45)
                            border.width: current ? 2 : 0
                            border.color: MobileTheme.accent
                            Text {
                                id: label
                                anchors.centerIn: parent
                                // The lens in use shows the zoom while it differs from the lens's own.
                                text: parent.current && parent.modelData.zoom < 1000 && Math.abs(root.zoom - parent.modelData.zoom) > 0.05
                                      ? root.zoomLabel(root.zoom) : parent.modelData.label
                                color: parent.current ? MobileTheme.accent : "white"
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 15
                                font.bold: parent.current
                            }
                            // Takes the press, so the tap does not also focus.
                            MouseArea {
                                anchors.fill: parent
                                onClicked: {
                                    if (parent.modelData.zoom < 1000) root.setZoom(parent.modelData.zoom, true);
                                    else camera.cameraIndex = parent.modelData.index;
                                }
                            }
                        }
                    }
                }

                // Time recorded, while recording.
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.topMargin: 16
                    width: elapsed.implicitWidth + 36
                    height: 32
                    radius: 16
                    color: Qt.rgba(0, 0, 0, 0.45)
                    visible: camera.recording
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        x: 12
                        width: 8
                        height: 8
                        radius: 4
                        color: "#e53935"
                    }
                    Text {
                        id: elapsed
                        anchors.verticalCenter: parent.verticalCenter
                        x: 26
                        text: Math.floor(root.recorded / 60) + ":" + String(root.recorded % 60).padStart(2, "0")
                        color: "white"
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 15
                    }
                }

                Text {
                    anchors.centerIn: parent
                    width: parent.width - 48
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    visible: camera.state === "starting" || camera.state === "error"
                    text: camera.state === "error" ? camera.error : "Starting the camera"
                    color: MobileTheme.foreground
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 16
                }
            }

            Item {
                id: controls
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 180

                // The most recent photo; tap to look at it.
                Rectangle {
                    id: thumb
                    width: 60
                    height: 60
                    radius: MobileTheme.radius(12)
                    anchors.verticalCenter: shutter.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 40
                    color: MobileTheme.surface
                    border.width: 2
                    border.color: MobileTheme.accent
                    clip: true
                    visible: root.latest.length > 0 && !root.captureFor
                    Image {
                        anchors.fill: parent
                        anchors.margins: 2
                        source: root.latest.length ? "file://" + root.latest : ""
                        sourceSize: Qt.size(128, 128)
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: false
                    }
                    // photos lists the newest first.
                    TapHandler { onTapped: root.viewing = 0 }
                }

                // Photo or video, above the shutter.
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: shutter.top
                    anchors.bottomMargin: 14
                    spacing: 28
                    visible: camera.canRecord && !camera.recording && !root.captureFor
                    Repeater {
                        model: [{ label: "Photo", video: false }, { label: "Video", video: true }]
                        Text {
                            required property var modelData
                            readonly property bool current: camera.video === modelData.video
                            text: modelData.label
                            color: current ? MobileTheme.accent : MobileTheme.secondary
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 15
                            font.bold: current
                            TapHandler {
                                enabled: camera.state === "preview"
                                onTapped: camera.video = parent.modelData.video
                            }
                        }
                    }
                }

                Rectangle {
                    id: shutter
                    readonly property bool ready: camera.state === "preview"
                        && (camera.video || camera.pending < 2)
                    anchors.centerIn: parent
                    width: 84
                    height: 84
                    radius: 42
                    color: "transparent"
                    border.width: 4
                    border.color: MobileTheme.accent
                    opacity: ready ? 1 : 0.5
                    // White for a photo, red to record, a red square to stop.
                    Rectangle {
                        anchors.centerIn: parent
                        width: camera.recording ? 34 : press.pressed ? 60 : 68
                        height: width
                        radius: camera.recording ? 6 : width / 2
                        color: camera.video ? "#e53935"
                            : press.pressed ? MobileTheme.accent : MobileTheme.foreground
                        Behavior on width { NumberAnimation { duration: 80 } }
                    }
                    TapHandler {
                        id: press
                        enabled: shutter.ready
                        onTapped: {
                            if (camera.recording) {
                                camera.stopRecording();
                            } else if (camera.video) {
                                camera.startRecording();
                            } else {
                                flashAnim.restart();
                                camera.capture();
                            }
                        }
                    }
                }

                Text {
                    id: status
                    function show(message) { text = message; hide.restart(); }
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: shutter.bottom
                    anchors.topMargin: 16
                    text: camera.state === "capturing" ? "Taking the photo"
                        : camera.pending > 0 ? "Processing" : ""
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 14
                    Timer { id: hide; interval: 3000; onTriggered: status.text = Qt.binding(() => camera.state === "capturing" ? "Taking the photo" : camera.pending > 0 ? "Processing" : "") }
                }
            }
        }

        // Photos full screen, newest first; swipe between them. The camera
        // keeps running underneath, so going back is instant.
        Rectangle {
            id: viewer
            anchors.fill: parent
            color: MobileTheme.background
            visible: root.viewing >= 0
            onVisibleChanged: if (visible) gallery.positionViewAtIndex(root.viewing, ListView.Beginning)
            ListView {
                id: gallery
                anchors.fill: parent
                orientation: ListView.Horizontal
                snapMode: ListView.SnapOneItem
                highlightRangeMode: ListView.StrictlyEnforceRange
                boundsBehavior: Flickable.StopAtBounds
                cacheBuffer: width
                model: photos
                delegate: Item {
                    required property url fileUrl
                    width: gallery.width
                    height: gallery.height
                    Image {
                        anchors.fill: parent
                        source: parent.fileUrl
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        cache: false
                        sourceSize: Qt.size(win.width * 2, win.height * 2)
                    }
                }
            }
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 32
                width: count.implicitWidth + 28
                height: 32
                radius: MobileTheme.radius(16)
                color: MobileTheme.surface
                border.width: 1
                border.color: MobileTheme.muted
                Text {
                    id: count
                    anchors.centerIn: parent
                    text: (gallery.currentIndex + 1) + " / " + photos.count
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 14
                }
            }
            // The photo in view, through the share sheet.
            Rectangle {
                id: shareButton
                anchors { right: parent.right; bottom: parent.bottom; rightMargin: 24; bottomMargin: 24 }
                width: 52
                height: 52
                radius: 26
                color: shareTap.pressed ? MobileTheme.selection : MobileTheme.surface
                border.width: 1
                border.color: MobileTheme.muted
                Text {
                    anchors.centerIn: parent
                    text: "\u{F0497}"
                    color: MobileTheme.foreground
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 24
                }
                TapHandler {
                    id: shareTap
                    onTapped: Quickshell.execDetached([root.bin("omarchy-mobile-share"), "--file",
                                                       String(photos.get(gallery.currentIndex, "filePath"))])
                }
            }
            // A tap elsewhere closes the photo (the share button's taps reach
            // this handler too).
            TapHandler {
                onTapped: point => {
                    if (!shareButton.contains(shareButton.mapFromItem(viewer, point.position))) root.viewing = -1;
                }
            }
        }

        // The shot for the app that asked (capture mode): Retake or Use photo.
        Rectangle {
            anchors.fill: parent
            visible: root.reviewing !== ""
            color: "black"
            Image {
                anchors { left: parent.left; right: parent.right; top: parent.top; bottom: reviewBar.top }
                source: root.reviewing ? "file://" + root.reviewing : ""
                sourceSize: Qt.size(1440, 1920)
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                cache: false
            }
            TapHandler {}
            Row {
                id: reviewBar
                anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 48 }
                spacing: 16
                TouchButton { width: 170; label: "Retake"; onClicked: root.reviewing = "" }
                TouchButton {
                    width: 170
                    label: handOver.running ? "Saving…" : "Use photo"
                    selected: true
                    enabled: !handOver.running
                    onClicked: handOver.running = true
                }
            }
            Process {
                id: handOver
                command: ["cp", "--", root.reviewing, root.captureFor]
                onExited: code => { if (code === 0) Qt.quit(); else status.show("The photo could not be handed over"); }
            }
        }
    }
}
