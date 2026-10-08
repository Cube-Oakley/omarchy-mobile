import QtQuick

// The under-display fingerprint sensor's mark and its feedback, drawn around
// the circle the panel lights while a finger is down (LHBM). That circle stays
// plain white: the sensor needs its light. phase: idle, pressed (scanning),
// matched or failed. The arcs are this style's animation; others can replace
// this item.
Item {
    id: ring
    // The lit circle's radius, in this item's pixels.
    property real radius: 60
    property string phase: "idle"
    property color tint: MobileTheme.accent
    readonly property color phaseColor: phase === "failed" ? MobileTheme.danger
        : phase === "matched" ? MobileTheme.success : tint

    width: radius * 3.4
    height: width
    transform: Translate { id: shake }

    // The mark, where the finger goes. Hidden under the finger while it scans.
    Text {
        anchors.centerIn: parent
        text: "󰈷"	// nf-md-fingerprint
        font.family: MobileTheme.fontFamily
        font.pixelSize: ring.radius * 1.5
        color: ring.phaseColor
        opacity: ring.phase === "pressed" ? 0 : 0.85
        Behavior on opacity { NumberAnimation { duration: 120 } }
        Behavior on color { ColorAnimation { duration: 150 } }
    }

    // Scanning: three arcs orbiting just outside the lit circle.
    Canvas {
        id: arcs
        anchors.fill: parent
        opacity: ring.phase === "pressed" ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 140 } }
        onPaint: {
            const ctx = getContext("2d");
            const c = width / 2, r = ring.radius * 1.32;
            ctx.reset();
            ctx.lineWidth = Math.max(3, ring.radius * 0.09);
            ctx.lineCap = "round";
            ctx.strokeStyle = ring.phaseColor;
            for (let i = 0; i < 3; i++) {
                const start = i * 2 * Math.PI / 3;
                ctx.beginPath();
                ctx.arc(c, c, r, start, start + Math.PI / 3);
                ctx.stroke();
            }
        }
        RotationAnimator on rotation {
            from: 0; to: 360; duration: 900
            loops: Animation.Infinite
            running: arcs.visible
        }
        Connections {
            target: ring
            function onPhaseColorChanged() { arcs.requestPaint(); }
            function onRadiusChanged() { arcs.requestPaint(); }
        }
    }

    // Result: a full ring that swells and fades, green or red.
    Rectangle {
        id: result
        anchors.centerIn: parent
        width: ring.radius * 2.64
        height: width
        radius: width / 2
        color: "transparent"
        border.width: Math.max(3, ring.radius * 0.09)
        border.color: ring.phaseColor
        opacity: 0
        scale: 1
    }
    ParallelAnimation {
        id: resultAnimation
        NumberAnimation { target: result; property: "opacity"; from: 1; to: 0; duration: 450; easing.type: Easing.OutCubic }
        NumberAnimation { target: result; property: "scale"; from: 1; to: 1.25; duration: 450; easing.type: Easing.OutCubic }
    }
    SequentialAnimation {
        id: shakeAnimation
        NumberAnimation { target: shake; property: "x"; to: -ring.radius * 0.18; duration: 50 }
        NumberAnimation { target: shake; property: "x"; to: ring.radius * 0.18; duration: 80 }
        NumberAnimation { target: shake; property: "x"; to: -ring.radius * 0.1; duration: 70 }
        NumberAnimation { target: shake; property: "x"; to: 0; duration: 60 }
    }
    onPhaseChanged: {
        if (phase === "matched" || phase === "failed") resultAnimation.restart();
        if (phase === "failed") shakeAnimation.restart();
    }
}
