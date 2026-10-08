import QtQuick
import QtTest
import "."

TestCase {
    name: "LeftBack"
    when: windowShown
    visible: true
    width: 480
    height: 800
    LeftBack { id: back; width: 24; height: 800 }
    SignalSpy { id: backs; target: back; signalName: "committed" }
    function init() { backs.clear(); }
    function test_inward_swipe_goes_back() {
        const touch = touchEvent(back);
        touch.press(0, back, 8, 400).commit(); wait(20);
        touch.move(0, back, 40, 402).commit(); wait(20);
        touch.move(0, back, 110, 406).commit(); wait(30);
        touch.release(0, back, 110, 406).commit(); wait(30);
        compare(backs.count, 1);
    }
    function test_a_finger_is_down_before_it_moves() {
        // The shell widens the strip's touch region on this, before any drag.
        const touch = touchEvent(back);
        compare(back.down, false);
        touch.press(0, back, 6, 300).commit(); wait(20);
        compare(back.down, true);
        compare(back.pressed, false);
        touch.release(0, back, 6, 300).commit(); wait(20);
        compare(back.down, false);
        compare(backs.count, 0);
    }
    function test_vertical_drag_does_not_go_back() {
        const touch = touchEvent(back);
        touch.press(0, back, 4, 200).commit(); wait(20);
        touch.move(0, back, 20, 360).commit(); wait(30);
        touch.release(0, back, 20, 360).commit(); wait(30);
        compare(backs.count, 0);
    }
}
