import QtQuick
import QtQuick.Effects

// A person: their photo, else their initials, else a person glyph. Follows
// the theme's corners: round themes get a round avatar, square ones square.
Item {
    id: avatar
    property string photo: ""
    property string initials: ""
    property int size: 48
    implicitWidth: size
    implicitHeight: size
    readonly property bool showPhoto: photo.length > 0 && picture.status === Image.Ready
    Rectangle {
        id: mask
        anchors.fill: parent
        radius: MobileTheme.radius(width / 2)
        visible: false
        layer.enabled: true
    }
    Rectangle {
        anchors.fill: parent
        visible: !avatar.showPhoto
        radius: MobileTheme.radius(width / 2)
        color: MobileTheme.selection
        Text {
            anchors.centerIn: parent
            text: avatar.initials || "\u{F0004}"
            color: avatar.initials ? MobileTheme.accent : MobileTheme.secondary
            font.family: MobileTheme.fontFamily
            font.pixelSize: avatar.size * (avatar.initials ? 0.38 : 0.52)
            font.bold: avatar.initials.length > 0
        }
    }
    Image {
        id: picture
        anchors.fill: parent
        visible: avatar.showPhoto
        source: avatar.photo ? "file://" + encodeURI(avatar.photo).replace(/#/g, "%23").replace(/\?/g, "%3F") : ""
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        cache: false
        sourceSize: Qt.size(avatar.size * 3, avatar.size * 3)
        layer.enabled: !MobileTheme.square
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: mask
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1.0
        }
    }
}
