import QtQuick
import QtQuick.Layouts
import Quickshell

// Shared window for an Omarchy Mobile app. The app supplies the page under
// the header. Theme, type, and closing the process come from here.
FloatingWindow {
    id: win
    property string kicker: "OMARCHY"
    property string heading: "App"
    // Stable window title. The heading can change (a folder name) without
    // making the back gesture lose track of which app is on screen.
    property string appTitle: ""
    property bool backVisible: false
    property bool compact: false
    property int pageMargin: 24
    // Shown at the header's right (PageHeader.accessory).
    property Component headerAccessory: null
    signal backClicked()
    default property alias content: slot.data
    title: appTitle.length ? appTitle : heading
    implicitWidth: 480
    implicitHeight: 840
    color: MobileTheme.background
    onClosed: Qt.quit()
    onBackingWindowVisibleChanged: if (!backingWindowVisible) Qt.quit()
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: win.pageMargin
        spacing: win.compact ? 8 : 12
        PageHeader {
            Layout.fillWidth: true
            kicker: win.kicker
            title: win.heading
            backVisible: win.backVisible
            compact: win.compact
            accessory: win.headerAccessory
            onBackClicked: win.backClicked()
        }
        AppBack {
            Layout.preferredWidth: 0
            Layout.preferredHeight: 0
            windowTitle: win.appTitle.length ? win.appTitle : win.heading
            onBack: win.backClicked()
        }
        Item {
            id: slot
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
    }
}
