import QtQuick
import QtQuick.Layouts
import Qt.labs.folderlistmodel
import Quickshell
import Quickshell.Io
import OmarchyMobile

// Contacts: the list, a contact, the editor, photos and import/export.
// Contacts are vCard files kept by omarchy-mobile-contacts, so any CardDAV or
// vCard tool can read them. Every colour comes from MobileTheme.
ShellRoot {
    id: root
    property bool ready: false
    property bool allowed: false
    property var requests: []
    property string view: "list"        // list, detail, edit, photo, import
    property var people: []
    property string query: ""
    property var person: null           // the full contact on screen
    property var draft: null            // the contact being edited
    // Typing changes the draft in place, so the field being typed in is never
    // rebuilt; adding or removing a row bumps this to rebuild the rows.
    property int draftRows: 0
    property string message: ""
    property string notice: ""
    property var importable: []
    property bool confirmDelete: false
    property bool menuOpen: false
    property string photoFolder: ""
    property var activeField: null
    property string cropFile: ""        // the square crop, removed once embedded
    property var queue: []
    readonly property string home: Quickshell.env("HOME")
    readonly property var shown: {
        const q = query.trim().toLowerCase();
        const digits = q.replace(/[^0-9]/g, "");
        const list = !q ? people : people.filter(p => (p.name || "").toLowerCase().indexOf(q) >= 0
            || (p.org || "").toLowerCase().indexOf(q) >= 0 || (p.email || "").toLowerCase().indexOf(q) >= 0
            || (digits.length >= 2 && (p.phones || []).some(n => (n.value || "").replace(/[^0-9]/g, "").indexOf(digits) >= 0)));
        // Favourites first, then the alphabet; section letters come from sort.
        return list.filter(p => p.favorite).map(p => Object.assign({ section: "\u{F04CE}  Favorites" }, p))
            .concat(list.map(p => Object.assign({ section: letter(p) }, p)));
    }
    readonly property var phoneTypes: ["mobile", "home", "work", "main", "other"]
    readonly property var emailTypes: ["home", "work", "other"]

    function letter(p) {
        const first = (p.sort || p.name || "#").trim().charAt(0).toUpperCase();
        return /[A-Z]/.test(first) ? first : "#";
    }
    function bin(name) { return home + "/.local/bin/" + name; }
    function helper(args) { return [bin("omarchy-mobile-contacts")].concat(args); }
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
    function refresh() { run("list", helper(["list"])); }
    function open(uid) {
        view = "detail";
        confirmDelete = false;
        run("get", helper(["get", uid]));
    }
    function blank() {
        return { uid: "", name: "", given: "", family: "", middle: "", prefix: "", suffix: "", nickname: "",
                 org: "", title: "", phones: [{ type: "mobile", value: "" }], emails: [], addresses: [],
                 urls: [], birthday: "", note: "", favorite: false, photo: "" };
    }
    function edit(contact) {
        const copy = JSON.parse(JSON.stringify(contact || blank()));
        if (!copy.phones || !copy.phones.length) copy.phones = [{ type: "mobile", value: "" }];
        copy.emails = copy.emails || [];
        copy.addresses = copy.addresses || [];
        copy.urls = copy.urls || [];
        draft = copy;
        draftRows++;
        view = "edit";
        message = "";
    }
    function setDraft(key, value) { if (draft) draft[key] = value; }
    function setEntry(list, index, key, value) { if (draft && draft[list][index]) draft[list][index][key] = value; }
    function addEntry(list, entry) { draft[list].push(entry); draftRows++; }
    function removeEntry(list, index) { draft[list].splice(index, 1); draftRows++; }
    function cycle(list, index, types) {
        const current = draft[list][index].type || types[0];
        const next = types[(types.indexOf(current) + 1) % types.length];
        setEntry(list, index, "type", next);
        return next;
    }
    function rows(list) { return draft ? (draftRows, draft[list].length) : 0; }
    function save() {
        activeField = null;
        const copy = JSON.parse(JSON.stringify(draft));
        copy.phones = copy.phones.filter(p => (p.value || "").trim());
        copy.emails = copy.emails.filter(e => (e.value || "").trim());
        copy.addresses = copy.addresses.filter(a => ["street", "city", "region", "postcode", "country"].some(k => (a[k] || "").trim()));
        copy.urls = copy.urls.filter(u => (u || "").trim());
        // A name from its parts is rebuilt from them; a name alone is kept.
        if (copy.given || copy.family) copy.name = "";
        run("save", helper(["save"]), JSON.stringify(copy));
        keyboard("hide");
        activeField = null;
    }
    function goBack() {
        if (menuOpen) { menuOpen = false; return; }
        if (activeField) { activeField = null; keyboard("hide"); return; }
        if (view === "photo") { view = "detail"; return; }
        if (view === "edit") { view = draft && draft.uid ? "detail" : "list"; draft = null; keyboard("hide"); return; }
        if (view === "detail" || view === "import") { view = "list"; person = null; refresh(); return; }
        Qt.quit();
    }
    function keyboard(action) {
        keyboardTool.command = [bin("omarchy-mobile-keyboard"), action];
        keyboardTool.running = true;
    }
    function editField(field) {
        activeField = field;
        field.forceActiveFocus();
        keyboard("show");
    }
    function launch(app, arg) { Quickshell.execDetached([bin("omarchy-mobile-app"), "launch", app, arg]); }
    function today() {
        const d = new Date();
        return d.getFullYear() + ("0" + (d.getMonth() + 1)).slice(-2) + ("0" + d.getDate()).slice(-2);
    }
    function safeName(name) { return (name || "contact").replace(/[^A-Za-z0-9 ._-]/g, "").trim().replace(/\s+/g, "-") || "contact"; }
    function birthdayText(value) {
        if (!value) return "";
        const m = /^(\d{4}|--)-?(\d{2})-(\d{2})$/.exec(value);
        if (!m) return value;
        const date = new Date(2000, Number(m[2]) - 1, Number(m[3]));
        const day = date.toLocaleDateString(Qt.locale(), "d MMMM");
        return m[1] === "--" ? day : day + " " + m[1];
    }
    function addressText(a) {
        return [a.street, [a.city, a.region].filter(s => s).join(", "), a.postcode, a.country].filter(s => s).join("\n");
    }

    Component.onCompleted: run("grants", [bin("omarchy-mobile-app"), "status", "contacts"])
    function begin() {
        refresh();
        // Other apps open Contacts on a person ("uid:...") or to save a number ("new:...").
        const arg = Quickshell.env("OMARCHY_MOBILE_APP_ARG") || "";
        if (arg.startsWith("uid:")) open(arg.slice(4));
        else if (arg.startsWith("new:")) {
            const fresh = blank();
            fresh.phones = [{ type: "mobile", value: arg.slice(4) }];
            edit(fresh);
        }
    }

    Process {
        id: tool
        property string kind: ""
        property string input: ""
        stdout: StdioCollector {}
        stderr: StdioCollector {}
        stdinEnabled: true
        onStarted: {
            if (tool.input) write(tool.input);
            stdinEnabled = false;
        }
        onExited: {
            tool.stdinEnabled = true;
            let result = {};
            try { result = JSON.parse(stdout.text); }
            catch (e) {
                const err = (stderr.text || "").trim().split("\n").pop();
                result = { ok: false, error: err || "The contacts helper did not answer" };
            }
            const kind = tool.kind;
            if (kind === "photo" && root.cropFile) {
                // The card holds the photo now; the crop was only the hand-over.
                Quickshell.execDetached(["rm", "-f", "--", root.cropFile]);
                root.cropFile = "";
            }
            if (kind === "grants") {
                const items = result.permissions || [];
                root.requests = items;
                root.allowed = items.length > 0 && items.every(p => p.granted);
                root.ready = result.ok !== false;
                if (result.ok === false) root.message = result.error || "Contacts is not installed";
                if (root.allowed) root.begin();
            } else if (kind === "grant") {
                if (result.ok !== false && (result.permissions || []).every(p => p.granted)) {
                    root.allowed = true;
                    root.begin();
                }
            } else if (result.ok === false) {
                root.message = result.error || "That didn't work";
            } else if (kind === "list") {
                root.people = result.contacts || [];
            } else if (kind === "get") {
                root.person = result.contact;
            } else if (kind === "save") {
                root.person = result.contact;
                root.draft = null;
                root.view = "detail";
                root.refresh();
            } else if (kind === "favorite" || kind === "photo") {
                root.person = result.contact || root.person;
                if (kind === "photo") root.view = "detail";
                if (root.person) root.run("get", root.helper(["get", root.person.uid]));
                root.refresh();
            } else if (kind === "delete") {
                root.person = null;
                root.view = "list";
                root.refresh();
            } else if (kind === "importable") {
                root.importable = result.files || [];
            } else if (kind === "import") {
                root.notice = "Added " + (result.added || 0) + (result.skipped ? ", skipped " + result.skipped + " already here" : "")
                    + (result.failed ? ", " + result.failed + " could not be read" : "");
                root.view = "list";
                root.refresh();
            } else if (kind === "export") {
                root.notice = "Saved " + (result.count || 0) + " to " + String(result.path || "").replace(root.home, "~");
            }
            Qt.callLater(root.next);
        }
    }
    Process { id: keyboardTool }
    Timer { id: noticeHold; interval: 5000; running: root.notice.length > 0; onTriggered: root.notice = "" }

    component Glyph: Text {
        color: MobileTheme.foreground
        font.family: MobileTheme.fontFamily
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    component Action: Rectangle {
        id: act
        property string glyph: ""
        property string label: ""
        property color ink: MobileTheme.accent
        property bool enabled: true
        signal clicked()
        Layout.fillWidth: true
        implicitHeight: 72
        radius: MobileTheme.radius(16)
        opacity: enabled ? 1 : 0.4
        color: actTap.pressed ? MobileTheme.selection : MobileTheme.surface
        Column {
            anchors.centerIn: parent
            spacing: 4
            Glyph { anchors.horizontalCenter: parent.horizontalCenter; text: act.glyph; color: act.ink; font.pixelSize: 24 }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: act.label
                color: MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 12
            }
        }
        TapHandler { id: actTap; enabled: act.enabled; onTapped: act.clicked() }
    }
    component SectionLabel: Text {
        Layout.fillWidth: true
        Layout.topMargin: 10
        color: MobileTheme.accent
        font.family: MobileTheme.fontFamily
        font.pixelSize: 11
        font.letterSpacing: 2.2
        font.bold: true
    }
    component Field: TouchTextField {
        id: field
        Layout.fillWidth: true
        editing: root.activeField === field
        onEditingRequested: root.editField(field)
    }
    component Pill: Rectangle {
        id: pill
        property string label: ""
        signal clicked()
        implicitWidth: pillText.implicitWidth + 20
        implicitHeight: 48
        radius: MobileTheme.radius(12)
        color: pillTap.pressed ? MobileTheme.muted : MobileTheme.selection
        Text {
            id: pillText
            anchors.centerIn: parent
            text: pill.label
            color: MobileTheme.accent
            font.family: MobileTheme.fontFamily
            font.pixelSize: 13
        }
        TapHandler { id: pillTap; onTapped: pill.clicked() }
    }

    AppWindow {
        compact: true
        pageMargin: 16
        kicker: ""
        appTitle: "Contacts"
        heading: root.view === "edit" ? (root.draft && root.draft.uid ? "Edit contact" : "New contact")
               : root.view === "photo" ? "Choose a photo"
               : root.view === "import" ? "Import contacts"
               : root.view === "detail" ? "" : "Contacts"
        backVisible: root.allowed && root.view !== "list"
        onBackClicked: root.goBack()

        GrantPage {
            anchors.fill: parent
            visible: root.ready && !root.allowed
            requests: root.requests
            onAllowed: {
                for (let i = 0; i < root.requests.length; i++)
                    root.run("grant", [root.bin("omarchy-mobile-app"), "grant", "contacts", root.requests[i].id]);
            }
            onDenied: Qt.quit()
        }

        // The list.
        ColumnLayout {
            anchors.fill: parent
            visible: root.allowed && root.view === "list"
            spacing: 10
            TouchTextField {
                id: search
                Layout.fillWidth: true
                placeholderText: "\u{F0349}  Search " + root.people.length + " contacts"
                onTextChanged: root.query = text
                onEditingRequested: { editing = true; forceActiveFocus(); root.keyboard("show"); }
                onEditingChanged: if (!editing) root.keyboard("hide")
            }
            Text {
                Layout.fillWidth: true
                visible: root.notice.length > 0
                text: root.notice
                color: MobileTheme.success
                font.family: MobileTheme.fontFamily
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }
            ListView {
                id: list
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: root.shown
                section.property: "section"
                section.criteria: ViewSection.FullString
                section.delegate: Text {
                    required property string section
                    width: list.width
                    topPadding: 12
                    bottomPadding: 4
                    leftPadding: 6
                    text: section
                    color: MobileTheme.accent
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 12
                    font.letterSpacing: 2
                    font.bold: true
                }
                delegate: Rectangle {
                    id: row
                    required property var modelData
                    width: list.width
                    implicitHeight: 64
                    radius: MobileTheme.radius(14)
                    color: rowTap.pressed ? MobileTheme.selection : "transparent"
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6
                        spacing: 12
                        Avatar { size: 46; photo: row.modelData.photo || ""; initials: row.modelData.initials || "" }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                Layout.fillWidth: true
                                text: row.modelData.name
                                textFormat: Text.PlainText
                                color: MobileTheme.foreground
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 16
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: text.length > 0
                                text: row.modelData.org || ((row.modelData.phones || [])[0] || {}).value || row.modelData.email || ""
                                textFormat: Text.PlainText
                                color: MobileTheme.secondary
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }
                        }
                    }
                    TapHandler { id: rowTap; onTapped: root.open(row.modelData.uid) }
                }
                Column {
                    anchors.centerIn: parent
                    visible: root.people.length === 0 && root.ready
                    width: list.width - 32
                    spacing: 14
                    Glyph { anchors.horizontalCenter: parent.horizontalCenter; text: "\u{F000E}"; font.pixelSize: 56; color: MobileTheme.muted }
                    Text {
                        width: parent.width
                        text: "No contacts yet. Add one, or import a .vcf or .csv file from Downloads or Documents."
                        color: MobileTheme.secondary
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 15
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                visible: root.message.length > 0
                text: root.message
                color: MobileTheme.danger
                font.family: MobileTheme.fontFamily
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                TouchButton {
                    Layout.fillWidth: true
                    label: "\u{F0014}  New contact"
                    selected: true
                    onClicked: root.edit(null)
                }
                TouchButton {
                    implicitWidth: 64
                    label: "\u{F01D9}"
                    textSize: 22
                    onClicked: root.menuOpen = true
                }
            }
        }

        // One contact.
        Flickable {
            anchors.fill: parent
            visible: root.allowed && root.view === "detail" && root.person !== null
            contentHeight: detail.implicitHeight + 24
            clip: true
            ColumnLayout {
                id: detail
                width: parent.width
                spacing: 10
                Item {
                    Layout.alignment: Qt.AlignHCenter
                    implicitWidth: 132
                    implicitHeight: 132
                    Avatar {
                        anchors.fill: parent
                        size: 132
                        photo: root.person ? (root.person.photo || "") : ""
                        initials: root.person ? (root.person.initials || "") : ""
                    }
                    Rectangle {
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        width: 40; height: 40
                        radius: MobileTheme.radius(20)
                        color: MobileTheme.accent
                        Glyph { anchors.centerIn: parent; text: "\u{F0100}"; color: MobileTheme.background; font.pixelSize: 20 }
                    }
                    TapHandler { onTapped: root.view = "photo" }
                }
                Text {
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    text: root.person ? root.person.name : ""
                    textFormat: Text.PlainText
                    color: MobileTheme.foreground
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 28
                    font.bold: true
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                }
                Text {
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: root.person ? [root.person.title, root.person.org].filter(s => s).join(" · ") : ""
                    textFormat: Text.PlainText
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 14
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    spacing: 8
                    readonly property string first: root.person && (root.person.phones || []).length ? root.person.phones[0].value : ""
                    Action { glyph: "\u{F03F2}"; label: "Call"; ink: MobileTheme.success; enabled: parent.first.length > 0; onClicked: root.launch("phone", "tel:" + parent.first) }
                    Action { glyph: "\u{F0369}"; label: "Text"; enabled: parent.first.length > 0; onClicked: root.launch("messages", "sms:" + parent.first) }
                    Action {
                        glyph: root.person && root.person.favorite ? "\u{F04CE}" : "\u{F04D2}"
                        label: root.person && root.person.favorite ? "Favorite" : "Add to favorites"
                        onClicked: root.run("favorite", root.helper(["favorite", root.person.uid, root.person.favorite ? "off" : "on"]))
                    }
                    Action { glyph: "\u{F03EB}"; label: "Edit"; onClicked: root.edit(root.person) }
                }
                Repeater {
                    model: root.person ? (root.person.phones || []) : []
                    delegate: Rectangle {
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: 64
                        radius: MobileTheme.radius(14)
                        color: MobileTheme.surface
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 14
                            anchors.rightMargin: 6
                            spacing: 6
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text { Layout.fillWidth: true; text: modelData.value; textFormat: Text.PlainText; color: MobileTheme.foreground; font.family: MobileTheme.fontFamily; font.pixelSize: 16; elide: Text.ElideRight }
                                Text { text: modelData.type || "phone"; color: MobileTheme.secondary; font.family: MobileTheme.fontFamily; font.pixelSize: 12 }
                            }
                            Rectangle {
                                implicitWidth: 48; implicitHeight: 48; radius: MobileTheme.radius(24)
                                color: textTap.pressed ? MobileTheme.selection : "transparent"
                                Glyph { anchors.centerIn: parent; text: "\u{F0369}"; color: MobileTheme.accent; font.pixelSize: 22 }
                                TapHandler { id: textTap; onTapped: root.launch("messages", "sms:" + modelData.value) }
                            }
                            Rectangle {
                                implicitWidth: 48; implicitHeight: 48; radius: MobileTheme.radius(24)
                                color: callTap.pressed ? MobileTheme.selection : "transparent"
                                Glyph { anchors.centerIn: parent; text: "\u{F03F2}"; color: MobileTheme.success; font.pixelSize: 22 }
                                TapHandler { id: callTap; onTapped: root.launch("phone", "tel:" + modelData.value) }
                            }
                        }
                    }
                }
                Repeater {
                    model: root.person ? (root.person.emails || []) : []
                    delegate: DetailBlock { glyph: "\u{F01EE}"; value: modelData.value; label: modelData.type || "email"; required property var modelData }
                }
                Repeater {
                    model: root.person ? (root.person.addresses || []) : []
                    delegate: DetailBlock { glyph: "\u{F034E}"; value: root.addressText(modelData); label: modelData.type || "address"; required property var modelData }
                }
                DetailBlock {
                    visible: !!(root.person && root.person.birthday)
                    glyph: "\u{F00EB}"
                    value: root.person ? root.birthdayText(root.person.birthday) : ""
                    label: "birthday"
                }
                Repeater {
                    model: root.person ? (root.person.urls || []) : []
                    delegate: DetailBlock { glyph: "\u{F059F}"; value: modelData; label: "website"; required property var modelData }
                }
                DetailBlock {
                    visible: !!(root.person && root.person.nickname)
                    glyph: "\u{F0004}"
                    value: root.person ? root.person.nickname || "" : ""
                    label: "nickname"
                }
                DetailBlock {
                    visible: !!(root.person && root.person.note)
                    glyph: "\u{F039E}"
                    value: root.person ? root.person.note || "" : ""
                    label: "notes"
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 12
                    spacing: 10
                    TouchButton {
                        Layout.fillWidth: true
                        label: "\u{F0497}  Save as .vcf"
                        onClicked: root.run("export", root.helper(["export", root.home + "/Documents/" + root.safeName(root.person.name) + ".vcf", root.person.uid]))
                    }
                    TouchButton {
                        Layout.fillWidth: true
                        label: root.confirmDelete ? "Tap again to delete" : "\u{F01B4}  Delete"
                        onClicked: {
                            if (!root.confirmDelete) { root.confirmDelete = true; return; }
                            root.run("delete", root.helper(["delete", root.person.uid]));
                        }
                    }
                }
                Text {
                    Layout.fillWidth: true
                    visible: root.notice.length > 0 || root.message.length > 0
                    text: root.message || root.notice
                    color: root.message ? MobileTheme.danger : MobileTheme.success
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 13
                    wrapMode: Text.WordWrap
                }
            }
        }

        // The editor.
        Flickable {
            id: editor
            anchors.fill: parent
            anchors.bottomMargin: saveRow.height + 10
            visible: root.allowed && root.view === "edit" && root.draft !== null
            contentHeight: form.implicitHeight + 24
            clip: true
            ColumnLayout {
                id: form
                width: parent.width
                spacing: 8
                SectionLabel { text: "NAME" }
                Field { placeholderText: "First name"; text: root.draft ? root.draft.given : ""; onTextChanged: root.setDraft("given", text) }
                Field { placeholderText: "Last name"; text: root.draft ? root.draft.family : ""; onTextChanged: root.setDraft("family", text) }
                Field { placeholderText: "Company"; text: root.draft ? root.draft.org : ""; onTextChanged: root.setDraft("org", text) }
                Field { placeholderText: "Job title"; text: root.draft ? root.draft.title : ""; onTextChanged: root.setDraft("title", text) }

                SectionLabel { text: "PHONE" }
                Repeater {
                    model: root.rows("phones")
                    delegate: RowLayout {
                        id: phoneRow
                        required property int index
                        readonly property var entry: root.draft.phones[index]
                        property string type: entry.type || "mobile"
                        Layout.fillWidth: true
                        spacing: 6
                        Pill { label: phoneRow.type; onClicked: phoneRow.type = root.cycle("phones", phoneRow.index, root.phoneTypes) }
                        Field {
                            placeholderText: "Phone number"
                            text: phoneRow.entry.value || ""
                            inputMethodHints: Qt.ImhDialableCharactersOnly
                            onTextChanged: root.setEntry("phones", phoneRow.index, "value", text)
                        }
                        Pill { label: "\u{F0156}"; onClicked: root.removeEntry("phones", phoneRow.index) }
                    }
                }
                Pill { label: "\u{F0659}  Add phone"; onClicked: root.addEntry("phones", { type: "mobile", value: "" }) }

                SectionLabel { text: "EMAIL" }
                Repeater {
                    model: root.rows("emails")
                    delegate: RowLayout {
                        id: emailRow
                        required property int index
                        readonly property var entry: root.draft.emails[index]
                        property string type: entry.type || "home"
                        Layout.fillWidth: true
                        spacing: 6
                        Pill { label: emailRow.type; onClicked: emailRow.type = root.cycle("emails", emailRow.index, root.emailTypes) }
                        Field {
                            placeholderText: "Email"
                            text: emailRow.entry.value || ""
                            inputMethodHints: Qt.ImhEmailCharactersOnly
                            onTextChanged: root.setEntry("emails", emailRow.index, "value", text)
                        }
                        Pill { label: "\u{F0156}"; onClicked: root.removeEntry("emails", emailRow.index) }
                    }
                }
                Pill { label: "\u{F01EE}  Add email"; onClicked: root.addEntry("emails", { type: "home", value: "" }) }

                SectionLabel { text: "ADDRESS" }
                Repeater {
                    model: root.rows("addresses")
                    delegate: ColumnLayout {
                        id: addressRow
                        required property int index
                        readonly property var entry: root.draft.addresses[index]
                        property string type: entry.type || "home"
                        Layout.fillWidth: true
                        spacing: 6
                        RowLayout {
                            Layout.fillWidth: true
                            Pill { label: addressRow.type; onClicked: addressRow.type = root.cycle("addresses", addressRow.index, root.emailTypes) }
                            Item { Layout.fillWidth: true }
                            Pill { label: "\u{F0156}  Remove"; onClicked: root.removeEntry("addresses", addressRow.index) }
                        }
                        Field { placeholderText: "Street"; text: addressRow.entry.street || ""; onTextChanged: root.setEntry("addresses", addressRow.index, "street", text) }
                        RowLayout {
                            Layout.fillWidth: true
                            Field { placeholderText: "City"; text: addressRow.entry.city || ""; onTextChanged: root.setEntry("addresses", addressRow.index, "city", text) }
                            Field { placeholderText: "State"; text: addressRow.entry.region || ""; onTextChanged: root.setEntry("addresses", addressRow.index, "region", text) }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Field { placeholderText: "Postal code"; text: addressRow.entry.postcode || ""; onTextChanged: root.setEntry("addresses", addressRow.index, "postcode", text) }
                            Field { placeholderText: "Country"; text: addressRow.entry.country || ""; onTextChanged: root.setEntry("addresses", addressRow.index, "country", text) }
                        }
                    }
                }
                Pill { label: "\u{F034E}  Add address"; onClicked: root.addEntry("addresses", { type: "home", street: "", city: "", region: "", postcode: "", country: "" }) }

                SectionLabel { text: "MORE" }
                Field {
                    placeholderText: "Birthday (YYYY-MM-DD)"
                    text: root.draft ? root.draft.birthday : ""
                    inputMethodHints: Qt.ImhDate
                    onTextChanged: root.setDraft("birthday", text)
                }
                Field { placeholderText: "Nickname"; text: root.draft ? root.draft.nickname : ""; onTextChanged: root.setDraft("nickname", text) }
                Field {
                    placeholderText: "Website"
                    text: root.draft && root.draft.urls.length ? root.draft.urls[0] : ""
                    inputMethodHints: Qt.ImhUrlCharactersOnly
                    onTextChanged: if (root.draft) root.draft.urls[0] = text
                }
                Field { placeholderText: "Notes"; text: root.draft ? root.draft.note : ""; onTextChanged: root.setDraft("note", text) }
                Text {
                    Layout.fillWidth: true
                    visible: root.message.length > 0
                    text: root.message
                    color: MobileTheme.danger
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 13
                    wrapMode: Text.WordWrap
                }
            }
        }
        RowLayout {
            id: saveRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            visible: root.allowed && root.view === "edit"
            spacing: 10
            TouchButton { Layout.fillWidth: true; label: "Cancel"; onClicked: root.goBack() }
            TouchButton { Layout.fillWidth: true; label: "\u{F012C}  Save"; selected: true; onClicked: root.save() }
        }

        // Choosing a photo: a picture from Pictures, cropped square.
        ColumnLayout {
            anchors.fill: parent
            visible: root.allowed && root.view === "photo"
            spacing: 10
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Repeater {
                    model: [{ label: "Camera", path: root.home + "/Pictures/Camera" }, { label: "Pictures", path: root.home + "/Pictures" },
                            { label: "Downloads", path: root.home + "/Downloads" }]
                    delegate: TouchButton {
                        required property var modelData
                        Layout.fillWidth: true
                        label: modelData.label
                        selected: root.photoFolder === modelData.path
                        onClicked: { root.photoFolder = modelData.path; cropper.source = ""; }
                    }
                }
            }
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: width
                visible: cropper.source.toString().length > 0
                // The square that becomes the photo: drag to move, pinch to zoom.
                Item {
                    id: crop
                    anchors.fill: parent
                    clip: true
                    Rectangle { anchors.fill: parent; color: MobileTheme.surface }
                    Flickable {
                        id: pan
                        anchors.fill: parent
                        contentWidth: cropper.width
                        contentHeight: cropper.height
                        boundsBehavior: Flickable.StopAtBounds
                        Image {
                            id: cropper
                            readonly property real cover: implicitWidth > 0 ? Math.max(crop.width / implicitWidth, crop.height / implicitHeight) : 1
                            property real zoom: 1
                            width: implicitWidth * cover * zoom
                            height: implicitHeight * cover * zoom
                            asynchronous: true
                            autoTransform: true
                            sourceSize: Qt.size(2048, 2048)
                            onStatusChanged: if (status === Image.Ready) { zoom = 1; pan.contentX = (width - crop.width) / 2; pan.contentY = (height - crop.height) / 2; }
                        }
                        PinchHandler {
                            target: null
                            property real startZoom: 1
                            onActiveChanged: if (active) startZoom = cropper.zoom
                            onActiveScaleChanged: if (active) cropper.zoom = Math.max(1, Math.min(4, startZoom * activeScale))
                        }
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    border.width: 2
                    border.color: MobileTheme.accent
                    radius: MobileTheme.radius(width / 2)
                }
            }
            GridView {
                id: pictures
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: cropper.source.toString().length === 0
                clip: true
                cellWidth: width / 3
                cellHeight: cellWidth
                model: FolderListModel {
                    folder: root.photoFolder ? "file://" + root.photoFolder : ""
                    nameFilters: ["*.jpg", "*.jpeg", "*.png", "*.JPG", "*.JPEG", "*.PNG"]
                    showDirs: false
                    sortField: FolderListModel.Time
                }
                delegate: Item {
                    required property url fileUrl
                    width: pictures.cellWidth
                    height: pictures.cellHeight
                    Image {
                        anchors.fill: parent
                        anchors.margins: 3
                        source: fileUrl
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        autoTransform: true
                        sourceSize: Qt.size(240, 240)
                    }
                    TapHandler { onTapped: cropper.source = fileUrl }
                }
                Text {
                    anchors.centerIn: parent
                    visible: pictures.count === 0
                    text: "No pictures here"
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 15
                }
            }
            Item { Layout.fillHeight: true; visible: cropper.source.toString().length > 0 }
            Text {
                Layout.fillWidth: true
                visible: root.message.length > 0
                text: root.message
                color: MobileTheme.danger
                font.family: MobileTheme.fontFamily
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                TouchButton {
                    Layout.fillWidth: true
                    visible: cropper.source.toString().length === 0 && root.person && root.person.photo
                    label: "\u{F01B4}  Remove photo"
                    onClicked: root.run("photo", root.helper(["clear-photo", root.person.uid]))
                }
                TouchButton {
                    Layout.fillWidth: true
                    visible: cropper.source.toString().length > 0
                    label: "Choose another"
                    onClicked: cropper.source = ""
                }
                TouchButton {
                    Layout.fillWidth: true
                    visible: cropper.source.toString().length > 0
                    label: "\u{F012C}  Use photo"
                    selected: true
                    onClicked: {
                        const target = root.home + "/.cache/omarchy-mobile/contacts/crop-" + Date.now() + ".jpg";
                        crop.grabToImage(result => {
                            if (!result.saveToFile(target)) { root.message = "Could not save the photo"; return; }
                            root.cropFile = target;
                            root.run("photo", root.helper(["set-photo", root.person.uid, target]));
                            cropper.source = "";
                        }, Qt.size(512, 512));
                    }
                }
            }
        }

        // Import from a file.
        ColumnLayout {
            anchors.fill: parent
            visible: root.allowed && root.view === "import"
            spacing: 10
            Text {
                Layout.fillWidth: true
                text: "vCard (.vcf) and CSV files (Google, Outlook) in Downloads, Documents and your home folder."
                color: MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }
            ListView {
                id: files
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 6
                model: root.importable
                delegate: Rectangle {
                    required property var modelData
                    width: files.width
                    implicitHeight: 64
                    radius: MobileTheme.radius(14)
                    color: fileTap.pressed ? MobileTheme.selection : MobileTheme.surface
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 12
                        Glyph { text: "\u{F02FA}"; color: MobileTheme.accent; font.pixelSize: 24 }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text { Layout.fillWidth: true; text: modelData.name; textFormat: Text.PlainText; color: MobileTheme.foreground; font.family: MobileTheme.fontFamily; font.pixelSize: 15; elide: Text.ElideMiddle }
                            Text {
                                Layout.fillWidth: true
                                text: String(modelData.path).replace(root.home, "~").replace(/\/[^\/]*$/, "") + (modelData.modified ? " · " + modelData.modified : "")
                                textFormat: Text.PlainText
                                color: MobileTheme.secondary
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }
                        }
                    }
                    TapHandler { id: fileTap; onTapped: root.run("import", root.helper(["import", modelData.path])) }
                }
                Text {
                    anchors.centerIn: parent
                    width: files.width - 32
                    visible: root.importable.length === 0
                    text: "No .vcf or .csv files found. Save one to Downloads (for example an export from Google Contacts) and come back."
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 15
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                }
            }
            Text {
                Layout.fillWidth: true
                visible: root.message.length > 0
                text: root.message
                color: MobileTheme.danger
                font.family: MobileTheme.fontFamily
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }
        }

        MenuOverlay {
            open: root.menuOpen
            onDismissed: root.menuOpen = false
            ColumnLayout {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 8
                spacing: 8
                TouchButton {
                    Layout.fillWidth: true
                    label: "\u{F02FA}  Import from a file"
                    onClicked: { root.menuOpen = false; root.message = ""; root.view = "import"; root.run("importable", root.helper(["importable"])); }
                }
                TouchButton {
                    Layout.fillWidth: true
                    label: "\u{F0207}  Export all to Documents"
                    enabled: root.people.length > 0
                    onClicked: { root.menuOpen = false; root.run("export", root.helper(["export", root.home + "/Documents/contacts-" + root.today() + ".vcf"])); }
                }
                TouchButton { Layout.fillWidth: true; label: "Cancel"; onClicked: root.menuOpen = false }
            }
        }
    }

    component DetailBlock: Rectangle {
        id: block
        property string glyph: ""
        property string value: ""
        property string label: ""
        Layout.fillWidth: true
        implicitHeight: blockColumn.implicitHeight + 24
        radius: MobileTheme.radius(14)
        color: MobileTheme.surface
        RowLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 14
            Glyph { Layout.alignment: Qt.AlignTop; text: block.glyph; color: MobileTheme.accent; font.pixelSize: 20 }
            ColumnLayout {
                id: blockColumn
                Layout.fillWidth: true
                spacing: 2
                TextEdit {
                    Layout.fillWidth: true
                    text: block.value
                    textFormat: TextEdit.PlainText
                    readOnly: true
                    wrapMode: TextEdit.Wrap
                    color: MobileTheme.foreground
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 15
                }
                Text { text: block.label; color: MobileTheme.secondary; font.family: MobileTheme.fontFamily; font.pixelSize: 12 }
            }
        }
    }
}
