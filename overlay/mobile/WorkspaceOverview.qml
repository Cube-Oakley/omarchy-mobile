import QtQuick
import Quickshell.Hyprland
import Quickshell.Wayland

// One card per occupied workspace. The open app stays centered and older apps
// sit to its left, so a drag to the right walks back through them. A tiled
// pair is one split card. Workspace numbers stay an implementation detail.
Item {
    id: overview
    property bool active: false
    property real openness: 1
    property int heldId: 0
    property point heldPoint: Qt.point(0, 0)
    property int dropId: 0
    property real browse: 0
    property int index: 0
    property int pending: 0
    property real offset: 0
    property bool snapping: false
    property int expandingId: 0
    property real lift: 0
    property real home: 0
    readonly property real present: Math.min(2, openness + home)
    signal minimized()
    property int closingAt: -1
    property int closingId: 0
    property int restoreId: 0
    signal covered()
    signal previewReady()
    property bool frontReady: false
    property bool orderDirty: false
    function noteReady() {
        if (frontReady) return;
        frontReady = true;
        previewReady();
    }
    // A recent copy of the screen, refreshed every 0.7 s while an app is in
    // front, nothing covers it and the phone is being touched. When a swipe starts, the front card crops
    // its app out of this copy, so the card follows the finger from the first
    // frame instead of waiting about 100 ms for a fresh copy of the window.
    // It copies the screen rather than the window: closing a window during a
    // copy kills the shell, and an app can close itself at any time.
    property var captureScreen: null
    property bool warmPaused: false
    // Screen rectangle of each foreground window when the copy was taken.
    property var warmRects: ({})
    readonly property string foregroundKey: groupKey(Hyprland.toplevels.values.filter(window => window.workspace && window.workspace.id === 1))
    function warmSoon() {
        // Keep a window's copy only while it is still in front, where it was
        // copied. Dropping them all on every close meant a quick reopen found
        // no copy and paused again. Copies stop whenever something covers the
        // app, so a kept copy never shows the shade or the switcher.
        const kept = {};
        Hyprland.toplevels.values.forEach(window => {
            const address = String(window.address);
            const rect = warmRects[address];
            const info = window.lastIpcObject;
            if (rect && window.workspace && window.workspace.id === 1 && info && info.at && info.size
                    && info.at[0] === rect.x && info.at[1] === rect.y && info.size[0] === rect.width && info.size[1] === rect.height)
                kept[address] = rect;
        });
        warmRects = kept;
        // Let an app switch or launch animation finish first.
        warmDelay.restart();
    }
    // Parking the app during a swipe changes the foreground too; the copy
    // must survive that until the switcher closes.
    onForegroundKeyChanged: if (!active) warmSoon()
    onWarmPausedChanged: if (!warmPaused) warmSoon()
    readonly property var warm: warmLoader.item
    readonly property bool warmReady: warm !== null && warm.hasContent
    // Created once the shell window exists. A capture view made during startup
    // sets up its buffers without a window and can then never capture.
    Loader {
        id: warmLoader
        active: false
        sourceComponent: ScreencopyView {
            width: overview.captureScreen ? overview.captureScreen.width : 0
            height: overview.captureScreen ? overview.captureScreen.height : 0
            captureSource: overview.captureScreen
            live: false
        }
    }
    Timer { interval: 1000; running: true; onTriggered: warmLoader.active = true }
    // The copy is only drawn through the cards' crops. This keeps it hidden
    // even when no card refers to it, such as after the last app closes.
    ShaderEffectSource { visible: false; sourceItem: overview.warm; hideSource: true }
    // Each copy wakes the GPU, so they stop 2 s after the last touch or key,
    // with one final copy; reading a page costs nothing.
    IdleMonitor {
        id: inputIdle
        timeout: 2
        respectInhibitors: false
        onIsIdleChanged: if (isIdle) overview.takeWarm()
    }
    readonly property bool warmWanted: warm !== null && !active && !warmPaused && captureScreen !== null && foregroundKey !== ""
    function takeWarm() {
        if (!warmWanted) return;
        const rects = {};
        Hyprland.toplevels.values.forEach(window => {
            const info = window.lastIpcObject;
            if (!window.workspace || window.workspace.id !== 1 || !info || !info.at || !info.size) return;
            rects[String(window.address)] = Qt.rect(info.at[0], info.at[1], info.size[0], info.size[1]);
        });
        warmRects = rects;
        warm.captureFrame();
    }
    Timer { id: warmDelay; interval: 350; onTriggered: overview.takeWarm() }
    Timer { interval: 700; repeat: true; running: overview.warmWanted && !inputIdle.isIdle; onTriggered: overview.takeWarm() }
    property var cardsModel: []
    property string cardsKey: ""
    property var recent: []
    readonly property int limit: MobileTheme.state.device.workspaceCount || 8
    readonly property var groups: {
        const buckets = {};
        const tops = Hyprland.toplevels.values;
        for (let i = 0; i < tops.length; i++) {
            const window = tops[i];
            if (!window.workspace || !window.wayland) continue;
            const info = window.lastIpcObject;
            if (info && info.mapped === false) continue;
            const size = info && info.size;
            if (size && (size[0] < 2 || size[1] < 2)) continue;
            const id = window.workspace.id;
            if (id < 1 || id > limit) continue;
            if (!buckets[id]) buckets[id] = [];
            buckets[id].push(window);
        }
        const ids = Object.keys(buckets).map(Number).filter(id => buckets[id].length > 0);
        ids.sort((a, b) => {
            const order = overview.recent;
            const ia = order.indexOf(overview.groupKey(buckets[a]));
            const ib = order.indexOf(overview.groupKey(buckets[b]));
            return (ia < 0 ? 99 : ia) - (ib < 0 ? 99 : ib);
        });
        return ids.map(id => ({id: id, windows: buckets[id]}));
    }
    signal openGroup(int id)
    signal closeGroup(int id)
    signal tileGroups(int source, int target)
    signal dismissed()
    function groupKey(windows) {
        return windows.map(window => String(window.address)).filter(address => address && address !== "undefined").sort().join("+");
    }
    function touchWindows(windows) {
        const key = groupKey(windows || []);
        if (!key) return;
        recent = [key].concat(recent.filter(entry => entry !== key)).slice(0, limit);
    }
    function homeIndex() {
        const found = groups.findIndex(group => group.id === 1);
        const last = Math.max(0, groups.length - 1);
        if (found < 0) return last;
        return last - found;
    }
    function indexForWorkspace(workspaceId) {
        const group = groups.find(entry => entry.id === workspaceId);
        const address = group && group.windows[0] ? String(group.windows[0].address) : "";
        if (address && cardsModel.length) {
            const found = cardsModel.findIndex(entry => entry.windows.some(window => String(window.address) === address));
            if (found >= 0) return found;
        }
        return homeIndex();
    }
    function syncCards() {
        orderDirty = false;
        const key = groups.map(group => groupKey(group.windows)).join("|");
        const closedAt = closingAt;
        if (key === cardsKey) return;
        const previousKey = cardsModel[index] ? groupKey(cardsModel[index].windows) : "";
        cardsKey = key;
        cardsModel = groups.slice().reverse();
        const last = Math.max(0, cardsModel.length - 1);
        // Closing the last app leaves nothing to switch to: go to the desktop,
        // as Android does, rather than an empty switcher.
        if (closedAt >= 0 && cardsModel.length === 0) {
            closingAt = -1;
            closingId = 0;
            dismissed();
            return;
        }
        if (closedAt >= 0 && cardsModel.length > 0) {
            const removedLast = closedAt > last;
            index = removedLast ? last : Math.min(closedAt, last);
            pending = index;
            offset = (removedLast ? -1 : 1) * step;
            closingAt = -1;
            closingId = 0;
            snapping = true;
            snap.stop();
            snap.to = 0;
            snap.duration = 260;
            snap.start();
            return;
        }
        closingAt = -1;
        const keep = previousKey ? cardsModel.findIndex(group => groupKey(group.windows) === previousKey) : -1;
        const next = keep >= 0 ? keep : homeIndex();
        if (cardsModel.length < 2 || next !== index) {
            index = Math.min(next, last);
            pending = index;
            offset = 0;
        }
    }
    onGroupsChanged: {
        if (expandingId) { orderDirty = true; return; }
        syncCards();
    }
    function expandCard(id) {
        expandingId = id;
        lift = 0;
        liftAnim.duration = 210;
        liftAnim.to = 1;
        liftAnim.start();
    }
    function cancelExpand() {
        liftAnim.duration = 160;
        liftAnim.to = 0;
        liftAnim.start();
    }
    // Match the real tiled window (gaps on every side), not the full screen.
    // Measured tiled window: 8px from the usable area on every side
    // (6px gap plus the 2px border), 464×988 on this 480-wide layout.
    readonly property real tileGap: 8
    readonly property real rowBias: 16
    // Live windows are 464×988. The card uses that same shape, scaled down,
    // so the capture is not cropped and the open does not uncover a hidden strip.
    readonly property real frameW: 464
    readonly property real frameH: 988
    readonly property real previewScale: 0.82
    readonly property real tileX: frameW / Math.max(1, cardW)
    readonly property real tileY: frameH / Math.max(1, cardH)
    // The open switcher lifts every card. Scaling toward the tile from that
    // raised spot overshoots the status bar, then the real window snaps down.
    function tileNudge(pose) {
        if (pose <= 0) return 0;
        const yScale = 1 + (tileY - 1) * pose;
        const cardTop = (height - cardH) / 2 - rowBias * openness;
        const scaledTop = cardTop + (cardH / 2) * (1 - yScale);
        const targetTop = cardTop + (tileGap - cardTop) * pose;
        return targetTop - scaledTop;
    }
    // From the home screen there is no app to shrink into a card: play the
    // minimize animation backwards, so the cards grow and fade in.
    function revealFromHome(duration) {
        homeAnim.stop();
        home = 2;
        homeAnim.to = 0;
        homeAnim.duration = duration;
        homeAnim.start();
    }
    function minimizeHome() {
        homeAnim.stop();
        homeAnim.to = Math.max(0, 2 - openness);
        homeAnim.duration = 260;
        homeAnim.start();
    }
    function finishClose(id, at) {
        closingAt = at;
        closingId = id;
        closeGroup(id);
        closeWatch.restart();
    }
    onActiveChanged: {
        snapping = false;
        snap.stop();
        homeAnim.stop();
        home = 0;
        if (!active) return;
        if (orderDirty) syncCards();
        index = indexForWorkspace(1);
        if (warmReady && Object.keys(warmRects).length > 0) noteReady();
        pending = index;
        offset = 0;
    }
    function resist(raw) {
        const width = Math.max(1, step);
        const last = Math.max(0, groups.length - 1);
        const minOffset = -(last - index) * width;
        const maxOffset = index * width;
        if (raw > maxOffset) return maxOffset + (raw - maxOffset) * 0.28;
        if (raw < minOffset) return minOffset + (raw - minOffset) * 0.28;
        return raw;
    }
    function finishBrowse(velocity) {
        const next = pager.target(index, groups.length, offset, step, velocity);
        pending = next;
        const dest = (index - next) * step;
        if (Math.abs(offset - dest) < 1) {
            index = next;
            pending = next;
            offset = 0;
            return;
        }
        snap.duration = Math.round(Math.min(240, 120 + Math.abs(offset - dest) * 0.28));
        snap.to = dest;
        snapping = true;
        snap.start();
    }
    function commitBrowse() {
        if (!snapping) return;
        snapping = false;
        index = pending;
        offset = 0;
    }
    function groupAt(point) {
        for (let i = 0; i < cards.count; i++) {
            const item = cards.itemAt(i);
            if (!item) continue;
            const local = item.mapFromItem(overview, point.x, point.y);
            if (local.x >= 0 && local.y >= 0 && local.x < item.width && local.y < item.height)
                return item.groupId;
        }
        return 0;
    }
    function beginHold(id, scenePosition) {
        heldId = id;
        heldPoint = mapFromItem(null, scenePosition.x, scenePosition.y);
        dropId = 0;
    }
    function moveHold(scenePosition) {
        if (!heldId) return;
        heldPoint = mapFromItem(null, scenePosition.x, scenePosition.y);
        const hit = groupAt(heldPoint);
        dropId = hit && hit !== heldId ? hit : 0;
    }
    function finishHold() {
        const source = heldId;
        const target = dropId;
        heldId = 0;
        dropId = 0;
        if (source && target) tileGroups(source, target);
    }
    // Card size stays put while the switcher opens. Resizing on every frame
    // forced a full layout and a new capture constraint, which is what made
    // the motion look closer to 30 Hz than 60.
    readonly property real cardW: frameW * previewScale
    readonly property real cardH: frameH * previewScale
    readonly property real step: cardW + 16
    SnapPager { id: pager }
    NumberAnimation {
        id: snap
        target: overview
        property: "offset"
        easing.type: Easing.OutCubic
        onFinished: overview.commitBrowse()
    }
    NumberAnimation {
        id: liftAnim
        target: overview
        property: "lift"
        easing.type: Easing.OutCubic
        onFinished: {
            if (overview.lift > 0.98 && overview.expandingId) overview.covered();
            else if (overview.lift < 0.02) overview.expandingId = 0;
        }
    }
    NumberAnimation {
        id: homeAnim
        target: overview
        property: "home"
        easing.type: Easing.OutCubic
        onFinished: if (overview.home > 0.2) overview.minimized()
    }
    Timer {
        id: closeWatch
        interval: 420
        onTriggered: {
            if (overview.closingId && overview.groups.some(group => group.id === overview.closingId))
                overview.restoreId = overview.closingId;
            overview.closingAt = -1;
            overview.closingId = 0;
        }
    }

    Item {
        anchors.fill: parent
        Row {
            id: strip
            spacing: 16
            height: overview.cardH
            y: (parent.height - height) / 2 - overview.rowBias * overview.openness
            x: parent.width / 2 - overview.cardW / 2 - overview.index * overview.step + overview.offset
            Repeater {
                id: cards
                model: overview.cardsModel
                delegate: Item {
                    id: card
                    required property var modelData
                    required property int index
                    readonly property int groupId: modelData.id
                    readonly property int panes: Math.min(modelData.windows.length, 2)
                    readonly property bool targeted: overview.dropId === groupId
                    property bool shown: false
                    readonly property string title: {
                        const windows = modelData.windows;
                        function name(window) {
                            return window && window.lastIpcObject && window.lastIpcObject.class ? window.lastIpcObject.class : "App";
                        }
                        if (!windows || windows.length === 0) return "App";
                        if (windows.length === 1) return name(windows[0]);
                        return name(windows[0]) + "  ·  " + name(windows[1]);
                    }
                    property real swipeOffset: 0
                    property bool closing: false
                    readonly property bool expanding: overview.expandingId === groupId
                    readonly property real thrown: swipeOffset < 0 ? Math.min(1, -swipeOffset / Math.max(1, height)) : 0
                    width: overview.cardW
                    height: overview.cardH
                    z: expanding || closing ? 8 : 0
                    function followSwipe(distance) {
                        dismiss.stop();
                        closing = false;
                        swipeOffset = -distance;
                    }
                    function endSwipe(close) {
                        closing = close;
                        const travel = close ? -(height + 160) : 0;
                        dismiss.easing.type = close ? Easing.InCubic : Easing.OutCubic;
                        dismiss.to = travel;
                        dismiss.duration = close ? Math.max(180, Math.min(340, Math.abs(travel - swipeOffset) * 0.42)) : 230;
                        dismiss.start();
                    }
                    NumberAnimation {
                        id: dismiss
                        target: card
                        property: "swipeOffset"
                        onFinished: if (card.closing) overview.finishClose(card.groupId, card.index)
                    }
                    Connections {
                        target: overview
                        function onRestoreIdChanged() {
                            if (overview.restoreId !== card.groupId) return;
                            overview.restoreId = 0;
                            card.endSwipe(false);
                        }
                    }
                    Item {
                        id: face
                        width: parent.width
                        height: parent.height
                        readonly property bool front: card.index === overview.index && !overview.expandingId
                        readonly property real pose: card.expanding ? overview.lift : (front && overview.present <= 1 ? (1 - overview.present) : 0)
                        // A card tapped off to the side slides to the middle as it
                        // grows, landing where its window will appear.
                        x: card.expanding ? -((card.index - overview.index) * overview.step + overview.offset) * overview.lift : 0
                        y: card.swipeOffset + overview.tileNudge(pose)
                        transformOrigin: Item.Center
                        readonly property real sx: {
                            const shrink = 1 - card.thrown * 0.2;
                            if (card.expanding) return (1 + (overview.tileX - 1) * overview.lift) * shrink;
                            if (!front) return (card.targeted ? 0.96 : 1) * shrink;
                            const amount = overview.present;
                            const base = amount <= 1 ? (overview.tileX + (1 - overview.tileX) * amount) : (1 - 0.32 * (amount - 1));
                            return base * shrink;
                        }
                        readonly property real sy: {
                            const shrink = 1 - card.thrown * 0.2;
                            if (card.expanding) return (1 + (overview.tileY - 1) * overview.lift) * shrink;
                            if (!front) return (card.targeted ? 0.96 : 1) * shrink;
                            const amount = overview.present;
                            const base = amount <= 1 ? (overview.tileY + (1 - overview.tileY) * amount) : (1 - 0.32 * (amount - 1));
                            return base * shrink;
                        }
                        transform: Scale {
                            origin.x: face.width / 2
                            origin.y: face.height / 2
                            xScale: face.sx
                            yScale: face.sy
                        }
                        opacity: {
                            const fade = card.thrown < 0.62 ? 1 : Math.max(0, 1 - (card.thrown - 0.62) / 0.38);
                            const other = overview.expandingId && !card.expanding ? (1 - overview.lift) : 1;
                            const homeFade = front
                                ? (overview.present <= 1 ? 1 : Math.max(0, 1 - (overview.present - 1)))
                                : (overview.present <= 1 ? Math.max(0, Math.min(1, (overview.present - 0.3) / 0.7))
                                                         : Math.max(0, 1 - (overview.present - 1)));
                            return fade * other * homeFade;
                        }
                    Rectangle {
                        anchors.fill: parent
                        radius: {
                            const tileRadius = MobileTheme.radius(8);
                            const cardRadius = MobileTheme.radius(22);
                            if (card.expanding) return cardRadius + (tileRadius - cardRadius) * overview.lift;
                            return tileRadius + (cardRadius - tileRadius) * Math.min(1, overview.present);
                        }
                        color: MobileTheme.surface
                        border.width: card.targeted ? 3 : 0
                        border.color: MobileTheme.accent
                        clip: true
                        Item {
                            anchors.fill: parent
                            Item {
                                anchors.fill: parent
                                Row {
                                    anchors.fill: parent
                                    spacing: 0
                                    Repeater {
                                        model: card.panes
                                        Rectangle {
                                            required property int index
                                            readonly property var window: card.modelData.windows[index]
                                            width: parent.width / card.panes
                                            height: parent.height
                                            color: MobileTheme.background
                                            clip: true
                                            ScreencopyView {
                                                id: shot
                                                anchors.centerIn: parent
                                                // A missing picture used to leave a blank gap that
                                                // could not be tapped or swiped. The card itself
                                                // stays up; only the capture waits for a real frame.
                                                // Stop copying as soon as the card is dismissed.
                                                // Closing the window while a capture is in flight
                                                // kills the whole shell.
                                                // Until its own capture has held a frame for 64 ms this
                                                // opening (the first is often black), the pane shows the
                                                // screen copy when its window was in front for it.
                                                readonly property var warmRect: window ? overview.warmRects[String(window.address)] : undefined
                                                readonly property bool warmed: overview.warmReady && warmRect !== undefined
                                                property bool fresh: false
                                                opacity: card.shown && (fresh || !warmed) ? 1 : 0
                                                // Every capture is a full-resolution copy of its window, so
                                                // starting them all with the opening animation made it stutter.
                                                // The current app's card starts at once, the others once the
                                                // zoom-out has nearly landed; each stays latched for this
                                                // opening so a partial drag back does not blank them.
                                                readonly property bool open: overview.active && !card.closing && window && window.wayland
                                                readonly property bool due: open && (card.index === overview.index || overview.openness > 0.9)
                                                property bool started: false
                                                onDueChanged: if (due) started = true
                                                onOpenChanged: if (!open) { started = false; settled = false; fresh = false; }
                                                readonly property bool copying: open && started
                                                // Like Android's thumbnails, a card stops copying shortly after
                                                // its first real frame and keeps it until the next opening.
                                                property bool settled: false
                                                captureSource: copying ? window.wayland : null
                                                constraintSize: Qt.size(Math.max(1, parent.width), Math.max(1, parent.height))
                                                live: copying && !settled
                                                Timer { id: settle; interval: 264; onTriggered: shot.settled = true }
                                                function publish() {
                                                    if (!hasContent || card.shown) return;
                                                    card.shown = true;
                                                    if (card.index === overview.index) overview.noteReady();
                                                }
                                                // The first captured frame is often black. Keep it
                                                // hidden until a later frame has replaced it.
                                                Component.onCompleted: { if (due) started = true; if (hasContent) { reveal.restart(); settle.restart(); } }
                                                onHasContentChanged: if (hasContent) { reveal.restart(); settle.restart(); } else fresh = false
                                                Timer { id: reveal; interval: 64; onTriggered: { shot.fresh = true; shot.publish(); } }
                                                Timer {
                                                    interval: 700
                                                    running: overview.active && !card.shown
                                                    onTriggered: {
                                                        card.shown = true;
                                                        if (card.index === overview.index) overview.noteReady();
                                                    }
                                                }
                                            }
                                            ShaderEffectSource {
                                                anchors.fill: parent
                                                visible: shot.warmed && !shot.fresh
                                                sourceItem: overview.warm
                                                sourceRect: shot.warmed ? shot.warmRect : Qt.rect(0, 0, 0, 0)
                                                hideSource: true
                                            }
                                        }
                                    }
                                }
                            }
                            Item {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                height: 36
                                opacity: (card.expanding ? (1 - overview.lift) : 1) * Math.min(1, overview.present)
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    x: 18
                                    width: parent.width - 36
                                    text: card.title
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    color: MobileTheme.foreground
                                    font.family: MobileTheme.fontFamily
                                    font.pixelSize: 16
                                    font.bold: true
                                }
                            }
                        }
                        PreviewGestures {
                            anchors.fill: parent
                            enabled: !overview.expandingId && overview.openness > 0.55 && (!overview.heldId || overview.heldId === card.groupId)
                            onOpened: overview.openGroup(card.groupId)
                            onSwipeMoved: distance => card.followSwipe(distance)
                            onSwipeFinished: close => card.endSwipe(close)
                            onHeld: position => overview.beginHold(card.groupId, position)
                            onHoldMoved: position => overview.moveHold(position)
                            onHoldFinished: overview.finishHold()
                            onGestureCanceled: {
                                card.endSwipe(false);
                                overview.heldId = 0;
                                overview.dropId = 0;
                            }
                        }
                    }
                    }
                }
            }
        }
        DragHandler {
            id: browseDrag
            target: null
            enabled: !overview.expandingId && overview.groups.length > 1 && !overview.heldId && overview.openness > 0.7
            xAxis.enabled: true
            yAxis.enabled: false
            property real startOffset: 0
            onActiveChanged: {
                if (active) {
                    overview.snapping = false;
                    snap.stop();
                    startOffset = overview.offset;
                    return;
                }
                overview.finishBrowse(centroid.velocity.x);
            }
            onActiveTranslationChanged: if (active) overview.offset = overview.resist(startOffset + activeTranslation.x)
        }
    }

}
