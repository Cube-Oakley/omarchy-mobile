import QtQuick
import QtTest
import "."

TestCase {
    id: test
    name: "TouchMotion"
    when: windowShown
    visible: true
    width: 480; height: 800
    DrawerMotion { id: motion; travel: 1000 }
    SignalSpy { id: closed; target: motion; signalName: "closed" }
    PreviewGestures { id: preview; x: 10; y: 10; width: 400; height: 500 }
    SignalSpy { id: opened; target: preview; signalName: "opened" }
    SignalSpy { id: held; target: preview; signalName: "held" }
    SignalSpy { id: moved; target: preview; signalName: "holdMoved" }
    SignalSpy { id: finished; target: preview; signalName: "holdFinished" }
    SignalSpy { id: swiped; target: preview; signalName: "swipeFinished" }
    function init() {
        motion.animateTo(0); wait(350);
        closed.clear(); opened.clear(); held.clear(); moved.clear(); finished.clear(); swiped.clear();
    }
    function test_drawer_follows_and_returns() {
        motion.begin(); motion.update(230, 0); compare(motion.progress, 0.23);
        motion.finish(); tryCompare(motion, "progress", 0); compare(closed.count, 1);
    }
    function test_drawer_fling_opens() {
        motion.begin(); motion.update(90, 1000); motion.finish();
        tryCompare(motion, "progress", 1); verify(motion.settledOpen);
    }
    function test_drawer_held_fling_expires() {
        motion.begin(); motion.update(90, 1000); wait(120); motion.finish();
        tryCompare(motion, "progress", 0);
    }
    function test_drawer_cancel_open_and_closed() {
        motion.begin(); motion.update(800, 0); motion.cancel(); tryCompare(motion, "progress", 0);
        motion.animateTo(1); tryCompare(motion, "progress", 1);
        motion.begin(); motion.update(-700, 0); motion.cancel(); tryCompare(motion, "progress", 1);
    }
    function test_drawer_reverse_and_interrupt() {
        motion.begin(); motion.update(600, 1000); motion.update(550, -1000); motion.finish();
        tryCompare(motion, "progress", 0);
        motion.animateTo(1); wait(80); motion.begin(); const start = motion.progress;
        motion.update(-40, 0); fuzzyCompare(motion.progress, start - .04, .001);
        motion.update(-1000, 0); motion.finish(); tryCompare(motion, "progress", 0);
    }
    function test_close_from_lower_screen() {
        motion.animateTo(1); tryCompare(motion, "progress", 1);
        motion.begin(); motion.update(-170, 0); motion.finishClose(); tryCompare(motion, "progress", 0);
    }
    function test_close_short_pull_returns_open() {
        motion.animateTo(1); tryCompare(motion, "progress", 1);
        motion.begin(); motion.update(-60, 0); motion.finishClose(); tryCompare(motion, "progress", 1);
    }
    function test_close_fling_and_reversal() {
        motion.animateTo(1); tryCompare(motion, "progress", 1);
        motion.begin(); motion.update(-70, -1000); motion.finishClose(); tryCompare(motion, "progress", 0);
        motion.animateTo(1); tryCompare(motion, "progress", 1);
        motion.begin(); motion.update(-200, -1000); motion.update(-170, 1000); motion.finishClose(); tryCompare(motion, "progress", 1);
    }
    function test_preview_tap() {
        touchEvent(preview).press(0, preview, 150, 300).commit(); wait(30);
        touchEvent(preview).release(0, preview, 150, 300).commit(); wait(30);
        compare(opened.count, 1); compare(swiped.count, 0); compare(held.count, 0);
    }
    function test_preview_short_swipe_bounces() {
        mousePress(preview, 150, 300); mouseMove(preview, 150, 265, 35); wait(120); mouseRelease(preview, 150, 265);
        compare(swiped.count, 1); compare(swiped.signalArguments[0][0], false); compare(opened.count, 0);
    }
    function test_preview_deliberate_swipe_closes() {
        const touch = touchEvent(preview);
        touch.press(0, preview, 150, 400).commit(); wait(30);
        touch.move(0, preview, 150, 350).commit(); wait(30);
        touch.move(0, preview, 150, 180).commit(); wait(120);
        touch.release(0, preview, 150, 180).commit(); wait(30);
        compare(swiped.count, 1); compare(swiped.signalArguments[0][0], true); compare(opened.count, 0);
        // The finger rested 120 ms before lifting: the card starts from rest.
        compare(swiped.signalArguments[0][1], 0);
    }
    function test_preview_flick_reports_release_speed() {
        const touch = touchEvent(preview);
        touch.press(0, preview, 150, 460).commit(); wait(10);
        for (let y = 430; y >= 130; y -= 30) {
            touch.move(0, preview, 150, y).commit(); wait(10);
        }
        touch.release(0, preview, 150, 130).commit(); wait(30);
        compare(swiped.count, 1); compare(swiped.signalArguments[0][0], true);
        // 30 px every ~10 ms: about 3000 px/s, allowing for timer slack.
        const speed = swiped.signalArguments[0][1];
        verify(speed > 1200 && speed < 4500, "release speed " + speed);
    }
    function test_preview_hold_is_not_close_or_open() {
        const touch = touchEvent(preview);
        touch.press(0, preview, 150, 300).commit(); wait(520);
        compare(held.count, 1);
        touch.move(0, preview, 160, 50).commit(); wait(30);
        touch.release(0, preview, 160, 50).commit(); wait(30);
        verify(moved.count > 0); compare(finished.count, 1); compare(swiped.count, 0); compare(opened.count, 0);
    }
}
