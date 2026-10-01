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

    function bin(name) { return Quickshell.env("HOME") + "/.local/bin/" + name; }
    function run(kind, args) {
        tool.kind = kind;
        tool.command = args;
        tool.running = true;
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
                root.allowed = true;
            }
        }
    }

    CameraSession {
        id: camera
        // Only while this app is in front: in the background the sensor, the
        // image processor and the GPU would keep working for nothing.
        active: root.allowed && back.focused()
        onPhotoFailed: reason => status.show(reason)
    }

    // qs ipc call camera capture, or focus 0.5 0.3: for tests and scripts.
    IpcHandler {
        target: "camera"
        function capture(): void { camera.capture(); }
        function focus(x: real, y: real): void { camera.focusAt(x, y); }
        function state(): string { return camera.state + " " + camera.focusState; }
        function lens(index: int): void { camera.cameraIndex = index; }
        function cameras(): string { return camera.cameraIndex + " " + JSON.stringify(camera.cameras); }
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
                if (root.viewing >= 0) root.viewing = -1;
                else Qt.quit();
            }
        }

        GrantPage {
            anchors.fill: parent
            anchors.margins: 24
            visible: root.ready && !root.allowed
            requests: root.requests
            onAllowed: root.run("grant", [root.bin("omarchy-mobile-app"), "grant", "camera", "camera"])
            onDenied: Qt.quit()
        }

        Item {
            id: stage
            anchors.fill: parent
            visible: root.allowed

            // The sensor is 4:3; upright on a portrait screen that is 3:4.
            Rectangle {
                id: frame
                width: parent.width
                height: Math.round(width * 4 / 3)
                y: Math.max(0, Math.round((parent.height - height - controls.height) / 2))
                clip: true
                color: "black"

                Viewfinder {
                    session: camera
                    anchors.centerIn: parent
                    width: camera.rotation % 180 ? parent.height : parent.width
                    height: camera.rotation % 180 ? parent.width : parent.height
                    rotation: camera.rotation
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
                        camera.focusAt(point.position.x / frame.width, point.position.y / frame.height);
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
                    visible: root.lenses.length > 0
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
                                text: parent.modelData.label
                                color: parent.current ? MobileTheme.accent : "white"
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 15
                                font.bold: parent.current
                            }
                            // Takes the press, so the tap does not also focus.
                            MouseArea {
                                anchors.fill: parent
                                onClicked: camera.cameraIndex = parent.modelData.index
                            }
                        }
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
                    visible: root.latest.length > 0
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

                Rectangle {
                    id: shutter
                    anchors.centerIn: parent
                    width: 84
                    height: 84
                    radius: 42
                    color: "transparent"
                    border.width: 4
                    border.color: MobileTheme.accent
                    opacity: camera.state === "preview" && camera.pending < 2 ? 1 : 0.5
                    Rectangle {
                        anchors.centerIn: parent
                        width: press.pressed ? 60 : 68
                        height: width
                        radius: width / 2
                        color: press.pressed ? MobileTheme.accent : MobileTheme.foreground
                        Behavior on width { NumberAnimation { duration: 80 } }
                    }
                    TapHandler {
                        id: press
                        enabled: camera.state === "preview" && camera.pending < 2
                        onTapped: {
                            flashAnim.restart();
                            camera.capture();
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
            TapHandler { onTapped: root.viewing = -1 }
        }
    }
}
