import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import OmarchyMobile
import "calc.js" as Calc

// Calculator: the keypad, with the scientific keys in a strip above it (a
// panel beside it in landscape), the answer shown as you type, a history,
// and a tap on the display to copy the number. The arithmetic is calc.js's
// parser. History and degrees/radians are kept in
// ~/.local/state/omarchy-mobile/calculator.json.
ShellRoot {
    id: root
    // The expression as pieces (a number, an operator, "sin("), so ⌫ takes
    // back what one key put in.
    property var pieces: []
    // The pieces are the result of "=": a digit starts over, an operator
    // goes on from it.
    property bool answered: false
    property bool degrees: true
    property var history: []
    property bool showHistory: false
    property string notice: ""
    readonly property string text: pieces.join("")
    readonly property real value: Calc.evaluate(text, degrees)
    readonly property string preview: !answered && /[-+−×÷^!%(a-zπ√]/.test(text) ? Calc.format(value) : ""
    readonly property string stateFile: (Quickshell.env("XDG_STATE_HOME") || Quickshell.env("HOME") + "/.local/state")
                                        + "/omarchy-mobile/calculator.json"

    readonly property var operators: ["+", "−", "×", "÷", "^"]
    function last() { return pieces.length ? pieces[pieces.length - 1] : ""; }
    // A number as typed or answered: −12.5, 0., 1.5E, 1.5E−3.
    function isNumber(piece) { return /^−?(\d+\.?\d*|\.\d+)(E−?\d*)?$/.test(piece); }
    // The expression ends in something a value can follow (an operator or
    // a postfix needs one).
    function endsValue() {
        const p = last();
        return p !== "" && (isNumber(p) || p === ")" || p === "π" || p === "e" || p === "!" || p === "%" || p === "^2" || p === "^−1");
    }
    function openCount() {
        let open = 0;
        for (const p of pieces) { if (p.endsWith("(")) open++; else if (p === ")") open--; }
        return open;
    }
    function push(piece) { pieces = pieces.concat([piece]); }
    function replaceLast(piece) { pieces = pieces.slice(0, -1).concat([piece]); }
    // A key that starts a new value: after "=", it starts over.
    function freshValue() { if (answered) { pieces = []; answered = false; } }

    function digit(d) {
        freshValue();
        const p = last();
        if (isNumber(p) && !p.startsWith("−")) {
            if (p === "0" && d !== ".") { replaceLast(d); return; }
            if (d === "." && (p.indexOf(".") >= 0 || p.indexOf("E") >= 0)) return;
            replaceLast(p + d);
        } else {
            push(d === "." ? "0." : d);
        }
    }
    function operator(op) {
        answered = false;
        const p = last();
        // The exponent's sign, right after EE.
        if (op === "−" && isNumber(p) && p.endsWith("E")) { replaceLast(p + "−"); return; }
        if (pieces.length === 0 || p === "(" || p.endsWith("(")) {
            if (op === "−") push("−");
            return;
        }
        if (operators.indexOf(p) >= 0) {
            // A sign after ×, ÷ or ^; else the new operator replaces the old.
            if (op === "−" && p !== "+" && p !== "−") push("−");
            else if (pieces.length > 1 || op === "−") replaceLast(op);
            return;
        }
        push(op);
    }
    function postfix(piece) {
        answered = false;
        if (endsValue()) push(piece);
    }
    function insert(piece) {
        freshValue();
        push(piece);
    }
    function press(key) {
        if (/^[\d.]$/.test(key)) digit(key);
        else if (operators.indexOf(key) >= 0) operator(key);
        else if (key === "%") postfix("%");
        else if (key === "()") parenthesis();
        else if (key === "back") backspace();
        else if (key === "clear") clearAll();
        else if (key === "=") equals();
    }
    function parenthesis() {
        freshValue();
        if (openCount() > 0 && endsValue()) push(")");
        else push("(");
    }
    function backspace() {
        answered = false;
        const p = last();
        if (isNumber(p) && p.length > 1) replaceLast(p.slice(0, -1));
        else pieces = pieces.slice(0, -1);
    }
    function clearAll() { pieces = []; answered = false; }
    function equals() {
        if (answered || pieces.length === 0) return;
        const result = Calc.format(value);
        if (result === "") { notice = /÷\s*0(?![.\d])/.test(text) ? "Can't divide by zero" : "Not a number"; return; }
        history = [{expression: text, result: result}].concat(history).slice(0, 50);
        pieces = [result];
        answered = true;
        save();
    }
    function useResult(result) {
        if (answered || pieces.length === 0 || !endsValue()) {
            if (answered) pieces = [];
            push(result);
        } else {
            pieces = [result];
        }
        answered = false;
        showHistory = false;
    }
    function copy() {
        const number = answered ? text : preview;
        if (!number) return;
        copier.command = ["wl-copy", "--", number.replace(/−/g, "-")];
        copier.running = true;
        notice = "Copied " + Calc.grouped(number);
    }
    function save() { saved.setText(JSON.stringify({history: history, degrees: degrees}) + "\n"); }

    FileView {
        id: saved
        path: root.stateFile
        blockLoading: true
        printErrors: false
        onLoaded: {
            try {
                const data = JSON.parse(text());
                root.history = Array.isArray(data.history) ? data.history : [];
                root.degrees = data.degrees !== false;
            } catch (e) {}
        }
    }
    Process { id: copier }

    AppWindow {
        id: window
        compact: true
        pageMargin: 16
        kicker: ""
        appTitle: "Calculator"
        heading: root.showHistory ? "History" : "Calculator"
        backVisible: root.showHistory
        onBackClicked: root.showHistory = false

        Item {
            id: page
            anchors.fill: parent
            // Landscape: the scientific keys beside the keypad.
            readonly property bool wide: width > height * 1.1

            ColumnLayout {
                anchors.fill: parent
                spacing: 10
                visible: !root.showHistory

                // The display: the expression (or the answer), and what it
                // comes to so far. A tap copies the number.
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: !page.wide
                    Layout.preferredHeight: page.wide ? 96 : -1
                    Layout.minimumHeight: page.wide ? 96 : 120
                    Column {
                        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                        spacing: 6
                        Text {
                            width: parent.width
                            horizontalAlignment: Text.AlignRight
                            text: root.answered && root.history.length ? Calc.grouped(root.history[0].expression) + " =" : ""
                            color: MobileTheme.secondary
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 18
                            elide: Text.ElideLeft
                        }
                        Text {
                            width: parent.width
                            height: page.wide ? 50 : 76
                            horizontalAlignment: Text.AlignRight
                            verticalAlignment: Text.AlignBottom
                            text: root.pieces.length ? Calc.grouped(root.text) : "0"
                            color: MobileTheme.foreground
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: page.wide ? 44 : 60
                            fontSizeMode: Text.HorizontalFit
                            minimumPixelSize: 22
                            elide: Text.ElideLeft
                        }
                        Text {
                            width: parent.width
                            height: page.wide ? 22 : 30
                            horizontalAlignment: Text.AlignRight
                            text: root.preview ? "= " + Calc.grouped(root.preview) : ""
                            color: MobileTheme.secondary
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: page.wide ? 18 : 24
                            elide: Text.ElideLeft
                        }
                    }
                    TapHandler { onTapped: root.copy() }
                }

                // History, degrees or radians, and the scientific strip in
                // portrait (landscape has them in the panel).
                RowLayout {
                    visible: !page.wide
                    Layout.fillWidth: true
                    spacing: 8
                    Key {
                        Layout.preferredWidth: 56
                        label: "\u{F02DA}"    // history
                        kind: "plain"
                        onClicked: root.showHistory = true
                    }
                    Key {
                        Layout.preferredWidth: 64
                        label: root.degrees ? "DEG" : "RAD"
                        kind: "plain"
                        textSize: 15
                        onClicked: { root.degrees = !root.degrees; root.save(); }
                    }
                    Flickable {
                        visible: !page.wide
                        Layout.fillWidth: true
                        Layout.preferredHeight: 48
                        contentWidth: strip.width
                        clip: true
                        flickableDirection: Flickable.HorizontalFlick
                        boundsBehavior: Flickable.StopAtBounds
                        Row {
                            id: strip
                            spacing: 8
                            Repeater {
                                model: page.wide ? [] : scientific.keys
                                Key {
                                    required property var modelData
                                    width: 60
                                    height: 48
                                    label: modelData.label
                                    kind: "plain"
                                    textSize: 17
                                    onClicked: scientific.press(modelData)
                                }
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: page.wide
                    Layout.preferredHeight: page.wide ? -1 : page.height * 0.52
                    spacing: 10
                    // Landscape: the scientific keys as a panel, with history
                    // and degrees or radians.
                    GridLayout {
                        visible: page.wide
                        Layout.fillWidth: false
                        Layout.fillHeight: true
                        Layout.preferredWidth: page.width * 0.45
                        Layout.maximumWidth: page.width * 0.45
                        columns: 4
                        rowSpacing: 6
                        columnSpacing: 6
                        Repeater {
                            model: page.wide ? [{label: "\u{F02DA}", history: true},
                                                {label: root.degrees ? "DEG" : "RAD", degrees: true}].concat(scientific.keys) : []
                            Key {
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                label: modelData.label
                                kind: "plain"
                                textSize: 16
                                onClicked: scientific.press(modelData)
                            }
                        }
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        columns: 4
                        rowSpacing: page.wide ? 6 : 10
                        columnSpacing: page.wide ? 6 : 10
                        Repeater {
                            model: [
                                {label: "C", kind: "plain", key: "clear"}, {label: "( )", kind: "plain", key: "()"},
                                {label: "%", kind: "plain", key: "%"}, {label: "÷", kind: "operator", key: "÷"},
                                {label: "7", key: "7"}, {label: "8", key: "8"}, {label: "9", key: "9"},
                                {label: "×", kind: "operator", key: "×"},
                                {label: "4", key: "4"}, {label: "5", key: "5"}, {label: "6", key: "6"},
                                {label: "−", kind: "operator", key: "−"},
                                {label: "1", key: "1"}, {label: "2", key: "2"}, {label: "3", key: "3"},
                                {label: "+", kind: "operator", key: "+"},
                                {label: "0", key: "0"}, {label: ".", key: "."},
                                {label: "\u{F006E}", kind: "plain", key: "back"},   // backspace
                                {label: "=", kind: "equals", key: "="}
                            ]
                            Key {
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                label: modelData.label
                                kind: modelData.kind || "digit"
                                onClicked: root.press(modelData.key)
                            }
                        }
                    }
                }
            }

            // History: the latest first; a tap uses the result.
            ColumnLayout {
                anchors.fill: parent
                visible: root.showHistory
                spacing: 10
                Text {
                    visible: root.history.length === 0
                    Layout.fillWidth: true
                    text: "Nothing yet. Each = adds a line here."
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 16
                    wrapMode: Text.WordWrap
                }
                ListView {
                    id: historyList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 4
                    model: root.history
                    delegate: Rectangle {
                        required property var modelData
                        width: historyList.width
                        height: 72
                        radius: MobileTheme.radius(12)
                        color: entryTap.pressed ? MobileTheme.selection : "transparent"
                        Column {
                            anchors { right: parent.right; rightMargin: 12; left: parent.left; leftMargin: 12
                                      verticalCenter: parent.verticalCenter }
                            spacing: 4
                            Text {
                                width: parent.width
                                horizontalAlignment: Text.AlignRight
                                text: Calc.grouped(modelData.expression)
                                color: MobileTheme.secondary
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 15
                                elide: Text.ElideLeft
                            }
                            Text {
                                width: parent.width
                                horizontalAlignment: Text.AlignRight
                                text: "= " + Calc.grouped(modelData.result)
                                color: MobileTheme.foreground
                                font.family: MobileTheme.fontFamily
                                font.pixelSize: 22
                                elide: Text.ElideLeft
                            }
                        }
                        TapHandler { id: entryTap; onTapped: root.useResult(modelData.result) }
                    }
                }
                TouchButton {
                    visible: root.history.length > 0
                    Layout.fillWidth: true
                    label: "Clear history"
                    onClicked: { root.history = []; root.save(); }
                }
            }

            // A passing notice: copied, or why there is no answer.
            Rectangle {
                anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: 8 }
                visible: root.notice !== ""
                width: noticeText.implicitWidth + 32
                height: 40
                radius: MobileTheme.radius(20)
                color: MobileTheme.surface
                Text {
                    id: noticeText
                    anchors.centerIn: parent
                    text: root.notice
                    color: MobileTheme.foreground
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 14
                }
                onVisibleChanged: if (visible) noticeTimer.restart()
                Timer { id: noticeTimer; interval: 2000; onTriggered: root.notice = "" }
            }
        }
    }

    // The scientific keys, in the strip (portrait) or the panel (landscape).
    QtObject {
        id: scientific
        readonly property var keys: [
            {label: "sin", piece: "sin("}, {label: "cos", piece: "cos("}, {label: "tan", piece: "tan("},
            {label: "ln", piece: "ln("}, {label: "log", piece: "log("}, {label: "√", piece: "√("},
            {label: "x²", post: "^2"}, {label: "xʸ", op: "^"}, {label: "1/x", post: "^−1"},
            {label: "π", piece: "π"}, {label: "e", piece: "e"}, {label: "x!", post: "!"},
            {label: "(", paren: "("}, {label: ")", paren: ")"}, {label: "EE", exponent: true}
        ]
        function press(key) {
            if (key.history) root.showHistory = true;
            else if (key.degrees) { root.degrees = !root.degrees; root.save(); }
            else if (key.piece) root.insert(key.piece);
            else if (key.post) root.postfix(key.post);
            else if (key.op) root.operator(key.op);
            else if (key.paren === "(") root.insert("(");
            else if (key.paren === ")") { if (root.openCount() > 0 && root.endsValue()) root.push(")"); }
            else if (key.exponent) {
                // Scientific notation for the number being typed: 1.5 EE 20.
                const p = root.last();
                if (root.isNumber(p) && p.indexOf("E") < 0 && !root.answered) root.replaceLast(p + "E");
            }
        }
    }

    // A key: digits on the surface, operators in the accent colour, = filled.
    component Key: Rectangle {
        id: key
        property string label: ""
        property string kind: "digit"
        property int textSize: (kind === "digit" || kind === "equals" ? 30 : 26) - (page.wide ? 8 : 0)
        signal clicked()
        implicitHeight: 56
        radius: MobileTheme.radius(16)
        color: kind === "equals" ? (tap.pressed ? Qt.darker(MobileTheme.accent, 1.2) : MobileTheme.accent)
             : tap.pressed ? MobileTheme.selection
             : kind === "digit" ? MobileTheme.surface : Qt.rgba(MobileTheme.surface.r, MobileTheme.surface.g, MobileTheme.surface.b, 0.6)
        Text {
            anchors.centerIn: parent
            text: key.label
            color: key.kind === "equals" ? MobileTheme.background
                 : key.kind === "operator" ? MobileTheme.accent : MobileTheme.foreground
            font.family: MobileTheme.fontFamily
            font.pixelSize: key.textSize
        }
        TapHandler { id: tap; onTapped: key.clicked() }
    }
}
