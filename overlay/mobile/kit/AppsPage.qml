import QtQuick
import Quickshell
import Quickshell.Io

Flickable {
    id: page
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    contentHeight: body.height
    property var apps: []
    // Sites added to the home screen from the browser, and the one whose
    // Remove is showing.
    property var webapps: []
    property string selectedWebApp: ""
    // Android apps (Waydroid): each opens as its own window, and is in the
    // app drawer once chosen here.
    property var android: []
    readonly property string helper: Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-app"
    function refresh() {
        if (!probe.running) probe.running = true;
        if (!webProbe.running) webProbe.running = true;
        if (!androidProbe.running) androidProbe.running = true;
    }
    Component.onCompleted: refresh()
    Process {
        id: probe
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-app", "apps"]
        running: true
        stdout: StdioCollector {}
        onExited: {
            try { page.apps = JSON.parse(stdout.text).apps || []; }
            catch (e) { page.apps = []; }
        }
    }
    Process {
        id: webProbe
        command: [page.helper, "webapps"]
        running: true
        stdout: StdioCollector {}
        onExited: {
            try { page.webapps = JSON.parse(stdout.text).webapps || []; }
            catch (e) { page.webapps = []; }
        }
    }
    Process {
        id: androidProbe
        command: [page.helper, "android"]
        running: true
        stdout: StdioCollector {}
        onExited: {
            try { page.android = JSON.parse(stdout.text).android || []; }
            catch (e) { page.android = []; }
        }
    }
    Process {
        id: androidToggle
        stdout: StdioCollector {}
        onExited: {
            try { page.android = JSON.parse(stdout.text).android || page.android; }
            catch (e) {}
        }
    }
    Process {
        id: webRemove
        stdout: StdioCollector {}
        onExited: { page.selectedWebApp = ""; page.refresh(); }
    }
    Process {
        id: revoke
        property var args: []
        stdout: StdioCollector {}
        onExited: page.refresh()
    }
    Column {
        id: body
        width: page.width
        spacing: 12
        Text {
            visible: page.apps.length === 0 && page.webapps.length === 0 && page.android.length === 0
            width: parent.width
            wrapMode: Text.WordWrap
            text: "No mobile apps are installed yet."
            color: MobileTheme.secondary
            font.family: MobileTheme.fontFamily
            font.pixelSize: 16
        }
        Repeater {
            model: page.apps
            delegate: Column {
                id: appBlock
                required property var modelData
                width: body.width
                spacing: 8
                Text {
                    width: parent.width
                    text: appBlock.modelData.name
                    color: MobileTheme.foreground
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 20
                    font.bold: true
                }
                Repeater {
                    model: appBlock.modelData.permissions || []
                    delegate: SettingsRow {
                        required property var modelData
                        width: appBlock.width
                        label: modelData.title
                        value: modelData.granted ? "Allowed" : "Not allowed"
                        selected: modelData.granted
                        onClicked: {
                            if (!modelData.granted || revoke.running) return;
                            revoke.command = [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-app", "revoke", appBlock.modelData.id, modelData.id];
                            revoke.running = true;
                        }
                    }
                }
            }
        }
        Text {
            visible: page.webapps.length > 0
            width: parent.width
            topPadding: 8
            text: "Web apps"
            color: MobileTheme.foreground
            font.family: MobileTheme.fontFamily
            font.pixelSize: 20
            font.bold: true
        }
        Repeater {
            model: page.webapps
            delegate: Column {
                id: webBlock
                required property var modelData
                width: body.width
                spacing: 8
                SettingsRow {
                    width: parent.width
                    label: webBlock.modelData.name
                    value: webBlock.modelData.url.replace(/^[a-z]+:\/\/(www\.)?/i, "").replace(/\/$/, "")
                    onClicked: page.selectedWebApp = page.selectedWebApp === webBlock.modelData.id ? "" : webBlock.modelData.id
                }
                TouchButton {
                    visible: page.selectedWebApp === webBlock.modelData.id
                    width: parent.width
                    label: "Remove from home screen"
                    onClicked: {
                        if (webRemove.running) return;
                        webRemove.command = [page.helper, "webapp-remove", webBlock.modelData.id];
                        webRemove.running = true;
                    }
                }
            }
        }
        Text {
            visible: page.android.length > 0
            width: parent.width
            topPadding: 8
            text: "Android apps"
            color: MobileTheme.foreground
            font.family: MobileTheme.fontFamily
            font.pixelSize: 20
            font.bold: true
        }
        Repeater {
            model: page.android
            delegate: SettingsRow {
                required property var modelData
                width: body.width
                label: modelData.name
                value: modelData.shown ? "In app drawer" : (modelData.builtin ? "Built into Android" : "Not in app drawer")
                selected: modelData.shown
                onClicked: {
                    if (androidToggle.running) return;
                    androidToggle.command = [page.helper, modelData.shown ? "android-hide" : "android-show", modelData.package];
                    androidToggle.running = true;
                }
            }
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            topPadding: 8
            text: "Tap an allowed permission to revoke it. The app asks again the next time it opens. This is not a process sandbox."
                + (page.webapps.length > 0 ? " Removing a web app also deletes its cookies and saved data." : "")
                + (page.android.length > 0 ? " Tap an Android app to add it to the app drawer or take it out; it opens as its own window." : "")
            color: MobileTheme.secondary
            font.family: MobileTheme.fontFamily
            font.pixelSize: 14
        }
    }
}
