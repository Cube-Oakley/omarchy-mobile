import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import OmarchyMobile

// Calendar: the month with each day's events, an agenda, events and their
// editor, and the calendars (new ones, .ics import). omarchy-mobile-calendar
// keeps them as iCalendar files a CalDAV sync can share; the shell shows
// reminders. Opened with "day:YYYY-MM-DD", it starts on that day.
ShellRoot {
    id: root
    property bool ready: false
    property bool allowed: false
    property var requests: []
    property string tab: "month"            // month, agenda, calendars
    property string view: ""                // "", event, edit
    property date month: new Date(new Date().getFullYear(), new Date().getMonth(), 1)
    property string selected: dayKey(new Date())
    property var events: []                 // occurrences in the month's six weeks
    property var agenda: []                 // the next 60 days
    property var calendars: []
    property var event: null                // the event open (helper's get)
    property var occurrence: null           // the occurrence it was opened from
    property var draft: null
    property bool remindersChanged: false
    property bool confirmDelete: false
    property var importable: []
    property string importInto: ""
    property bool newCalendar: false
    property string newColor: palette[0]
    property string message: ""
    property var queue: []
    property double now: Date.now()
    readonly property string home: Quickshell.env("HOME")
    readonly property var palette: ["#7aa2f7", "#9ece6a", "#f7768e", "#e0af68", "#bb9af7", "#7dcfff", "#ff9e64", "#73daca"]
    readonly property var weekdayLetters: ["S", "M", "T", "W", "T", "F", "S"]
    readonly property var weekdayNames: ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]
    // The grid's first day: the Sunday on or before the 1st.
    readonly property date gridStart: new Date(month.getFullYear(), month.getMonth(), 1 - new Date(month.getFullYear(), month.getMonth(), 1).getDay())
    // dayKey -> occurrences on that day (an event on each day it spans).
    readonly property var byDay: {
        const map = {};
        for (const row of events) {
            const first = new Date(row.start * 1000);
            const last = new Date(Math.max(row.start, row.end - 1) * 1000);
            const day = new Date(first.getFullYear(), first.getMonth(), first.getDate());
            for (let guard = 0; day <= last && guard < 62; guard++) {
                const key = dayKey(day);
                (map[key] = map[key] || []).push(row);
                day.setDate(day.getDate() + 1);
            }
        }
        return map;
    }

    function bin(name) { return home + "/.local/bin/" + name; }
    function helper(args) { return [bin("omarchy-mobile-calendar")].concat(args); }
    function run(kind, args, input) {
        if (tool.running) { queue = queue.concat([[kind, args, input]]); return; }
        tool.kind = kind;
        tool.input = input || "";
        tool.command = args;
        tool.running = true;
    }
    function next() {
        if (!queue.length || tool.running) return;
        const item = queue[0];
        queue = queue.slice(1);
        run(item[0], item[1], item[2]);
    }
    function keyboard(action) {
        keyboardTool.command = [bin("omarchy-mobile-keyboard"), action];
        keyboardTool.running = true;
    }
    function pad(n) { return (n < 10 ? "0" : "") + n; }
    function dayKey(d) { return d.getFullYear() + "-" + pad(d.getMonth() + 1) + "-" + pad(d.getDate()); }
    function keyDate(key) { const p = key.split("-"); return new Date(Number(p[0]), Number(p[1]) - 1, Number(p[2])); }
    function hm(seconds) { return Qt.formatDateTime(new Date(seconds * 1000), "HH:mm"); }
    function dayTitle(key) {
        const d = keyDate(key);
        const today = new Date(now);
        const diff = Math.round((d - new Date(today.getFullYear(), today.getMonth(), today.getDate())) / 86400000);
        if (diff === 0) return "Today";
        if (diff === 1) return "Tomorrow";
        if (diff === -1) return "Yesterday";
        return Qt.formatDateTime(d, d.getFullYear() === today.getFullYear() ? "dddd, MMMM d" : "dddd, MMMM d, yyyy");
    }
    function rowTime(row, key) {
        if (row.allDay) return "All day";
        const startKey = dayKey(new Date(row.start * 1000));
        const endKey = dayKey(new Date((row.end - 1) * 1000));
        if (key && startKey !== key && endKey !== key) return "All day";
        if (key && startKey !== key) return "until " + hm(row.end);
        if (key && endKey !== key && row.end > row.start) return hm(row.start) + " →";
        return row.end > row.start ? hm(row.start) + "–" + hm(row.end) : hm(row.start);
    }
    function refresh() {
        const last = new Date(gridStart.getFullYear(), gridStart.getMonth(), gridStart.getDate() + 41);
        run("range", helper(["range", dayKey(gridStart), dayKey(last)]));
        if (tab === "agenda") loadAgenda();
    }
    function loadAgenda() {
        const today = new Date(now);
        run("agenda", helper(["range", dayKey(today), dayKey(new Date(today.getFullYear(), today.getMonth(), today.getDate() + 60))]));
    }
    function showMonth(offset) {
        month = new Date(month.getFullYear(), month.getMonth() + offset, 1);
        refresh();
    }
    function goToday() {
        const today = new Date(now);
        month = new Date(today.getFullYear(), today.getMonth(), 1);
        selected = dayKey(today);
        refresh();
    }
    function open(row) {
        occurrence = row;
        event = null;
        confirmDelete = false;
        view = "event";
        run("get", helper(["get", row.uid]));
    }
    function repeatText(repeat) {
        if (!repeat || !repeat.freq) return "";
        if (repeat.custom) return "Repeats (a rule kept as it came)";
        const n = repeat.interval || 1;
        const unit = {daily: "day", weekly: "week", monthly: "month", yearly: "year"}[repeat.freq] || repeat.freq;
        let text = n > 1 ? "Every " + n + " " + unit + "s" : "Every " + unit;
        if (repeat.freq === "weekly" && repeat.days && repeat.days.length)
            text += " on " + repeat.days.map(d => weekdayNames[d]).join(", ");
        if (repeat.count) text += ", " + repeat.count + " times";
        else if (repeat.until) text += ", until " + Qt.formatDateTime(keyDate(repeat.until), "MMM d, yyyy");
        return text;
    }
    function reminderName(minutes, allDay) {
        if (minutes === null || minutes === undefined) return "None";
        if (allDay) {
            if (minutes === -540) return "On the day at 09:00";
            if (minutes === 900) return "The day before at 09:00";
        }
        if (minutes === 0) return "At the start";
        if (minutes % 1440 === 0) return minutes / 1440 + (minutes === 1440 ? " day" : " days") + " before";
        if (minutes % 60 === 0) return minutes / 60 + (minutes === 60 ? " hour" : " hours") + " before";
        return minutes + " min before";
    }
    function reminderOptions(allDay) {
        const values = allDay ? [null, -540, 900] : [null, 0, 5, 10, 15, 30, 60, 1440];
        return values.map(v => ({value: v === null ? "none" : String(v), label: reminderName(v, allDay)}));
    }
    // Event times as the editor holds them: "YYYY-MM-DDTHH:MM" or "YYYY-MM-DD".
    function draftDate(text) { return text.slice(0, 10); }
    function draftTime(text) { return text.length > 10 ? text.slice(11, 16) : "09:00"; }
    function toMs(dateKey, time) { const d = keyDate(dateKey); const t = time.split(":"); d.setHours(Number(t[0]), Number(t[1])); return d.getTime(); }
    function fromMs(ms) { const d = new Date(ms); return dayKey(d) + "T" + pad(d.getHours()) + ":" + pad(d.getMinutes()); }
    function newEvent() {
        const day = keyDate(selected);
        const today = new Date(now);
        let start = new Date(day.getFullYear(), day.getMonth(), day.getDate(), 9, 0);
        if (dayKey(day) === dayKey(today)) start = new Date(today.getFullYear(), today.getMonth(), today.getDate(), today.getHours() + 1, 0);
        edit({uid: "", calendar: calendars.length ? calendars[0].id : "", title: "", location: "", notes: "", allDay: false,
              start: fromMs(start.getTime()), end: fromMs(start.getTime() + 3600000), repeat: null, reminders: [10]});
        remindersChanged = true;
    }
    function edit(source) {
        draft = JSON.parse(JSON.stringify(source));
        remindersChanged = false;
        confirmDelete = false;
        titleField.text = draft.title || "";
        locationField.text = draft.location || "";
        notesField.text = draft.notes || "";
        view = "edit";
    }
    function setDraft(change) { draft = Object.assign({}, draft, change); }
    // A new start keeps the event's length.
    function moveStart(dateKey, time) {
        if (draft.allDay) {
            const length = keyDate(draftDate(draft.end)) - keyDate(draftDate(draft.start));
            setDraft({start: dateKey, end: dayKey(new Date(keyDate(dateKey).getTime() + length))});
            return;
        }
        const length = toMs(draftDate(draft.end), draftTime(draft.end)) - toMs(draftDate(draft.start), draftTime(draft.start));
        const start = toMs(dateKey, time);
        setDraft({start: fromMs(start), end: fromMs(start + Math.max(0, length))});
    }
    function moveEnd(dateKey, time) {
        if (draft.allDay) setDraft({end: dateKey < draftDate(draft.start) ? draftDate(draft.start) : dateKey});
        else {
            const end = Math.max(toMs(dateKey, time), toMs(draftDate(draft.start), draftTime(draft.start)));
            setDraft({end: fromMs(end)});
        }
    }
    function setAllDay(on) {
        if (on === draft.allDay) return;
        if (on) setDraft({allDay: true, start: draftDate(draft.start), end: draftDate(draft.end), reminders: []});
        else setDraft({allDay: false, start: draftDate(draft.start) + "T09:00", end: draftDate(draft.start) + "T10:00", reminders: [10]});
        remindersChanged = true;
    }
    function repeatOptions() {
        const day = keyDate(draftDate(draft.start));
        const weekday = (day.getDay() + 6) % 7;
        const options = [{value: "none", label: "Does not repeat"}, {value: "daily", label: "Every day"},
                         {value: "weekly", label: "Every week on " + Qt.formatDateTime(day, "dddd")},
                         {value: "weekdays", label: "Every weekday (Mon–Fri)"},
                         {value: "monthly", label: "Every month on the " + day.getDate() + ordinal(day.getDate())},
                         {value: "yearly", label: "Every year on " + Qt.formatDateTime(day, "MMMM d")}];
        if (draft.repeat && draft.repeat.custom) options.push({value: "custom", label: "As it came (" + draft.repeat.text + ")"});
        return options;
    }
    function repeatValue() {
        const r = draft ? draft.repeat : null;
        if (!r || !r.freq) return "none";
        if (r.custom) return "custom";
        if (r.freq === "weekly" && r.days && r.days.length === 5 && r.days.every(d => d < 5)) return "weekdays";
        return r.freq;
    }
    function setRepeat(value) {
        const weekday = (keyDate(draftDate(draft.start)).getDay() + 6) % 7;
        if (value === "custom") return;
        if (value === "none") setDraft({repeat: null});
        else if (value === "weekdays") setDraft({repeat: {freq: "weekly", interval: 1, days: [0, 1, 2, 3, 4]}});
        else if (value === "weekly") setDraft({repeat: {freq: "weekly", interval: 1, days: [weekday]}});
        else setDraft({repeat: {freq: value, interval: 1}});
    }
    function ordinal(n) { return (n % 100 >= 11 && n % 100 <= 13) ? "th" : ({1: "st", 2: "nd", 3: "rd"})[n % 10] || "th"; }
    function save() {
        const copy = Object.assign({}, draft, {title: titleField.text.trim(), location: locationField.text.trim(),
                                               notes: notesField.text.trim()});
        if (!remindersChanged) delete copy.reminders;
        run("save", helper(["save"]), JSON.stringify(copy));
        keyboard("hide");
    }
    function goBack() {
        if (datePicker.open) { datePicker.open = false; return; }
        if (timePicker.open) { timePicker.open = false; return; }
        if (titleField.editing || locationField.editing || notesField.editing || nameField.editing) {
            titleField.editing = locationField.editing = notesField.editing = nameField.editing = false;
            keyboard("hide");
            return;
        }
        if (view === "edit") { view = draft && draft.uid && event ? "event" : ""; draft = null; return; }
        if (view === "event") { view = ""; event = null; return; }
        if (newCalendar) { newCalendar = false; return; }
        Qt.quit();
    }
    function begin() {
        const arg = Quickshell.env("OMARCHY_MOBILE_APP_ARG") || "";
        const match = /^day:(\d{4}-\d{2}-\d{2})$/.exec(arg);
        if (match) {
            selected = match[1];
            const d = keyDate(selected);
            month = new Date(d.getFullYear(), d.getMonth(), 1);
        }
        refresh();
    }
    Component.onCompleted: run("grants", [bin("omarchy-mobile-app"), "status", "calendar"])

    Process {
        id: tool
        property string kind: ""
        property string input: ""
        stdout: StdioCollector {}
        stderr: StdioCollector {}
        stdinEnabled: true
        onStarted: { if (tool.input) write(tool.input); stdinEnabled = false; }
        onExited: {
            tool.stdinEnabled = true;
            let result = {};
            try { result = JSON.parse(stdout.text); }
            catch (e) { result = {ok: false, error: (stderr.text || "").trim().split("\n").pop() || "The calendar helper did not answer"}; }
            const kind = tool.kind;
            if (kind === "grants") {
                const items = result.permissions || [];
                root.requests = items;
                root.allowed = items.length > 0 && items.every(p => p.granted);
                root.ready = result.ok !== false;
                if (root.allowed) root.begin();
            } else if (kind === "grant") {
                if (result.ok !== false && (result.permissions || []).every(p => p.granted)) {
                    root.allowed = true;
                    root.begin();
                }
            } else if (result.ok === false) {
                root.message = result.error || "That did not work";
            } else {
                root.message = "";
                if (kind === "range") { root.events = result.events || []; root.calendars = result.calendars || []; }
                else if (kind === "agenda") root.agenda = result.events || [];
                else if (kind === "get") root.event = result.event;
                else if (kind === "save") {
                    const start = root.keyDate(result.event.start.slice(0, 10));
                    root.selected = root.dayKey(start);
                    root.month = new Date(start.getFullYear(), start.getMonth(), 1);
                    root.draft = null;
                    root.event = null;
                    root.view = "";
                    root.refresh();
                }
                else if (kind === "deleted") { root.view = ""; root.event = null; root.refresh(); }
                else if (kind === "calendarChanged") root.refresh();
                else if (kind === "importable") root.importable = result.files || [];
                else if (kind === "imported") { root.message = result.imported + " events imported"; root.refresh(); }
            }
            root.next();
        }
    }
    Process { id: keyboardTool }
    Timer { interval: 60000; repeat: true; running: true; onTriggered: root.now = Date.now() }

    AppWindow {
        compact: true
        pageMargin: 16
        kicker: ""
        appTitle: "Calendar"
        heading: root.view === "edit" ? (root.draft && root.draft.uid ? "Edit event" : "New event")
               : root.view === "event" ? "" : ({month: "Calendar", agenda: "Agenda", calendars: "Calendars"})[root.tab]
        backVisible: root.allowed && root.view !== ""
        onBackClicked: root.goBack()

        GrantPage {
            anchors.fill: parent
            visible: root.ready && !root.allowed
            requests: root.requests
            onAllowed: {
                for (let i = 0; i < root.requests.length; i++)
                    root.run("grant", [root.bin("omarchy-mobile-app"), "grant", "calendar", root.requests[i].id]);
            }
            onDenied: Qt.quit()
        }

        ColumnLayout {
            anchors.fill: parent
            visible: root.allowed
            spacing: 0

            Text {
                Layout.fillWidth: true
                visible: root.message.length > 0
                text: root.message
                color: MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 14
                wrapMode: Text.WordWrap
                bottomPadding: 8
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                // The month.
                ColumnLayout {
                    anchors.fill: parent
                    visible: root.tab === "month" && root.view === ""
                    spacing: 8
                    RowLayout {
                        Layout.fillWidth: true
                        TouchButton { implicitWidth: 44; implicitHeight: 44; label: "\u{F0141}"; onClicked: root.showMonth(-1) }
                        Text {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            text: Qt.formatDateTime(root.month, "MMMM yyyy")
                            color: MobileTheme.foreground
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 20
                            font.bold: true
                        }
                        TouchButton { implicitWidth: 44; implicitHeight: 44; label: "\u{F0142}"; onClicked: root.showMonth(1) }
                        TouchButton { implicitWidth: 72; implicitHeight: 44; label: "Today"; textSize: 14; onClicked: root.goToday() }
                    }
                    Item {
                        Layout.fillWidth: true
                        implicitHeight: grid.implicitHeight
                        GridLayout {
                            id: grid
                            width: parent.width
                            columns: 7
                            columnSpacing: 2
                            rowSpacing: 2
                            Repeater {
                                model: root.weekdayLetters
                                Text {
                                    required property string modelData
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: 1
                                    horizontalAlignment: Text.AlignHCenter
                                    text: modelData
                                    color: MobileTheme.secondary
                                    font.family: MobileTheme.fontFamily
                                    font.pixelSize: 13
                                }
                            }
                            Repeater {
                                model: 42
                                Rectangle {
                                    id: cell
                                    required property int index
                                    readonly property date day: new Date(root.gridStart.getFullYear(), root.gridStart.getMonth(), root.gridStart.getDate() + index)
                                    readonly property string key: root.dayKey(day)
                                    readonly property bool today: key === root.dayKey(new Date(root.now))
                                    readonly property bool chosen: key === root.selected
                                    readonly property var rows: root.byDay[key] || []
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: 1
                                    implicitHeight: 52
                                    // Today fills its tile, as in the shade; the chosen day is
                                    // outlined (in the text colour on today's fill).
                                    radius: MobileTheme.radius(12)
                                    color: today ? MobileTheme.accent : chosen ? MobileTheme.selection : "transparent"
                                    border.width: chosen ? 2 : 0
                                    border.color: today ? MobileTheme.foreground : MobileTheme.accent
                                    Text {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        y: 8
                                        text: cell.day.getDate()
                                        color: cell.today ? MobileTheme.background : MobileTheme.foreground
                                        opacity: cell.day.getMonth() === root.month.getMonth() ? 1 : 0.35
                                        font.family: MobileTheme.fontFamily
                                        font.pixelSize: 15
                                        font.bold: cell.today
                                    }
                                    Row {
                                        anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 6 }
                                        spacing: 3
                                        Repeater {
                                            model: cell.rows.slice(0, 3)
                                            Rectangle {
                                                required property var modelData
                                                width: 6; height: 6
                                                radius: MobileTheme.radius(3)
                                                // On today's accent fill a calendar's colour can vanish.
                                                color: cell.today ? MobileTheme.background : modelData.color
                                                opacity: cell.day.getMonth() === root.month.getMonth() ? 1 : 0.4
                                            }
                                        }
                                    }
                                    TapHandler { onTapped: root.selected = cell.key; onDoubleTapped: { root.selected = cell.key; root.newEvent(); } }
                                }
                            }
                        }
                        // A sideways swipe turns the month.
                        DragHandler {
                            target: null
                            xAxis.enabled: true
                            yAxis.enabled: false
                            onActiveChanged: if (!active && Math.abs(translation.x) > 60) root.showMonth(translation.x < 0 ? 1 : -1)
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        Layout.topMargin: 4
                        text: root.dayTitle(root.selected)
                        color: MobileTheme.accent
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 15
                        font.bold: true
                    }
                    ListView {
                        id: dayList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 8
                        bottomMargin: 88
                        model: (root.byDay[root.selected] || []).slice().sort((a, b) => (b.allDay - a.allDay) || (a.start - b.start))
                        delegate: EventCard { dayKey: root.selected }
                    }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.bottomMargin: 120
                        visible: dayList.count === 0
                        text: "Nothing planned"
                        color: MobileTheme.secondary
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 15
                    }
                }

                // The agenda.
                ListView {
                    id: agendaList
                    anchors.fill: parent
                    visible: root.tab === "agenda" && root.view === ""
                    clip: true
                    spacing: 8
                    bottomMargin: 88
                    model: root.agenda.map(row => Object.assign({section: root.dayKey(new Date(Math.max(row.start * 1000, new Date(root.now).setHours(0, 0, 0, 0))))}, row))
                    section.property: "section"
                    section.delegate: Text {
                        required property string section
                        width: ListView.view.width
                        topPadding: 12
                        bottomPadding: 4
                        text: root.dayTitle(section)
                        color: MobileTheme.accent
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 15
                        font.bold: true
                    }
                    delegate: EventCard { dayKey: modelData.section }
                }
                Text {
                    anchors.centerIn: parent
                    visible: root.tab === "agenda" && root.view === "" && agendaList.count === 0
                    text: "Nothing in the next 60 days"
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 15
                }

                // The calendars.
                Flickable {
                    anchors.fill: parent
                    visible: root.tab === "calendars" && root.view === ""
                    contentHeight: calendarsColumn.height
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ColumnLayout {
                        id: calendarsColumn
                        width: parent.width
                        spacing: 10
                        Repeater {
                            model: root.calendars
                            Rectangle {
                                id: calendarRow
                                required property var modelData
                                Layout.fillWidth: true
                                implicitHeight: 64
                                radius: MobileTheme.radius(16)
                                color: MobileTheme.surface
                                Rectangle {
                                    anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
                                    width: 18; height: 18
                                    radius: MobileTheme.radius(9)
                                    color: calendarRow.modelData.color
                                }
                                Column {
                                    anchors { left: parent.left; leftMargin: 48; verticalCenter: parent.verticalCenter; right: removeCalendar.left }
                                    Text {
                                        text: calendarRow.modelData.name
                                        color: MobileTheme.foreground
                                        font.family: MobileTheme.fontFamily
                                        font.pixelSize: 17
                                    }
                                    Text {
                                        text: calendarRow.modelData.events + (calendarRow.modelData.events === 1 ? " event" : " events")
                                        color: MobileTheme.secondary
                                        font.family: MobileTheme.fontFamily
                                        font.pixelSize: 13
                                    }
                                }
                                Text {
                                    id: removeCalendar
                                    anchors { right: parent.right; rightMargin: 16; verticalCenter: parent.verticalCenter }
                                    visible: root.calendars.length > 1
                                    property bool armed: false
                                    text: armed ? "Delete it" : "\u{F09E7}"
                                    color: armed ? MobileTheme.danger : MobileTheme.secondary
                                    font.family: MobileTheme.fontFamily
                                    font.pixelSize: armed ? 14 : 20
                                    TapHandler {
                                        margin: 12
                                        onTapped: {
                                            if (!removeCalendar.armed) { removeCalendar.armed = true; return; }
                                            root.run("calendarChanged", root.helper(["calendar-delete", calendarRow.modelData.id]));
                                        }
                                    }
                                }
                            }
                        }
                        // A new calendar.
                        TouchButton {
                            Layout.fillWidth: true
                            visible: !root.newCalendar
                            label: "\u{F0415}  New calendar"
                            onClicked: { root.newCalendar = true; nameField.text = ""; }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: root.newCalendar
                            spacing: 10
                            TouchTextField {
                                id: nameField
                                Layout.fillWidth: true
                                placeholderText: "Calendar name"
                                onEditingRequested: { editing = true; forceActiveFocus(); root.keyboard("show"); }
                            }
                            Row {
                                spacing: 10
                                Repeater {
                                    model: root.palette
                                    Rectangle {
                                        required property string modelData
                                        width: 32; height: 32
                                        radius: MobileTheme.radius(16)
                                        color: modelData
                                        border.width: root.newColor === modelData ? 3 : 0
                                        border.color: MobileTheme.foreground
                                        TapHandler { margin: 4; onTapped: root.newColor = parent.modelData }
                                    }
                                }
                            }
                            TouchButton {
                                Layout.fillWidth: true
                                label: "Add calendar"
                                selected: true
                                onClicked: {
                                    if (!nameField.text.trim()) return;
                                    root.run("calendarChanged", root.helper(["calendar-save"]), JSON.stringify({name: nameField.text.trim(), color: root.newColor}));
                                    root.newCalendar = false;
                                    nameField.editing = false;
                                    root.keyboard("hide");
                                }
                            }
                        }
                        // Import.
                        Text {
                            Layout.fillWidth: true
                            Layout.topMargin: 16
                            text: "IMPORT AN .ICS FILE"
                            color: MobileTheme.accent
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 12
                            font.letterSpacing: 2
                            font.bold: true
                        }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: root.importable.length ? "Tap a file to add its events to the calendar chosen here."
                                : "Files ending in .ics in Downloads, Documents or the home folder show here: an export from Google Calendar or Outlook."
                            color: MobileTheme.secondary
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 13
                        }
                        Flow {
                            Layout.fillWidth: true
                            visible: root.importable.length > 0 && root.calendars.length > 1
                            spacing: 8
                            Repeater {
                                model: root.calendars
                                Rectangle {
                                    required property var modelData
                                    readonly property bool on: (root.importInto || root.calendars[0].id) === modelData.id
                                    width: chipText.implicitWidth + 28; height: 36
                                    radius: MobileTheme.radius(18)
                                    color: on ? modelData.color : MobileTheme.surface
                                    Text {
                                        id: chipText
                                        anchors.centerIn: parent
                                        text: parent.modelData.name
                                        color: parent.on ? MobileTheme.background : MobileTheme.foreground
                                        font.family: MobileTheme.fontFamily
                                        font.pixelSize: 14
                                    }
                                    TapHandler { onTapped: root.importInto = parent.modelData.id }
                                }
                            }
                        }
                        Repeater {
                            model: root.importable
                            SettingsRow {
                                required property var modelData
                                Layout.fillWidth: true
                                label: modelData.name
                                value: modelData.modified
                                onClicked: root.run("imported", root.helper(["import", modelData.path, root.importInto || root.calendars[0].id]))
                            }
                        }
                        Item { implicitHeight: 24 }
                    }
                }

                // An event.
                Flickable {
                    anchors.fill: parent
                    visible: root.view === "event"
                    contentHeight: eventColumn.height
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ColumnLayout {
                        id: eventColumn
                        width: parent.width
                        spacing: 14
                        readonly property var row: root.occurrence
                        readonly property var full: root.event
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12
                            Rectangle {
                                Layout.alignment: Qt.AlignTop
                                Layout.topMargin: 10
                                width: 16; height: 16
                                radius: MobileTheme.radius(8)
                                color: eventColumn.full ? eventColumn.full.color : (eventColumn.row ? eventColumn.row.color : MobileTheme.accent)
                            }
                            Text {
                                Layout.fillWidth: true
                                text: eventColumn.full ? (eventColumn.full.title || "(No title)") : (eventColumn.row ? eventColumn.row.title : "")
                                textFormat: Text.PlainText
                                wrapMode: Text.WordWrap
                                color: MobileTheme.foreground
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 26
                                font.bold: true
                            }
                        }
                        Detail {
                            glyph: "\u{F0150}"
                            text: {
                                const row = eventColumn.row;
                                if (!row) return "";
                                const first = Qt.formatDateTime(new Date(row.start * 1000), "dddd, MMMM d");
                                if (row.allDay) {
                                    const last = new Date((row.end - 86400) * 1000);
                                    return root.dayKey(last) === root.dayKey(new Date(row.start * 1000)) ? first + "\nAll day"
                                        : first + " – " + Qt.formatDateTime(last, "dddd, MMMM d") + "\nAll day";
                                }
                                return first + "\n" + root.rowTime(row, "");
                            }
                        }
                        Detail { glyph: "\u{F0456}"; text: eventColumn.full ? root.repeatText(eventColumn.full.repeat) : "" }
                        Detail { glyph: "\u{F07D9}"; text: eventColumn.full ? eventColumn.full.location : "" }
                        Detail {
                            glyph: "\u{F009C}"
                            text: eventColumn.full && eventColumn.full.reminders.length
                                  ? eventColumn.full.reminders.map(m => root.reminderName(m, eventColumn.full.allDay)).join("\n") : ""
                        }
                        Detail { glyph: "\u{F00F6}"; text: eventColumn.full ? eventColumn.full.calendarName : "" }
                        Detail { glyph: "\u{F09ED}"; text: eventColumn.full ? eventColumn.full.notes : "" }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 8
                            spacing: 10
                            visible: eventColumn.full !== null
                            TouchButton {
                                Layout.fillWidth: true
                                label: root.confirmDelete ? (eventColumn.row && eventColumn.row.recurring ? "This one" : "Delete it") : "Delete"
                                onClicked: {
                                    if (!root.confirmDelete) { root.confirmDelete = true; return; }
                                    if (eventColumn.row.recurring)
                                        root.run("deleted", root.helper(["skip", eventColumn.row.uid, String(eventColumn.row.original)]));
                                    else root.run("deleted", root.helper(["delete", eventColumn.row.uid]));
                                }
                            }
                            TouchButton {
                                Layout.fillWidth: true
                                visible: root.confirmDelete && eventColumn.row !== null && eventColumn.row.recurring
                                label: "Every one"
                                onClicked: root.run("deleted", root.helper(["delete", eventColumn.row.uid]))
                            }
                            TouchButton {
                                Layout.fillWidth: true
                                visible: !root.confirmDelete
                                label: "Edit"
                                selected: true
                                onClicked: root.edit(root.event)
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: root.confirmDelete && eventColumn.row !== null && eventColumn.row.recurring
                            wrapMode: Text.WordWrap
                            text: "Delete only this occurrence, or the event with all its repeats?"
                            color: MobileTheme.secondary
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 13
                        }
                    }
                }

                // The editor.
                Flickable {
                    anchors.fill: parent
                    visible: root.view === "edit"
                    contentHeight: editColumn.height + 24
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ColumnLayout {
                        id: editColumn
                        width: parent.width
                        spacing: 12
                        TouchTextField {
                            id: titleField
                            Layout.fillWidth: true
                            placeholderText: "Title"
                            font.pixelSize: 20
                            onEditingRequested: { editing = true; forceActiveFocus(); root.keyboard("show"); }
                        }
                        SettingsRow {
                            Layout.fillWidth: true
                            label: "All day"
                            value: root.draft && root.draft.allDay ? "On" : "Off"
                            selected: root.draft !== null && root.draft.allDay
                            onClicked: root.setAllDay(!root.draft.allDay)
                        }
                        Repeater {
                            model: ["start", "end"]
                            RowLayout {
                                id: whenRow
                                required property string modelData
                                Layout.fillWidth: true
                                spacing: 8
                                readonly property string value: root.draft ? root.draft[modelData] : ""
                                Text {
                                    Layout.preferredWidth: 64
                                    text: whenRow.modelData === "start" ? "Starts" : "Ends"
                                    color: MobileTheme.secondary
                                    font.family: MobileTheme.fontFamily
                                    font.pixelSize: 15
                                }
                                TouchButton {
                                    Layout.fillWidth: true
                                    implicitHeight: 48
                                    label: whenRow.value ? Qt.formatDateTime(root.keyDate(root.draftDate(whenRow.value)), "ddd, MMM d, yyyy") : ""
                                    onClicked: datePicker.show(whenRow.modelData)
                                }
                                TouchButton {
                                    implicitWidth: 92
                                    implicitHeight: 48
                                    visible: root.draft !== null && !root.draft.allDay
                                    label: whenRow.value ? root.draftTime(whenRow.value) : ""
                                    onClicked: timePicker.show(whenRow.modelData)
                                }
                            }
                        }
                        SettingsRow {
                            Layout.fillWidth: true
                            label: "Repeat"
                            value: root.draft ? (root.repeatOptions().find(o => o.value === root.repeatValue()) || {label: ""}).label : ""
                            onClicked: repeatPicker.open = true
                        }
                        SettingsRow {
                            Layout.fillWidth: true
                            label: "Reminder"
                            value: root.draft ? (root.draft.reminders && root.draft.reminders.length
                                                 ? root.reminderName(root.draft.reminders[0], root.draft.allDay) : "None") : ""
                            onClicked: reminderPicker.open = true
                        }
                        SettingsRow {
                            Layout.fillWidth: true
                            visible: root.calendars.length > 1
                            label: "Calendar"
                            value: root.draft ? ((root.calendars.find(c => c.id === root.draft.calendar) || {name: ""}).name) : ""
                            onClicked: calendarPicker.open = true
                        }
                        TouchTextField {
                            id: locationField
                            Layout.fillWidth: true
                            placeholderText: "\u{F07D9}  Location"
                            onEditingRequested: { editing = true; forceActiveFocus(); root.keyboard("show"); }
                        }
                        TouchTextField {
                            id: notesField
                            Layout.fillWidth: true
                            placeholderText: "Notes"
                            onEditingRequested: { editing = true; forceActiveFocus(); root.keyboard("show"); }
                        }
                        TouchButton {
                            Layout.fillWidth: true
                            label: "Save"
                            selected: true
                            onClicked: root.save()
                        }
                    }
                }

                AddButton {
                    anchors { right: parent.right; bottom: parent.bottom; bottomMargin: 16 }
                    visible: root.view === "" && root.tab !== "calendars"
                    onClicked: root.newEvent()
                }
            }

            // Tabs.
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 8
                visible: root.view === ""
                spacing: 0
                Repeater {
                    model: [{id: "month", glyph: "\u{F0E17}", name: "Month"}, {id: "agenda", glyph: "\u{F00F6}", name: "Agenda"},
                            {id: "calendars", glyph: "\u{F00F1}", name: "Calendars"}]
                    Item {
                        id: tabItem
                        required property var modelData
                        readonly property bool current: root.tab === modelData.id
                        Layout.fillWidth: true
                        implicitHeight: 60
                        Rectangle {
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: 2
                            width: 56; height: 30
                            radius: MobileTheme.radius(15)
                            color: tabItem.current ? MobileTheme.selection : "transparent"
                            Text {
                                anchors.centerIn: parent
                                text: tabItem.modelData.glyph
                                color: tabItem.current ? MobileTheme.accent : MobileTheme.secondary
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 20
                            }
                        }
                        Text {
                            anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 4 }
                            text: tabItem.modelData.name
                            color: tabItem.current ? MobileTheme.foreground : MobileTheme.secondary
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 12
                            font.bold: tabItem.current
                        }
                        TapHandler {
                            onTapped: {
                                root.tab = tabItem.modelData.id;
                                if (root.tab === "agenda") root.loadAgenda();
                                if (root.tab === "calendars") root.run("importable", root.helper(["importable"]));
                            }
                        }
                    }
                }
            }
        }

        // Pickers over the editor.
        ChoicePicker {
            id: repeatPicker
            title: "Repeat"
            current: root.draft ? root.repeatValue() : "none"
            options: root.draft ? root.repeatOptions() : []
            onChosen: value => root.setRepeat(value)
        }
        ChoicePicker {
            id: reminderPicker
            title: "Reminder"
            current: root.draft && root.draft.reminders && root.draft.reminders.length ? String(root.draft.reminders[0]) : "none"
            options: root.draft ? root.reminderOptions(root.draft.allDay) : []
            onChosen: value => { root.setDraft({reminders: value === "none" ? [] : [Number(value)]}); root.remindersChanged = true; }
        }
        ChoicePicker {
            id: calendarPicker
            title: "Calendar"
            current: root.draft ? root.draft.calendar : ""
            options: root.calendars.map(c => ({value: c.id, label: c.name}))
            onChosen: value => root.setDraft({calendar: value})
        }
        MenuOverlay {
            id: datePicker
            property string which: "start"
            property date shown: new Date()
            scrim: Qt.rgba(MobileTheme.background.r, MobileTheme.background.g, MobileTheme.background.b, 0.92)
            onDismissed: open = false
            function show(which) {
                datePicker.which = which;
                const d = root.keyDate(root.draftDate(root.draft[which]));
                shown = new Date(d.getFullYear(), d.getMonth(), 1);
                open = true;
            }
            readonly property date first: new Date(shown.getFullYear(), shown.getMonth(), 1 - new Date(shown.getFullYear(), shown.getMonth(), 1).getDay())
            ColumnLayout {
                anchors.centerIn: parent
                width: parent.width - 24
                spacing: 8
                RowLayout {
                    Layout.fillWidth: true
                    TouchButton { implicitWidth: 44; implicitHeight: 44; label: "\u{F0141}"; onClicked: datePicker.shown = new Date(datePicker.shown.getFullYear(), datePicker.shown.getMonth() - 1, 1) }
                    Text {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        text: Qt.formatDateTime(datePicker.shown, "MMMM yyyy")
                        color: MobileTheme.foreground
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 18
                        font.bold: true
                    }
                    TouchButton { implicitWidth: 44; implicitHeight: 44; label: "\u{F0142}"; onClicked: datePicker.shown = new Date(datePicker.shown.getFullYear(), datePicker.shown.getMonth() + 1, 1) }
                }
                GridLayout {
                    Layout.fillWidth: true
                    columns: 7
                    columnSpacing: 2
                    rowSpacing: 2
                    Repeater {
                        model: root.weekdayLetters
                        Text {
                            required property string modelData
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            horizontalAlignment: Text.AlignHCenter
                            text: modelData
                            color: MobileTheme.secondary
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 13
                        }
                    }
                    Repeater {
                        model: 42
                        Rectangle {
                            id: pickCell
                            required property int index
                            readonly property date day: new Date(datePicker.first.getFullYear(), datePicker.first.getMonth(), datePicker.first.getDate() + index)
                            readonly property bool chosen: root.draft !== null && root.dayKey(day) === root.draftDate(root.draft[datePicker.which])
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            implicitHeight: 44
                            radius: MobileTheme.radius(22)
                            color: chosen ? MobileTheme.accent : (pickTap.pressed ? MobileTheme.selection : "transparent")
                            Text {
                                anchors.centerIn: parent
                                text: pickCell.day.getDate()
                                color: pickCell.chosen ? MobileTheme.background : MobileTheme.foreground
                                opacity: pickCell.day.getMonth() === datePicker.shown.getMonth() ? 1 : 0.35
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 16
                            }
                            TapHandler {
                                id: pickTap
                                onTapped: {
                                    const key = root.dayKey(pickCell.day);
                                    if (datePicker.which === "start") root.moveStart(key, root.draftTime(root.draft.start));
                                    else root.moveEnd(key, root.draftTime(root.draft.end));
                                    datePicker.open = false;
                                }
                            }
                        }
                    }
                }
            }
        }
        MenuOverlay {
            id: timePicker
            property string which: "start"
            scrim: Qt.rgba(MobileTheme.background.r, MobileTheme.background.g, MobileTheme.background.b, 0.92)
            onDismissed: open = false
            function show(which) {
                timePicker.which = which;
                const time = root.draftTime(root.draft[which]).split(":");
                hourWheel.currentIndex = Number(time[0]);
                minuteWheel.currentIndex = Math.round(Number(time[1]) / 5) % 12;
                open = true;
            }
            ColumnLayout {
                anchors.centerIn: parent
                spacing: 16
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: timePicker.which === "start" ? "STARTS" : "ENDS"
                    color: MobileTheme.accent
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 12
                    font.letterSpacing: 2.2
                    font.bold: true
                }
                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 8
                    Wheel { id: hourWheel; size: 24; step: 1 }
                    Text {
                        text: ":"
                        color: MobileTheme.foreground
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 48
                        font.weight: Font.Light
                    }
                    Wheel { id: minuteWheel; size: 12; step: 5 }
                }
                TouchButton {
                    Layout.fillWidth: true
                    label: "Done"
                    selected: true
                    onClicked: {
                        const time = root.pad(hourWheel.currentIndex) + ":" + root.pad(minuteWheel.currentIndex * 5);
                        const which = timePicker.which;
                        if (which === "start") root.moveStart(root.draftDate(root.draft.start), time);
                        else root.moveEnd(root.draftDate(root.draft.end), time);
                        timePicker.open = false;
                    }
                }
            }
        }
    }

    component EventCard: Rectangle {
        id: card
        required property var modelData
        property string dayKey: ""
        width: ListView.view ? ListView.view.width : 0
        height: 64
        radius: MobileTheme.radius(14)
        color: cardTap.pressed ? MobileTheme.selection : MobileTheme.surface
        Rectangle {
            anchors { left: parent.left; top: parent.top; bottom: parent.bottom; margins: 8 }
            width: 4
            radius: 2
            color: card.modelData.color
        }
        Column {
            anchors { left: parent.left; leftMargin: 22; right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
            spacing: 2
            Text {
                width: parent.width
                text: card.modelData.title
                textFormat: Text.PlainText
                color: MobileTheme.foreground
                font.family: MobileTheme.fontFamily
                font.pixelSize: 16
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                text: root.rowTime(card.modelData, card.dayKey) + (card.modelData.location ? " · " + card.modelData.location : "")
                    + (card.modelData.recurring ? "  \u{F0456}" : "")
                textFormat: Text.PlainText
                color: MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 13
                elide: Text.ElideRight
            }
        }
        TapHandler { id: cardTap; onTapped: root.open(card.modelData) }
    }

    component Detail: RowLayout {
        id: detail
        property string glyph: ""
        property string text: ""
        Layout.fillWidth: true
        visible: text.length > 0
        spacing: 14
        Text {
            Layout.alignment: Qt.AlignTop
            text: detail.glyph
            color: MobileTheme.secondary
            font.family: MobileTheme.fontFamily
            font.pixelSize: 20
        }
        Text {
            Layout.fillWidth: true
            text: detail.text
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: MobileTheme.foreground
            font.family: MobileTheme.fontFamily
            font.pixelSize: 16
        }
    }

    // A wheel of numbers (hours; minutes in steps of five) that wraps.
    component Wheel: Tumbler {
        id: wheel
        property int size: 24
        property int step: 1
        model: size
        visibleItemCount: 5
        implicitWidth: 92
        implicitHeight: 240
        wrap: true
        delegate: Text {
            required property int index
            text: root.pad(index * wheel.step)
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: index === wheel.currentIndex ? MobileTheme.foreground : MobileTheme.secondary
            opacity: 1.0 - Math.min(0.8, Math.abs(Tumbler.displacement) / 2.5)
            font.family: MobileTheme.fontFamily
            font.pixelSize: index === wheel.currentIndex ? 48 : 32
            font.weight: Font.Light
        }
        Rectangle {
            anchors.centerIn: parent
            width: parent.width
            height: parent.height / wheel.visibleItemCount
            color: "transparent"
            border.width: 1
            border.color: MobileTheme.muted
            radius: MobileTheme.radius(12)
        }
    }

    component AddButton: Rectangle {
        id: fab
        signal clicked()
        width: 64; height: 64
        radius: MobileTheme.radius(20)
        color: fabTap.pressed ? Qt.darker(MobileTheme.accent, 1.2) : MobileTheme.accent
        Text {
            anchors.centerIn: parent
            text: "\u{F0415}"
            color: MobileTheme.background
            font.family: MobileTheme.fontFamily
            font.pixelSize: 30
        }
        TapHandler { id: fabTap; onTapped: fab.clicked() }
    }
}
