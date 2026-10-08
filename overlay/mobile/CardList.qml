import QtQuick

// The switcher's cards, edited in place so their delegates survive a sync:
// a group that goes away loses only its own card, a reorder moves cards, and
// a card whose workspace number changed keeps its delegate. Assigning a new
// array as a Repeater model instead rebuilds every delegate.
ListModel {
    // entries: [{cardKey, groupId}] in display order, keys unique.
    function apply(entries) {
        const keys = entries.map(entry => entry.cardKey);
        for (let i = count - 1; i >= 0; i--)
            if (keys.indexOf(get(i).cardKey) < 0) remove(i);
        for (let i = 0; i < entries.length; i++) {
            let at = -1;
            for (let j = i; j < count; j++) {
                if (get(j).cardKey === keys[i]) {
                    at = j;
                    break;
                }
            }
            if (at < 0) {
                insert(i, {cardKey: keys[i], groupId: entries[i].groupId});
                continue;
            }
            if (at !== i) move(at, i, 1);
            if (get(i).groupId !== entries[i].groupId) setProperty(i, "groupId", entries[i].groupId);
        }
    }
}
