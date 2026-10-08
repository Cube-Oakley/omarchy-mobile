import QtQuick
import QtQuick.Layouts

Item {
    id: header
    property string kicker: ""
    property string title: ""
    property bool backVisible: false
    property bool compact: false
    // Something the app puts at the header's right (a picture, a button);
    // the title gives it room.
    property Component accessory: null
    signal backClicked()
    signal closeClicked()
    implicitHeight: compact ? 52 : 88
    Text {
        y: 0
        visible: !header.compact && header.kicker !== ""
        text: header.kicker
        color: MobileTheme.accent
        font.family: MobileTheme.fontFamily
        font.pixelSize: 11
        font.letterSpacing: 2.4
        font.bold: true
    }
    Text {
        y: header.compact ? 8 : 23
        width: parent.width - (header.backVisible ? 64 : 0) - (extra.item && extra.item.visible ? extra.item.width + 12 : 0)
        text: header.title
        color: MobileTheme.foreground
        font.family: MobileTheme.fontFamily
        font.pixelSize: header.compact ? 22 : 34
        font.bold: true
        elide: Text.ElideRight
    }
    Loader {
        id: extra
        sourceComponent: header.accessory
        anchors.right: parent.right
        anchors.rightMargin: header.backVisible ? 60 : 0
        anchors.verticalCenter: parent.verticalCenter
    }
    TouchButton {
        visible: header.backVisible
        anchors.right: parent.right
        y: header.compact ? 2 : 21
        implicitWidth: 48
        implicitHeight: 48
        radius: MobileTheme.radius(header.compact ? 12 : 24)
        label: "‹"
        textSize: 28
        onClicked: header.backClicked()
    }
}
