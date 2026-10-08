import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Io

// The home screen: pages of app icons and widgets, under the drawer, shown
// only while no app is in front. It belongs to the phone shell alone, so a
// docked desktop session never has it. The layout is
// ~/.config/omarchy-mobile/home.json; widgets are plugins, each a folder in
// ~/.local/share/omarchy-mobile/widgets/ with manifest.json and Widget.qml
// (widgets/README.md).
//
// Long-press anywhere to edit: drag icons and widgets (to a screen edge for
// the next page), remove them, add apps, widgets and pages, and pick the grid
// and the page transition.
Item {
    id: home

    // Set by the shell.
    property var apps: []
    property bool shown: false
    // Shown with the screen on: widgets on the current page may update.
    property bool active: false
    property var weather: ({})
    signal launchApp(var app, Item from, string icon)
    signal weatherWanted()
    signal weatherOpened()

    property bool editing: false
    // "" | apps | widgets | layout: the sheet open over the edited home.
    property string sheet: ""
    readonly property var transitions: [
        { id: "slide", label: "Slide" }, { id: "depth", label: "Depth" },
        { id: "cube", label: "Cube" }, { id: "stack", label: "Stack" },
        { id: "fade", label: "Fade" }, { id: "carousel", label: "Carousel" }
    ]
    readonly property string widgetRoot: (Quickshell.env("XDG_DATA_HOME") || Quickshell.env("HOME") + "/.local/share")
                                         + "/omarchy-mobile/widgets"
    // Installed widgets by id: {dir, name, description, sizes: [[w, h]...]}.
    property var widgets: ({})

    property var layout: defaultLayout()
    readonly property int columns: layout.columns
    readonly property int rows: layout.rows
    readonly property int pageCount: layout.pages.length
    // The page in view, fractional while swiping.
    property real position: 0
    readonly property int current: Math.max(0, Math.min(pageCount - 1, Math.round(position)))

    // The grid's place on a page: above the page dots and the navigation
    // strip (the drawer surface itself starts below the status bar).
    readonly property real sidePad: 12
    readonly property real topPad: 12
    readonly property real bottomPad: 58
    readonly property real cellW: (width - 2 * sidePad) / columns
    readonly property real cellH: (height - topPad - bottomPad) / rows
    readonly property real iconSize: Math.max(44, Math.min(78, Math.min(cellW * 0.6, (cellH - (layout.labels ? 22 : 0)) * 0.8)
                                                                * (layout.iconScale || 1)))

    onShownChanged: if (!shown) editing = false
    onEditingChanged: {
        if (editing) scan.running = true;
        else { sheet = ""; cancelDrag(); tidy(); }
    }
    onPageCountChanged: if (position > pageCount - 1) goToPage(pageCount - 1)

    // ---- layout ----
    function defaultLayout() {
        return { version: 1, columns: 4, rows: 6, transition: "slide", labels: true, iconScale: 1,
                 pages: [{ uid: "p0", items: [{ uid: "clock", type: "widget", widget: "clock", col: 0, row: 0, w: 4, h: 2 }] }] };
    }
    function newUid() { return "i" + Date.now().toString(36) + Math.floor(Math.random() * 1e6).toString(36); }
    function sanitize(raw) {
        const out = defaultLayout();
        if (!raw || typeof raw !== "object") return out;
        const whole = (value, low, high, fallback) => Number.isInteger(value) ? Math.max(low, Math.min(high, value)) : fallback;
        out.columns = whole(raw.columns, 3, 6, 4);
        out.rows = whole(raw.rows, 4, 8, 6);
        out.transition = transitions.some(t => t.id === raw.transition) ? raw.transition : "slide";
        out.labels = raw.labels !== false;
        out.iconScale = [0.85, 1, 1.15].indexOf(raw.iconScale) >= 0 ? raw.iconScale : 1;
        if (Array.isArray(raw.pages) && raw.pages.length) {
            const seen = {};
            out.pages = raw.pages.slice(0, 12).map(page => ({
                // Pages keep an id too, so the page views survive edits.
                uid: page && typeof page.uid === "string" && page.uid && !seen[page.uid] ? (seen[page.uid] = true, page.uid) : newUid(),
                items: (page && Array.isArray(page.items) ? page.items : []).filter(item => item
                        && ((item.type === "app" && typeof item.app === "string")
                            || (item.type === "widget" && typeof item.widget === "string")))
                    .map(item => {
                        let uid = typeof item.uid === "string" && item.uid && !seen[item.uid] ? item.uid : newUid();
                        seen[uid] = true;
                        const clean = { uid: uid, type: item.type, col: whole(item.col, 0, 7, 0), row: whole(item.row, 0, 7, 0),
                                        w: whole(item.w, 1, 6, 1), h: whole(item.h, 1, 8, 1) };
                        if (item.type === "app") clean.app = item.app; else clean.widget = item.widget;
                        return clean;
                    })
            }));
        }
        reflow(out);
        return out;
    }
    // Whether an item of w x h fits at col, row among items (skip: an item
    // that is moving).
    function fits(next, items, col, row, w, h, skip) {
        if (col < 0 || row < 0 || col + w > next.columns || row + h > next.rows) return false;
        return !items.some(other => other.uid !== skip && col < other.col + other.w && other.col < col + w
                                    && row < other.row + other.h && other.row < row + h);
    }
    function freeSpot(next, items, w, h) {
        for (let row = 0; row + h <= next.rows; row++)
            for (let col = 0; col + w <= next.columns; col++)
                if (fits(next, items, col, row, w, h, "")) return { col: col, row: row };
        return null;
    }
    // Puts item on the first page from start with room, or a new page.
    function placeItem(next, item, start) {
        item.w = Math.min(item.w, next.columns);
        item.h = Math.min(item.h, next.rows);
        for (let k = 0; k < next.pages.length; k++) {
            const page = (start + k) % next.pages.length;
            const spot = freeSpot(next, next.pages[page].items, item.w, item.h);
            if (spot) {
                item.col = spot.col; item.row = spot.row;
                next.pages[page].items.push(item);
                return page;
            }
        }
        item.col = 0; item.row = 0;
        next.pages.push({ uid: newUid(), items: [item] });
        return next.pages.length - 1;
    }
    // After the grid changes, items that no longer fit move to free room.
    function reflow(next) {
        const homeless = [];
        next.pages.forEach((page, index) => {
            const kept = [];
            page.items.forEach(item => {
                item.w = Math.min(item.w, next.columns);
                item.h = Math.min(item.h, next.rows);
                if (fits(next, kept, item.col, item.row, item.w, item.h, item.uid)) kept.push(item);
                else homeless.push({ item: item, page: index });
            });
            page.items = kept;
        });
        homeless.forEach(entry => placeItem(next, entry.item, entry.page));
    }
    function edit(change) {
        const next = JSON.parse(JSON.stringify(layout));
        const result = change(next);
        layout = next;
        saved = JSON.stringify(next, null, 1) + "\n";
        store.setText(saved);
        return result;
    }
    // Drops empty pages (keeping one), as editing ends.
    function tidy() {
        if (!layout.pages.some(page => !page.items.length) || layout.pages.length < 2) return;
        const keep = Math.round(position);
        edit(next => {
            next.pages = next.pages.filter(page => page.items.length);
            if (!next.pages.length) next.pages = [{ uid: newUid(), items: [] }];
        });
        position = Math.min(keep, pageCount - 1);
    }
    function onHome(appId) {
        for (let p = 0; p < layout.pages.length; p++)
            if (layout.pages[p].items.some(item => item.type === "app" && item.app === appId)) return p;
        return -1;
    }
    // From the drawer (a long press on an app) or the apps sheet.
    function addApp(appId) {
        const already = onHome(appId);
        if (already >= 0) { goToPage(already); return; }
        const page = edit(next => placeItem(next, { uid: newUid(), type: "app", app: appId, w: 1, h: 1 }, current));
        goToPage(page);
    }
    function addWidget(widgetId, w, h) {
        const page = edit(next => placeItem(next, { uid: newUid(), type: "widget", widget: widgetId, w: w, h: h }, current));
        goToPage(page);
        sheet = "";
    }
    function removeItem(uid) {
        edit(next => next.pages.forEach(page => page.items = page.items.filter(item => item.uid !== uid)));
    }
    // A widget's next size from its manifest that fits where it is.
    function resizeWidget(uid) {
        edit(next => {
            for (const page of next.pages) {
                const item = page.items.find(entry => entry.uid === uid);
                if (!item) continue;
                const sizes = (widgets[item.widget] || {}).sizes || [[item.w, item.h]];
                const at = sizes.findIndex(size => size[0] === item.w && size[1] === item.h);
                for (let k = 1; k <= sizes.length; k++) {
                    const size = sizes[(at + k) % sizes.length];
                    if (fits(next, page.items, item.col, item.row, size[0], size[1], uid)) {
                        item.w = size[0]; item.h = size[1];
                        return;
                    }
                }
            }
        });
    }
    function addPage() {
        edit(next => next.pages.push({ uid: newUid(), items: [] }));
        goToPage(pageCount - 1);
    }
    function setOption(name, value) {
        edit(next => {
            next[name] = value;
            if (name === "columns" || name === "rows") reflow(next);
        });
    }
    function entryFor(appId) {
        return apps.find(app => app.id === appId) || DesktopEntries.byId(appId);
    }
    function open(appId, from) {
        const entry = entryFor(appId);
        if (entry) launchApp(entry, from || home, entry.icon ? Quickshell.iconPath(entry.icon, true) : "");
    }
    function widgetSource(widgetId) {
        const info = widgets[widgetId];
        return info ? "file://" + info.dir + "/Widget.qml" : "";
    }

    // What this screen last wrote, so its own write is not applied again.
    property string saved: ""
    FileView {
        id: store
        path: (Quickshell.env("XDG_CONFIG_HOME") || Quickshell.env("HOME") + "/.config") + "/omarchy-mobile/home.json"
        blockLoading: true
        printErrors: false
        atomicWrites: true
        // Edited elsewhere (by hand, or restored): applied at once.
        watchChanges: true
        onFileChanged: reload()
        onLoaded: {
            if (text() === home.saved) return;
            try { home.layout = home.sanitize(JSON.parse(text())); }
            catch (e) { console.warn("MOBILE_HOME_LAYOUT_INVALID " + e); }
        }
    }
    // Installed widgets: each folder's manifest.json, one line each.
    Process {
        id: scan
        running: true
        command: ["sh", "-c", "for f in \"$1\"/*/manifest.json; do [ -f \"$f\" ] && printf '%s\\t' \"${f%/manifest.json}\" && tr -d '\\n' < \"$f\" && echo; done", "scan", home.widgetRoot]
        property var found: ({})
        onStarted: found = {}
        stdout: SplitParser {
            onRead: line => {
                const tab = line.indexOf("\t");
                if (tab < 0) return;
                try {
                    const manifest = JSON.parse(line.slice(tab + 1));
                    const id = manifest.id || line.slice(0, tab).split("/").pop();
                    const sizes = (Array.isArray(manifest.sizes) ? manifest.sizes : [[2, 2]])
                        .filter(size => Array.isArray(size) && size.length === 2);
                    scan.found[id] = { dir: line.slice(0, tab), name: manifest.name || id,
                                       description: manifest.description || "", sizes: sizes.length ? sizes : [[2, 2]] };
                } catch (e) { console.warn("MOBILE_HOME_WIDGET_INVALID " + line.slice(0, tab)); }
            }
        }
        onExited: home.widgets = found
    }

    // ---- pages ----
    NumberAnimation {
        id: settle
        target: home; property: "position"
        duration: 320; easing.type: Easing.OutCubic
    }
    function goToPage(page) {
        settle.stop();
        settle.from = position;
        settle.to = Math.max(0, Math.min(pageCount - 1, page));
        settle.start();
    }
    function pageAt(index) { return pages.itemAt(index); }

    // Sideways between pages; the drawer's own handler keeps the vertical.
    DragHandler {
        id: swipe
        target: null
        xAxis.enabled: true
        yAxis.enabled: false
        enabled: home.shown && home.sheet === "" && !home.dragging
        property real from: 0
        onActiveChanged: {
            if (active) { settle.stop(); from = home.position; return; }
            const base = Math.round(from);
            const speed = centroid.velocity.x;
            home.goToPage(speed < -350 ? base + 1 : speed > 350 ? base - 1 : Math.round(home.position));
        }
        onActiveTranslationChanged: {
            if (!active) return;
            let next = from - activeTranslation.x / home.width;
            const last = home.pageCount - 1;
            if (next < 0) next *= 0.3;
            else if (next > last) next = last + (next - last) * 0.3;
            home.position = next;
        }
    }
    // A long press anywhere edits; a tap on empty space ends editing. (The
    // tiles' own handlers see the same touches, so a tap on one is told
    // apart by where it fell.)
    TapHandler {
        enabled: home.shown && home.sheet === ""
        longPressThreshold: 0.45
        onLongPressed: home.editing = true
        onTapped: point => {
            if (Date.now() - home.handledAt < 300) return;
            if (home.editing && !home.tileAt(point.position) && point.position.y > 56
                    && point.position.y < home.height - 100)
                home.editing = false;
        }
    }
    // When a tile or its badge last handled a tap: the background's handler
    // sees the same tap afterwards (after a removal its cell is empty).
    property double handledAt: 0
    function handled() { handledAt = Date.now(); }
    // Whether a point (in home's coordinates) falls on an icon or widget of
    // the page in view.
    function tileAt(point) {
        const page = pageAt(current);
        if (!page) return false;
        const local = page.grid.mapFromItem(home, point.x, point.y);
        const col = Math.floor(local.x / cellW), row = Math.floor(local.y / cellH);
        return (layout.pages[current] || { items: [] }).items.some(item => col >= item.col && col < item.col + item.w
                                                                    && row >= item.row && row < item.row + item.h);
    }

    Item {
        id: pager
        anchors.fill: parent
        // Editing shows the whole page smaller, with room for the tools.
        scale: home.editing ? 0.82 : 1
        Behavior on scale { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }

        Repeater {
            id: pages
            model: ScriptModel { values: home.layout.pages; objectProp: "uid" }
            Item {
                id: page
                required property var modelData
                required property int index
                readonly property real d: index - home.position
                readonly property string kind: home.layout.transition
                readonly property alias grid: grid
                width: pager.width
                height: pager.height
                visible: Math.abs(d) < 0.999
                z: kind === "stack" ? (d > 0 ? 1 : 0) : kind === "depth" ? (d < 0 ? 1 : 0) : 0
                x: kind === "fade" ? 0 : kind === "depth" ? (d < 0 ? d * width : 0)
                   : kind === "stack" ? (d > 0 ? d * width : 0) : d * width
                opacity: kind === "fade" ? 1 - Math.abs(d) : kind === "depth" && d > 0 ? 1 - d : 1
                scale: kind === "depth" && d > 0 ? 0.75 + 0.25 * (1 - d) : kind === "stack" && d < 0 ? 1 + d * 0.1 : 1
                transform: [
                    Rotation {
                        // The cube turns about the edge the two pages share.
                        origin.x: page.d < 0 ? page.width : 0
                        origin.y: page.height / 2
                        axis { x: 0; y: 1; z: 0 }
                        angle: page.kind === "cube" ? page.d * 70 : 0
                    },
                    Rotation {
                        // The carousel swings about a point below the screen.
                        origin.x: page.width / 2
                        origin.y: page.height * 1.6
                        angle: page.kind === "carousel" ? page.d * 22 : 0
                    }
                ]
                // The page behind a stacked page dims.
                Rectangle {
                    anchors.fill: parent
                    color: "black"
                    opacity: page.kind === "stack" && page.d < 0 ? -page.d * 0.35 : 0
                    visible: opacity > 0
                    z: 2
                }
                // The edited page's outline.
                Rectangle {
                    anchors.fill: parent
                    anchors.topMargin: home.topPad - 8
                    anchors.bottomMargin: home.bottomPad - 22
                    radius: MobileTheme.radius(28)
                    color: "transparent"
                    border.color: MobileTheme.muted
                    border.width: 2
                    opacity: home.editing ? 0.9 : 0
                    Behavior on opacity { NumberAnimation { duration: 180 } }
                }
                Item {
                    id: grid
                    x: home.sidePad
                    y: home.topPad
                    width: home.cellW * home.columns
                    height: home.cellH * home.rows
                    // Cell guides while editing.
                    Repeater {
                        model: home.editing && page.index === home.current ? home.columns * home.rows : 0
                        Rectangle {
                            required property int index
                            x: (index % home.columns) * home.cellW + 4
                            y: Math.floor(index / home.columns) * home.cellH + 4
                            width: home.cellW - 8
                            height: home.cellH - 8
                            radius: MobileTheme.radius(16)
                            color: MobileTheme.foreground
                            opacity: 0.06
                        }
                    }
                    Repeater {
                        model: ScriptModel {
                            values: page.modelData.items
                            objectProp: "uid"
                        }
                        Tile { pageIndex: page.index; pageCurrent: Math.abs(page.d) < 0.5 }
                    }
                }
            }
        }
    }

    // Page dots, above the navigation strip.
    Row {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 34
        spacing: 8
        visible: home.pageCount > 1 || home.editing
        Repeater {
            model: home.pageCount
            Rectangle {
                required property int index
                width: index === home.current ? 18 : 7
                height: 7
                radius: MobileTheme.radius(4)
                color: index === home.current ? MobileTheme.accent : MobileTheme.foreground
                opacity: index === home.current ? 1 : 0.45
                Behavior on width { NumberAnimation { duration: 160 } }
            }
        }
    }

    // ---- moving an item while editing ----
    property var dragged: null
    readonly property bool dragging: dragged !== null
    property point dragPoint: Qt.point(0, 0)
    function beginDrag(item, pageIndex, tile, point) {
        const corner = tile.mapToItem(home, 0, 0);
        dragged = { uid: item.uid, type: item.type, app: item.app, widget: item.widget, w: item.w, h: item.h,
                    page: pageIndex, dx: point.x - corner.x, dy: point.y - corner.y };
        dragPoint = point;
    }
    function moveDrag(point) {
        dragPoint = point;
        const edge = 28;
        if (point.x < edge || point.x > width - edge) { if (!flip.running) flip.start(); }
        else flip.stop();
    }
    // The moved tile went away mid-drag (its page was rebuilt): nothing moves.
    function cancelDrag() {
        flip.stop();
        dragged = null;
    }
    function endDrag() {
        flip.stop();
        const moving = dragged;
        dragged = null;
        if (!moving) return;
        const target = current;
        const page = pageAt(target);
        if (!page) return;
        // The item's top-left on the page grid, to the nearest cell.
        const local = page.grid.mapFromItem(home, dragPoint.x - moving.dx, dragPoint.y - moving.dy);
        const col = Math.round(local.x / cellW);
        const row = Math.round(local.y / cellH);
        edit(next => {
            const from = next.pages[moving.page].items;
            const index = from.findIndex(item => item.uid === moving.uid);
            if (index < 0) return;
            const item = from[index];
            const there = next.pages[target].items;
            if (fits(next, there, col, row, item.w, item.h, item.uid)) {
                from.splice(index, 1);
                item.col = col; item.row = row;
                there.push(item);
                return;
            }
            // Onto an item of the same size: they trade places.
            const other = there.find(entry => entry.uid !== item.uid && entry.col === col && entry.row === row
                                               && entry.w === item.w && entry.h === item.h);
            if (other && moving.page === target) {
                other.col = item.col; other.row = item.row;
                item.col = col; item.row = row;
            }
        });
    }
    // Held at a screen edge, a moved item turns the page; past the last page
    // it makes a new one.
    Timer {
        id: flip
        interval: 550
        repeat: true
        onTriggered: {
            if (!home.dragging) { stop(); return; }
            if (home.dragPoint.x < 40) home.goToPage(home.current - 1);
            else if (home.current < home.pageCount - 1) home.goToPage(home.current + 1);
            // One new page past the last, not one more for each moment held.
            else if (home.layout.pages[home.current].items.length) home.addPage();
        }
    }
    // What follows the finger.
    Rectangle {
        visible: home.dragging
        z: 50
        readonly property real across: home.dragged ? home.dragged.w * home.cellW * pager.scale : 0
        readonly property real down: home.dragged ? home.dragged.h * home.cellH * pager.scale : 0
        x: home.dragPoint.x - (home.dragged ? home.dragged.dx : 0)
        y: home.dragPoint.y - (home.dragged ? home.dragged.dy : 0)
        width: across
        height: down
        radius: MobileTheme.radius(20)
        color: MobileTheme.surface
        border.color: MobileTheme.accent
        border.width: 2
        opacity: 0.92
        AppIcon {
            anchors.centerIn: parent
            visible: home.dragged && home.dragged.type === "app"
            width: home.iconSize * 0.8; height: width
            readonly property var entry: home.dragged && home.dragged.type === "app" ? home.entryFor(home.dragged.app) : null
            icon: entry && entry.icon ? entry.icon : ""
            name: entry ? (entry.name || "") : ""
        }
        Text {
            anchors.centerIn: parent
            visible: home.dragged && home.dragged.type === "widget"
            text: home.dragged && home.dragged.type === "widget" ? ((home.widgets[home.dragged.widget] || {}).name || "Widget") : ""
            color: MobileTheme.foreground
            font.family: MobileTheme.fontFamily
            font.pixelSize: 14
        }
    }

    // ---- editing tools ----
    RowLayout {
        visible: home.editing && home.sheet === ""
        anchors { left: parent.left; right: parent.right; top: parent.top; leftMargin: 14; rightMargin: 14; topMargin: 6 }
        spacing: 8
        TouchButton { Layout.fillWidth: true; implicitHeight: 42; textSize: 14; label: "Apps"; onClicked: home.sheet = "apps" }
        TouchButton { Layout.fillWidth: true; implicitHeight: 42; textSize: 14; label: "Widgets"; onClicked: { scan.running = true; home.sheet = "widgets"; } }
        TouchButton { Layout.fillWidth: true; implicitHeight: 42; textSize: 14; label: "Layout"; onClicked: home.sheet = "layout" }
        TouchButton { Layout.fillWidth: true; implicitHeight: 42; textSize: 14; label: "Done"; selected: true; onClicked: home.editing = false }
    }
    Row {
        visible: home.editing && home.sheet === ""
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 52
        spacing: 10
        TouchButton { implicitWidth: 132; implicitHeight: 40; textSize: 13; label: "+ Page"; onClicked: home.addPage() }
    }

    // A sheet over the edited home: the apps to add, the widgets, the layout.
    Rectangle {
        id: sheetView
        visible: home.sheet !== ""
        z: 60
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom; top: parent.top; topMargin: 6 }
        color: MobileTheme.background
        radius: MobileTheme.radius(24)
        border.color: MobileTheme.muted
        border.width: 1
        // Touches stay in the sheet.
        TapHandler {}
        DragHandler { target: null; xAxis.enabled: true; yAxis.enabled: false }
        RowLayout {
            id: sheetHeader
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 20 }
            Text {
                Layout.fillWidth: true
                text: home.sheet === "apps" ? "Add apps" : home.sheet === "widgets" ? "Add a widget" : "Home layout"
                color: MobileTheme.foreground
                font.family: MobileTheme.fontFamily
                font.pixelSize: 24
                font.bold: true
            }
            TouchButton { implicitWidth: 92; implicitHeight: 42; textSize: 14; label: "Done"; selected: true; onClicked: home.sheet = "" }
        }
        Flickable {
            anchors { left: parent.left; right: parent.right; top: sheetHeader.bottom; bottom: parent.bottom; margins: 20; topMargin: 16 }
            clip: true
            contentHeight: sheetBody.implicitHeight + 40
            boundsBehavior: Flickable.StopAtBounds
            ColumnLayout {
                id: sheetBody
                width: parent.width
                spacing: 16

                // Apps: tap to add (a tick for those on home already).
                GridLayout {
                    visible: home.sheet === "apps"
                    Layout.fillWidth: true
                    columns: 4
                    rowSpacing: 18
                    columnSpacing: 8
                    Repeater {
                        model: home.sheet === "apps" ? home.apps : []
                        Item {
                            id: pick
                            required property var modelData
                            readonly property bool added: home.onHome(modelData.id) >= 0
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            implicitHeight: 96
                            AppIcon {
                                id: pickTile
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: 58; height: 58
                                icon: pick.modelData.icon || ""
                                name: pick.modelData.name || ""
                                pressed: pickTap.pressed
                                opacity: pick.added ? 0.55 : 1
                                // On the home screen already.
                                Rectangle {
                                    visible: pick.added
                                    anchors { right: parent.right; top: parent.top; margins: -6 }
                                    width: 22; height: 22
                                    radius: MobileTheme.radius(11)
                                    color: MobileTheme.accent
                                    Text { anchors.centerIn: parent; text: "✓"; color: MobileTheme.background; font.pixelSize: 13; font.bold: true }
                                }
                            }
                            Text {
                                anchors { top: pickTile.bottom; topMargin: 6; left: parent.left; right: parent.right }
                                text: pick.modelData.name
                                textFormat: Text.PlainText
                                color: MobileTheme.foreground
                                font.family: MobileTheme.fontFamily; font.pixelSize: 11
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                            }
                            TapHandler { id: pickTap; onTapped: home.addApp(pick.modelData.id) }
                        }
                    }
                }

                // Widgets: one row each, a button for each size it comes in.
                Repeater {
                    model: home.sheet === "widgets" ? Object.keys(home.widgets).sort() : []
                    Rectangle {
                        id: widgetRow
                        required property string modelData
                        readonly property var info: home.widgets[modelData]
                        Layout.fillWidth: true
                        implicitHeight: widgetColumn.implicitHeight + 28
                        radius: MobileTheme.radius(18)
                        color: MobileTheme.surface
                        ColumnLayout {
                            id: widgetColumn
                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 14 }
                            spacing: 8
                            Text {
                                text: widgetRow.info.name
                                color: MobileTheme.foreground
                                font.family: MobileTheme.fontFamily; font.pixelSize: 17; font.bold: true
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: text !== ""
                                text: widgetRow.info.description
                                color: MobileTheme.secondary
                                font.family: MobileTheme.fontFamily; font.pixelSize: 13
                                wrapMode: Text.WordWrap
                            }
                            Flow {
                                Layout.fillWidth: true
                                spacing: 8
                                Repeater {
                                    model: widgetRow.info.sizes
                                    TouchButton {
                                        required property var modelData
                                        implicitWidth: 76; implicitHeight: 40; textSize: 14
                                        label: modelData[0] + " × " + modelData[1]
                                        onClicked: home.addWidget(widgetRow.modelData, modelData[0], modelData[1])
                                    }
                                }
                            }
                        }
                    }
                }
                Text {
                    visible: home.sheet === "widgets" && Object.keys(home.widgets).length === 0
                    text: "No widgets are installed."
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily; font.pixelSize: 14
                }

                // Layout: the grid, icons, labels and the page transition.
                Stepper { visible: home.sheet === "layout"; label: "Columns"; value: home.columns; minimum: 3; maximum: 6; onPicked: v => home.setOption("columns", v) }
                Stepper { visible: home.sheet === "layout"; label: "Rows"; value: home.rows; minimum: 4; maximum: 8; onPicked: v => home.setOption("rows", v) }
                Text {
                    visible: home.sheet === "layout"
                    text: "Icon size"
                    color: MobileTheme.secondary; font.family: MobileTheme.fontFamily; font.pixelSize: 15
                }
                RowLayout {
                    visible: home.sheet === "layout"
                    Layout.fillWidth: true
                    spacing: 8
                    Repeater {
                        model: [{ label: "Small", value: 0.85 }, { label: "Medium", value: 1 }, { label: "Large", value: 1.15 }]
                        TouchButton {
                            required property var modelData
                            Layout.fillWidth: true; implicitHeight: 44; textSize: 14
                            label: modelData.label
                            selected: (home.layout.iconScale || 1) === modelData.value
                            onClicked: home.setOption("iconScale", modelData.value)
                        }
                    }
                }
                RowLayout {
                    visible: home.sheet === "layout"
                    Layout.fillWidth: true
                    Text {
                        Layout.fillWidth: true
                        text: "App names under icons"
                        color: MobileTheme.secondary; font.family: MobileTheme.fontFamily; font.pixelSize: 15
                    }
                    TouchButton {
                        implicitWidth: 84; implicitHeight: 44; textSize: 14
                        label: home.layout.labels ? "On" : "Off"
                        selected: home.layout.labels
                        onClicked: home.setOption("labels", !home.layout.labels)
                    }
                }
                Text {
                    visible: home.sheet === "layout"
                    text: "Page transition"
                    color: MobileTheme.secondary; font.family: MobileTheme.fontFamily; font.pixelSize: 15
                }
                GridLayout {
                    visible: home.sheet === "layout"
                    Layout.fillWidth: true
                    columns: 3
                    rowSpacing: 8
                    columnSpacing: 8
                    Repeater {
                        model: home.transitions
                        TouchButton {
                            required property var modelData
                            Layout.fillWidth: true; implicitHeight: 44; textSize: 14
                            label: modelData.label
                            selected: home.layout.transition === modelData.id
                            onClicked: {
                                home.setOption("transition", modelData.id);
                                // Show it off when there is a page to go to.
                                if (home.pageCount > 1) home.goToPage(home.current === home.pageCount - 1 ? home.current - 1 : home.current + 1);
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- one icon or widget ----
    component Tile: Item {
        id: tile
        required property var modelData
        property int pageIndex: 0
        property bool pageCurrent: false
        readonly property bool isApp: modelData.type === "app"
        readonly property var entry: isApp ? home.entryFor(modelData.app) : null
        readonly property bool moving: home.dragging && home.dragged.uid === modelData.uid
        x: modelData.col * home.cellW
        y: modelData.row * home.cellH
        width: modelData.w * home.cellW
        height: modelData.h * home.cellH
        visible: !isApp || entry !== null
        opacity: moving ? 0.25 : 1
        Behavior on x { enabled: home.editing; NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
        Behavior on y { enabled: home.editing; NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

        // An app: its icon, full size, its name under it.
        AppIcon {
            id: iconTile
            visible: tile.isApp
            anchors.horizontalCenter: parent.horizontalCenter
            y: Math.max(2, (parent.height - height - (home.layout.labels ? 22 : 0)) / 2)
            width: home.iconSize
            height: home.iconSize
            icon: tile.entry && tile.entry.icon ? tile.entry.icon : ""
            name: tile.entry ? (tile.entry.name || "") : ""
            pressed: tap.pressed && !home.editing
        }
        Text {
            visible: tile.isApp && home.layout.labels
            anchors { top: iconTile.bottom; topMargin: 6; left: parent.left; right: parent.right; leftMargin: 2; rightMargin: 2 }
            text: tile.entry ? tile.entry.name : ""
            textFormat: Text.PlainText
            color: MobileTheme.foreground
            font.family: MobileTheme.fontFamily
            font.pixelSize: 12
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            // Legible over any wallpaper.
            style: Text.Raised
            styleColor: Qt.rgba(0, 0, 0, 0.55)
        }

        // A widget: the plugin's Widget.qml.
        Loader {
            id: widget
            anchors.fill: parent
            anchors.margins: 6
            active: !tile.isApp && home.widgetSource(tile.modelData.widget) !== ""
            source: home.widgetSource(tile.modelData.widget)
            enabled: !home.editing
            asynchronous: true
        }
        Binding { target: widget.item; property: "host"; value: home; when: widget.status === Loader.Ready }
        Binding { target: widget.item; property: "active"; value: home.active && tile.pageCurrent; when: widget.status === Loader.Ready }
        Binding { target: widget.item; property: "spanX"; value: tile.modelData.w; when: widget.status === Loader.Ready }
        Binding { target: widget.item; property: "spanY"; value: tile.modelData.h; when: widget.status === Loader.Ready }
        // A widget not installed (any more): a placeholder that can be removed.
        Rectangle {
            anchors.fill: parent
            anchors.margins: 6
            visible: !tile.isApp && !widget.active
            radius: MobileTheme.radius(20)
            color: "transparent"
            border.color: MobileTheme.muted
            border.width: home.editing ? 1 : 0
            Text {
                anchors.centerIn: parent
                visible: home.editing
                text: tile.modelData.widget || ""
                color: MobileTheme.secondary
                font.family: MobileTheme.fontFamily; font.pixelSize: 13
            }
        }
        // While editing, a widget shows its frame.
        Rectangle {
            anchors.fill: parent
            anchors.margins: 4
            visible: !tile.isApp && home.editing
            radius: MobileTheme.radius(22)
            color: "transparent"
            border.color: MobileTheme.accent
            border.width: 2
            opacity: 0.6
        }

        TapHandler {
            id: tap
            longPressThreshold: 0.45
            gesturePolicy: TapHandler.DragThreshold
            onTapped: {
                home.handled();
                if (!home.editing && tile.isApp && tile.entry)
                    home.launchApp(tile.entry, iconTile, iconTile.found ? iconTile.source : "");
            }
            onLongPressed: home.editing = true
        }
        Component.onDestruction: if (home.dragging && home.dragged.uid === modelData.uid) home.cancelDrag()
        DragHandler {
            id: move
            target: null
            enabled: home.editing && home.sheet === ""
            onActiveChanged: {
                if (active) home.beginDrag(tile.modelData, tile.pageIndex, tile, tile.mapToItem(home, centroid.position.x, centroid.position.y));
                else home.endDrag();
            }
            onCentroidChanged: if (active) home.moveDrag(tile.mapToItem(home, centroid.position.x, centroid.position.y))
        }

        // Remove (top left), and for widgets the next size (bottom right).
        // Each takes touches well around itself (the page is shrunk while
        // editing).
        Item {
            visible: home.editing && !tile.moving
            z: 3
            x: (tile.isApp ? iconTile.x : 4) - width / 2 + 6
            y: (tile.isApp ? iconTile.y : 4) - height / 2 + 6
            width: 56; height: 56
            Rectangle {
                anchors.centerIn: parent
                width: 32; height: 32
                radius: MobileTheme.radius(16)
                color: MobileTheme.danger
                Text {
                    anchors.centerIn: parent
                    text: "−"
                    color: MobileTheme.background
                    font.pixelSize: 22; font.bold: true
                }
            }
            TapHandler { onTapped: { home.handled(); home.removeItem(tile.modelData.uid); } }
        }
        Item {
            visible: home.editing && !tile.isApp && !tile.moving && ((home.widgets[tile.modelData.widget] || {}).sizes || []).length > 1
            z: 3
            anchors { right: parent.right; bottom: parent.bottom; rightMargin: -12; bottomMargin: -12 }
            width: 56; height: 56
            Rectangle {
                anchors.centerIn: parent
                width: 34; height: 34
                radius: MobileTheme.radius(17)
                color: MobileTheme.accent
                Text {
                    anchors.centerIn: parent
                    text: "⤡"
                    color: MobileTheme.background
                    font.pixelSize: 19; font.bold: true
                }
            }
            TapHandler { onTapped: { home.handled(); home.resizeWidget(tile.modelData.uid); } }
        }
    }

    component Stepper: RowLayout {
        id: stepper
        property string label: ""
        property int value: 0
        property int minimum: 0
        property int maximum: 10
        signal picked(int value)
        Layout.fillWidth: true
        spacing: 10
        Text {
            Layout.fillWidth: true
            text: stepper.label
            color: MobileTheme.secondary; font.family: MobileTheme.fontFamily; font.pixelSize: 15
        }
        TouchButton { implicitWidth: 48; implicitHeight: 44; label: "−"; enabled: stepper.value > stepper.minimum; onClicked: stepper.picked(stepper.value - 1) }
        Text {
            Layout.preferredWidth: 44
            horizontalAlignment: Text.AlignHCenter
            text: stepper.value
            color: MobileTheme.foreground; font.family: MobileTheme.fontFamily; font.pixelSize: 18
        }
        TouchButton { implicitWidth: 48; implicitHeight: 44; label: "+"; enabled: stepper.value < stepper.maximum; onClicked: stepper.picked(stepper.value + 1) }
    }
}
