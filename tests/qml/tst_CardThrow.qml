import QtQuick
import QtTest
import "."
TestCase {
    id: test; name: "CardThrow"
    CardThrow { id: cardThrow }
    // Pixels a second at the start of the curve.
    function startSpeed(motion, distance) {
        return motion.bezier[1] / motion.bezier[0] * distance / motion.duration * 1000;
    }
    function monotonic(motion) {
        const b = motion.bezier;
        return b[1] >= 0 && b[1] <= 1 && b[3] >= 0 && b[3] <= 1 && b[4] === 1 && b[5] === 1;
    }
    function test_flick_leaves_at_finger_speed() {
        const motion = cardThrow.curve(570, 2500);
        fuzzyCompare(startSpeed(motion, 570), 2500, 25);
        verify(motion.duration >= 150 && motion.duration <= 320);
        verify(monotonic(motion));
    }
    function test_stopped_finger_starts_from_rest_and_accelerates() {
        const motion = cardThrow.curve(570, 0);
        compare(motion.duration, 320);
        compare(startSpeed(motion, 570), 0);
        verify(monotonic(motion));
    }
    function test_slow_release_keeps_its_speed() {
        const motion = cardThrow.curve(570, 300);
        compare(motion.duration, 320);
        fuzzyCompare(startSpeed(motion, 570), 300, 5);
        verify(monotonic(motion));
    }
    function test_fast_flick_is_short_and_bounded() {
        const motion = cardThrow.curve(570, 20000);
        compare(motion.duration, 150);
        verify(startSpeed(motion, 570) > 9000);
        verify(monotonic(motion));
    }
}
