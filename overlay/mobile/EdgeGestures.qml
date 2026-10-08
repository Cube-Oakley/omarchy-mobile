import QtQuick

// One bottom swipe. The shell decides home, launcher, or switcher.
// A pause after the swipe has started arms the switcher, matching Android's
// swipe-up-and-hold. A flick that never pauses does not.
Item {
    id: edge
    property color handleColor: "white"
    signal started()
    signal moved(real distance, real velocity)
    signal held()
    signal released(real distance, real velocity, bool held)
    signal canceled()
    signal tapped()
    property bool tracking: false
    readonly property bool dragActive: drag.active
    // When the drag last moved: one still for long lost its lift (the
    // shell's stuck check).
    property double lastMove: 0
    property bool armed: false
    property real distance: 0
    property real velocity: 0
    property real armAnchor: 0

    DragHandler {
        id: drag
        target: null
        minimumPointCount: 1
        maximumPointCount: 1
        onActiveChanged: {
            edge.lastMove = Date.now();
            if (active) {
                edge.tracking = false;
                edge.armed = false;
                edge.distance = 0;
                hold.stop();
                return;
            }
            if (!edge.tracking) return;
            hold.stop();
            const held = edge.armed;
            edge.tracking = false;
            edge.released(edge.distance, edge.velocity, held);
        }
        onActiveTranslationChanged: {
            if (!active) return;
            edge.lastMove = Date.now();
            const up = -activeTranslation.y;
            edge.velocity = -centroid.velocity.y;
            if (!edge.tracking) {
                if (up < 10 || up < Math.abs(activeTranslation.x) * 1.15) return;
                edge.tracking = true;
                edge.started();
            }
            edge.distance = Math.max(0, up);
            // Arm only after the finger pauses. Touchscreens keep reporting
            // small jitter while a finger is held, so that must not reset the timer.
            if (up > 72 && !edge.armed) {
                if (!hold.running) {
                    edge.armAnchor = up;
                    hold.start();
                } else if (Math.abs(up - edge.armAnchor) > 36) {
                    edge.armAnchor = up;
                    hold.restart();
                }
            }
            edge.moved(edge.distance, edge.velocity);
        }
        onCanceled: {
            hold.stop();
            edge.tracking = false;
            edge.armed = false;
            edge.canceled();
        }
    }
    Timer {
        id: hold
        interval: 180
        onTriggered: {
            if (!edge.tracking || edge.armed) return;
            edge.armed = true;
            edge.held();
        }
    }
    TapHandler { onTapped: edge.tapped() }
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 6
        width: drag.active ? 84 : 64
        height: 4
        radius: 2
        color: edge.handleColor
        opacity: drag.active ? 0.9 : 0.35
        Behavior on width { NumberAnimation { duration: 120 } }
        Behavior on opacity { NumberAnimation { duration: 120 } }
    }
}
