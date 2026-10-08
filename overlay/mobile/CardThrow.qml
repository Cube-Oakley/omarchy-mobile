import QtQml

// The motion of a card thrown off the switcher. It leaves at the finger's
// release speed: starting from rest (InCubic) stalled the card for about
// 80 ms after a quick flick before it accelerated away.
QtObject {
    // distance: pixels left to travel; velocity: release speed in pixels a
    // second (0 when the finger had stopped). The curve is a cubic in time
    // whose starting slope is the release speed; a slow release speeds up
    // toward the end, a fast one ends a little slower.
    function curve(distance, velocity) {
        const span = Math.max(1, distance);
        const speed = Math.max(0, velocity || 0) / 1000;
        const duration = Math.max(150, Math.min(320, span / Math.max(speed, 0.001)));
        const start = Math.min(2.5, speed * duration / span);
        const end = Math.max(0.5, 3 - start);
        return {duration: Math.round(duration), bezier: [1 / 3, start / 3, 2 / 3, 1 - end / 3, 1, 1]};
    }
}
