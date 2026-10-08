import QtQuick
import QtTest
import "."
TestCase {
    id: test; name: "CardList"; when: windowShown; visible: true
    width: 480; height: 200
    property int created: 0
    CardList { id: cards }
    Row {
        Repeater {
            id: repeater
            model: cards
            delegate: Item {
                required property string cardKey
                required property int groupId
                width: 10; height: 10
                Component.onCompleted: test.created++
            }
        }
    }
    function entries(list) {
        return list.map(pair => ({cardKey: pair[0], groupId: pair[1]}));
    }
    function keys() {
        const list = [];
        for (let i = 0; i < repeater.count; i++) list.push(repeater.itemAt(i).cardKey);
        return list.join(",");
    }
    function init() {
        cards.clear();
        cards.apply(entries([["a", 2], ["b", 3], ["c", 1]]));
        created = 0;
    }
    function test_close_keeps_other_delegates() {
        const b = repeater.itemAt(1), c = repeater.itemAt(2);
        cards.apply(entries([["a", 2], ["c", 1]]));
        compare(keys(), "a,c");
        compare(created, 0);
        verify(repeater.itemAt(1) === c);
        verify(b !== c);
    }
    function test_reorder_moves_delegates() {
        const a = repeater.itemAt(0), c = repeater.itemAt(2);
        cards.apply(entries([["c", 1], ["a", 2], ["b", 3]]));
        compare(keys(), "c,a,b");
        compare(created, 0);
        verify(repeater.itemAt(0) === c);
        verify(repeater.itemAt(1) === a);
    }
    function test_new_workspace_number_keeps_delegate() {
        const a = repeater.itemAt(0);
        cards.apply(entries([["a", 5], ["b", 3], ["c", 1]]));
        compare(created, 0);
        verify(repeater.itemAt(0) === a);
        compare(a.groupId, 5);
    }
    function test_open_adds_one_delegate() {
        cards.apply(entries([["d", 4], ["a", 2], ["b", 3], ["c", 1]]));
        compare(keys(), "d,a,b,c");
        compare(created, 1);
    }
    function test_close_last_empties() {
        cards.apply([]);
        compare(repeater.count, 0);
    }
}
