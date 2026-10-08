import QtQuick
import Quickshell

// A notification's face: its app's icon, else a tile with the app's first
// letter in the theme's accent.
Rectangle {
    id: face
    property var notification: null
    property string appName: notification ? (notification.appName || "") : ""
    readonly property string source: {
        const icon = notification ? (notification.appIcon || "") : "";
        if (!icon) return "";
        if (icon.startsWith("/")) return "file://" + icon;
        if (icon.indexOf("://") > 0) return icon;
        return Quickshell.iconPath(icon, true);
    }
    implicitWidth: 34
    implicitHeight: 34
    radius: MobileTheme.radius(width * 0.32)
    color: picture.status === Image.Ready ? "transparent" : Qt.rgba(MobileTheme.accent.r, MobileTheme.accent.g, MobileTheme.accent.b, 0.18)
    Image {
        id: picture
        anchors.fill: parent
        anchors.margins: 2
        source: face.source
        sourceSize: Qt.size(width * 3, height * 3)
        fillMode: Image.PreserveAspectFit
        // On the GUI thread: theme icons come from Quickshell's image://icon
        // provider, a QPixmap from QIcon, and on Qt Quick's loader thread,
        // alongside the GUI thread's own QIcon lookups, it crashed the shell
        // (October 5). A condition on the source does not do: the source can
        // change before the condition does, and that load starts in the
        // background. Notification icons are small.
        asynchronous: false
    }
    Text {
        anchors.centerIn: parent
        visible: picture.status !== Image.Ready
        text: (face.appName || "?").substring(0, 1).toUpperCase()
        color: MobileTheme.accent
        font.family: MobileTheme.fontFamily
        font.pixelSize: face.height * 0.5
        font.bold: true
    }
}
