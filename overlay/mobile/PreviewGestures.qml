import QtQuick

// Horizontal motion remains available to the parent ListView. Once an upward
// swipe or a hold is recognized, retain the touch until release or cancellation.
MouseArea {
    id: gesture
    property string mode: "tap"
    property point startPoint: Qt.point(0, 0)
    property real distance: 0
    property real velocity: 0
    property double lastMove: 0
    property real closeDistance: 160
    // Recent [time, distance] pairs: the release speed is taken over the
    // last 60 ms rather than from the final, noisy pair of events.
    property var samples: []
    signal opened()
    signal swipeMoved(real distance)
    // velocity: upward speed at release in scene pixels a second, 0 when the
    // finger had stopped.
    signal swipeFinished(bool close, real velocity)
    signal held(point position)
    signal holdMoved(point position)
    signal holdFinished(point position)
    signal gestureCanceled()
    pressAndHoldInterval: 450
    preventStealing: mode === "swipe" || mode === "hold"
    function scenePoint(mouse) { return mapToItem(null, mouse.x, mouse.y); }
    onPressed: mouse => {
        mode = "tap"; startPoint = scenePoint(mouse);
        distance = 0; velocity = 0; lastMove = Date.now(); samples = [];
    }
    onPressAndHold: mouse => {
        if (mode !== "tap") return;
        mode = "hold"; held(scenePoint(mouse));
    }
    onPositionChanged: mouse => {
        if (!pressed) return;
        const p = scenePoint(mouse);
        if (mode === "hold") { holdMoved(p); return; }
        const up = startPoint.y - p.y;
        const dx = Math.abs(p.x - startPoint.x);
        if (mode === "tap" && up > 14 && up > dx * 1.2) mode = "swipe";
        if (mode !== "swipe") return;
        const now = Date.now();
        velocity = (up - distance) * 1000 / Math.max(1, now - lastMove);
        lastMove = now; distance = up;
        samples = samples.filter(sample => now - sample[0] <= 60).concat([[now, up]]);
        swipeMoved(Math.max(0, up));
    }
    function releaseVelocity() {
        if (Date.now() - lastMove >= 100 || samples.length < 2) return 0;
        const first = samples[0], last = samples[samples.length - 1];
        if (last[0] - first[0] < 8) return velocity;
        return (last[1] - first[1]) * 1000 / (last[0] - first[0]);
    }
    onReleased: mouse => {
        // Reset after clicked is delivered, and before the next touch grab.
        Qt.callLater(() => { if (!pressed) mode = "tap"; });
        if (mode === "hold") holdFinished(scenePoint(mouse));
        else if (mode === "swipe") {
            const freshVelocity = Date.now() - lastMove < 100 ? velocity : 0;
            swipeFinished(distance >= closeDistance || (distance >= 54 && freshVelocity > 900),
                          Math.max(0, releaseVelocity()));
        }
    }
    onClicked: if (mode === "tap") opened()
    onCanceled: { mode = "canceled"; gestureCanceled(); }
}
