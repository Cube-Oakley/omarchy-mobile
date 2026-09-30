import QtQuick

// A sheet of choices over a settings page, in the font picker's look: the
// current choice is marked, and a tap picks one and closes the sheet. Open it
// with `open = true`; options are {value, label}.
MenuOverlay {
    id: picker
    property string title: ""
    property var options: []
    property var current
    signal chosen(var value)

    scrim: Qt.rgba(MobileTheme.background.r, MobileTheme.background.g, MobileTheme.background.b, 0.88)
    onDismissed: open = false

    Column {
        anchors.centerIn: parent
        width: parent.width - 24
        spacing: 8
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: picker.title.toUpperCase()
            color: MobileTheme.accent
            font.family: MobileTheme.fontFamily
            font.pixelSize: 12
            font.letterSpacing: 2.2
            font.bold: true
        }
        ListView {
            id: list
            width: parent.width
            height: Math.min(picker.height * 0.7, Math.max(picker.options.length, 1) * 74)
            clip: true
            spacing: 10
            boundsBehavior: Flickable.StopAtBounds
            model: picker.options
            delegate: Rectangle {
                required property var modelData
                readonly property bool isCurrent: modelData.value === picker.current
                width: list.width
                height: 64
                radius: MobileTheme.radius(16)
                color: isCurrent ? MobileTheme.selection : MobileTheme.surface
                border.width: isCurrent ? 1 : 0
                border.color: MobileTheme.accent
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 18
                    text: modelData.label
                    color: MobileTheme.foreground
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 18
                }
                Text {
                    visible: parent.isCurrent
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right
                    anchors.rightMargin: 18
                    text: "Current"
                    color: MobileTheme.accent
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 13
                }
                TapHandler {
                    onTapped: {
                        picker.chosen(modelData.value);
                        picker.open = false;
                    }
                }
            }
        }
    }
}
