import QtQuick

Item {
    id: motion
    property real progress: 0
    property real travel: 1
    property bool dragging: false
    property real startProgress: 0
    property real lastDistance: 0
    property real lastVelocity: 0
    property double lastMotionTime: 0
    readonly property bool settledOpen: progress > 0.999 && !dragging && !settle.running
    readonly property bool moving: dragging || settle.running
    signal closed()
    function begin() {
        settle.stop(); dragging = true;
        startProgress = progress; lastDistance = 0; lastVelocity = 0;
        lastMotionTime = Date.now();
    }
    function update(distance, velocity) {
        if (!dragging) return;
        lastDistance = distance; lastVelocity = velocity;
        lastMotionTime = Date.now();
        progress = Math.max(0, Math.min(1, startProgress + distance / Math.max(1, travel)));
    }
    function finish() {
        if (!dragging) return;
        dragging = false;
        const speed = Date.now() - lastMotionTime < 100 ? lastVelocity : 0;
        const fling = Math.abs(lastDistance) >= 40 && Math.abs(speed) >= 700;
        animateTo(fling ? (speed > 0 ? 1 : 0) : (progress >= 0.4 ? 1 : 0));
    }
    function finishClose() {
        if (!dragging) return;
        const speed = Date.now() - lastMotionTime < 100 ? lastVelocity : 0;
        const fling = Math.abs(lastDistance) >= 40 && Math.abs(speed) >= 700;
        // A lower-screen pull has less travel available than an edge opening.
        const farEnough = lastDistance <= -Math.min(160, travel * 0.2);
        animateTo(fling ? (speed < 0 ? 0 : 1) : (farEnough ? 0 : 1));
    }
    function cancel() { dragging = false; animateTo(startProgress >= 0.5 ? 1 : 0); }
    function dismiss() {
        settle.stop();
        dragging = false;
        progress = 0;
        closed();
    }
    function animateTo(value) {
        settle.stop();
        dragging = false;
        if (Math.abs(progress - value) < 0.001) {
            progress = value;
            if (value === 0) closed();
            return;
        }
        settle.to = value;
        settle.duration = Math.round(90 + 120 * Math.abs(progress - value));
        settle.start();
    }
    NumberAnimation {
        id: settle; target: motion; property: "progress"; easing.type: Easing.OutCubic
        onFinished: if (motion.progress < 0.001) motion.closed()
    }
}
