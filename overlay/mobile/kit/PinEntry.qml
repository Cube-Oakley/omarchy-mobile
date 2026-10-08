import QtQuick
import QtQuick.Layouts

// A PIN keypad with its dots (or its digits, with reveal), or a password
// field. submitted(value) on the last digit of a PIN of known length, on ✓,
// or on Enter.
ColumnLayout {
    id: entry
    property string kind: "pin"
    // A PIN's length when known (submits on its last digit); 0 asks for ✓.
    property int length: 0
    property bool busy: false
    // The digits themselves instead of dots: codes that are not secrets,
    // like a Bluetooth passkey shown on the other device.
    property bool reveal: false
    property string text: ""
    signal submitted(string value)
    function clear() { text = ""; field.text = ""; }
    function shake() { shakeAnimation.restart(); }
    function focusField() { if (kind === "password") field.forceActiveFocus(); }
    spacing: 18

    Text {
        visible: entry.kind === "pin" && entry.reveal
        Layout.alignment: Qt.AlignHCenter
        text: entry.text.length > 0 ? entry.text : "·".repeat(Math.max(entry.length, 4))
        color: entry.text.length > 0 ? MobileTheme.foreground : MobileTheme.muted
        font.family: MobileTheme.fontFamily
        font.pixelSize: 34
        font.letterSpacing: 6
    }
    Row {
        visible: entry.kind === "pin" && !entry.reveal
        Layout.alignment: Qt.AlignHCenter
        spacing: 14
        transform: Translate { id: offset }
        Repeater {
            model: Math.max(entry.length, entry.text.length, 4)
            Rectangle {
                required property int index
                width: 16
                height: 16
                radius: 8
                color: index < entry.text.length ? MobileTheme.foreground : "transparent"
                border.width: 2
                border.color: MobileTheme.foreground
            }
        }
    }
    SequentialAnimation {
        id: shakeAnimation
        NumberAnimation { target: offset; property: "x"; to: -14; duration: 50 }
        NumberAnimation { target: offset; property: "x"; to: 14; duration: 80 }
        NumberAnimation { target: offset; property: "x"; to: -7; duration: 70 }
        NumberAnimation { target: offset; property: "x"; to: 0; duration: 60 }
    }

    Rectangle {
        visible: entry.kind === "password"
        Layout.fillWidth: true
        implicitHeight: 56
        radius: MobileTheme.radius(14)
        color: MobileTheme.surface
        border.width: 1
        border.color: field.activeFocus ? MobileTheme.accent : MobileTheme.muted
        TextInput {
            id: field
            anchors { fill: parent; margins: 16 }
            verticalAlignment: TextInput.AlignVCenter
            echoMode: TextInput.Password
            color: MobileTheme.foreground
            font.family: MobileTheme.fontFamily
            font.pixelSize: 18
            enabled: !entry.busy
            onTextEdited: entry.text = text
            onAccepted: if (entry.text.length > 0) entry.submitted(entry.text)
        }
    }

    Grid {
        visible: entry.kind === "pin"
        Layout.alignment: Qt.AlignHCenter
        columns: 3
        rowSpacing: 12
        columnSpacing: 22
        Repeater {
            model: ["1", "2", "3", "4", "5", "6", "7", "8", "9", "back", "0", "enter"]
            Rectangle {
                required property string modelData
                width: 76
                height: 76
                radius: 38
                color: key.pressed ? MobileTheme.selection : (modelData.length === 1 ? MobileTheme.surface : "transparent")
                opacity: entry.busy ? 0.4 : 1
                Text {
                    anchors.centerIn: parent
                    text: parent.modelData === "back" ? "⌫" : parent.modelData === "enter" ? "✓" : parent.modelData
                    color: MobileTheme.foreground
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: parent.modelData.length === 1 ? 30 : 26
                }
                MouseArea {
                    id: key
                    anchors.fill: parent
                    enabled: !entry.busy
                    onClicked: {
                        const value = parent.modelData;
                        if (value === "back") entry.text = entry.text.slice(0, -1);
                        else if (value === "enter") { if (entry.text.length > 0) entry.submitted(entry.text); }
                        else if (entry.text.length < 16) {
                            entry.text += value;
                            if (entry.length > 0 && entry.text.length === entry.length) entry.submitted(entry.text);
                        }
                    }
                }
            }
        }
    }
    Text {
        visible: entry.kind === "password"
        Layout.alignment: Qt.AlignHCenter
        text: "Continue"
        color: MobileTheme.accent
        font.family: MobileTheme.fontFamily
        font.pixelSize: 18
        TapHandler { onTapped: if (entry.text.length > 0) entry.submitted(entry.text) }
    }
}
