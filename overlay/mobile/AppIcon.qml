import QtQuick
import Quickshell

// An app's icon at full size: the icon theme's own picture (Yaru's are
// shaped and coloured, 512 px), drawn from the large source so it stays sharp
// at the phone's 3x scale. An app without one gets its initial on a tile in
// the theme's accent.
Item {
    id: appIcon
    property string icon: ""
    property string name: ""
    property bool pressed: false
    readonly property bool found: picture.status === Image.Ready
    readonly property string source: {
        if (!icon) return "";
        if (icon.startsWith("/")) return "file://" + icon;
        if (icon.indexOf("://") > 0) return icon;
        return Quickshell.iconPath(icon, true);
    }
    implicitWidth: 64
    implicitHeight: 64
    scale: pressed ? 0.9 : 1
    Behavior on scale { NumberAnimation { duration: 90; easing.type: Easing.OutQuad } }

    Image {
        id: picture
        anchors.fill: parent
        source: appIcon.source
        sourceSize: Qt.size(Math.ceil(appIcon.width * 3), Math.ceil(appIcon.height * 3))
        fillMode: Image.PreserveAspectFit
        smooth: true
        mipmap: true
        // In the background: the drawer's 512 px icons would stall the GUI
        // thread. (Notification icons load on the GUI thread; see
        // NotificationIcon.qml.)
        asynchronous: true
    }
    Rectangle {
        visible: !appIcon.found
        anchors.fill: parent
        anchors.margins: parent.width * 0.04
        radius: MobileTheme.radius(width * 0.26)
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.lighter(MobileTheme.accent, 1.15) }
            GradientStop { position: 1; color: Qt.darker(MobileTheme.accent, 1.45) }
        }
        Text {
            anchors.centerIn: parent
            text: (appIcon.name || "?").substring(0, 1).toUpperCase()
            color: MobileTheme.background
            font.family: MobileTheme.fontFamily
            font.pixelSize: parent.height * 0.5
            font.bold: true
        }
    }
}
