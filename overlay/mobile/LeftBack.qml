import QtQuick

// Swipe from the left edge (or with fromRight, the right edge) toward the center.
// The shell decides what back means.
Item {
    id: edge
    signal committed()
    property bool fromRight: false
    property color pill: "#24283b"
    property color ink: "#c0caf5"
    property real distance: 0
    property real velocity: 0
    property bool pressed: false
    property bool tracking: false
    // A finger is down on the strip, before any drag is recognized: the shell
    // widens the touch region at once (the grab dies at the region's edge),
    // so the resting strip can stay narrow and taps beside it reach the app.
    readonly property bool down: point.active

    PointHandler { id: point }

    DragHandler {
        id: drag
        target: null
        minimumPointCount: 1
        maximumPointCount: 1
        onActiveChanged: {
            if (active) {
                // Grow the touch region before the finger moves. Waiting until
                // the swipe has already traveled lets the grab die at the edge.
                edge.pressed = true;
                edge.tracking = false;
                edge.distance = 0;
                edge.velocity = 0;
                return;
            }
            edge.pressed = false;
            const travel = edge.distance;
            const speed = edge.velocity;
            edge.tracking = false;
            edge.distance = 0;
            if (travel > 48 || (travel > 24 && speed > 600)) edge.committed();
        }
        onActiveTranslationChanged: {
            if (!active) return;
            const across = edge.fromRight ? -activeTranslation.x : activeTranslation.x;
            if (!edge.tracking) {
                if (across < 14 || across < Math.abs(activeTranslation.y) * 1.2) return;
                edge.tracking = true;
            }
            edge.distance = Math.max(0, across);
            edge.velocity = edge.fromRight ? -centroid.velocity.x : centroid.velocity.x;
        }
    }

    Rectangle {
        width: 36
        height: 56
        radius: 18
        readonly property real inset: Math.min(edge.distance, 88) - 28
        x: edge.fromRight ? edge.width - width - inset : inset
        anchors.verticalCenter: parent.verticalCenter
        color: edge.pill
        border.width: 1
        border.color: edge.ink
        opacity: Math.min(1, edge.distance / 28)
        visible: edge.distance > 6
        Text {
            anchors.centerIn: parent
            text: "‹"
            color: edge.ink
            font.pixelSize: 30
            font.bold: true
        }
    }
}
