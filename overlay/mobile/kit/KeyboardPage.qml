import QtQuick
import Quickshell
import Quickshell.Io

// Settings > Keyboard: voice typing (omarchy-mobile-dictate), its models
// downloaded, chosen and removed here. Speech becomes text on the phone.
Flickable {
    id: page
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    contentHeight: body.height
    property var status: ({})
    property string working: ""         // the model being downloaded or removed
    property string message: ""
    readonly property string helper: Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-dictate"
    readonly property var choices: [
        { name: "tiny", label: "Fast" },
        { name: "small", label: "Accurate" }
    ]
    function run(args, model) {
        if (tool.running) return;
        working = model || "";
        message = args[0] === "install" ? "Downloading… this can take a minute" : "";
        tool.command = [helper].concat(args);
        tool.running = true;
    }
    function installed(name) { return !!(status.models && status.models[name] && status.models[name].installed); }
    function size(name) { return status.models && status.models[name] ? status.models[name].megabytes + " MB" : ""; }
    Component.onCompleted: run(["status"])
    onVisibleChanged: if (visible) run(["status"])
    Process {
        id: tool
        stdout: StdioCollector {}
        onExited: {
            try {
                const result = JSON.parse(stdout.text);
                if (result.ok) { page.status = result; page.message = ""; }
                else page.message = result.error || "Voice typing did not answer";
            } catch (e) { page.message = "Voice typing did not answer"; }
            page.working = "";
        }
    }
    Column {
        id: body
        width: page.width
        spacing: 12
        Text {
            width: parent.width
            text: "Voice typing"
            color: MobileTheme.foreground
            font.family: MobileTheme.fontFamily
            font.pixelSize: 16
            font.bold: true
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: "Tap the microphone above the keys and speak; each phrase is typed when you pause. "
                + "Speech becomes text on the phone: nothing is recorded or sent anywhere."
            color: MobileTheme.secondary
            font.family: MobileTheme.fontFamily
            font.pixelSize: 14
        }
        Repeater {
            model: page.choices
            delegate: SettingsRow {
                required property var modelData
                width: body.width
                label: modelData.label + " · " + page.size(modelData.name)
                value: page.working === modelData.name ? "…"
                     : !page.installed(modelData.name) ? "Download"
                     : page.status.model === modelData.name ? "In use" : "Use"
                selected: page.installed(modelData.name) && page.status.model === modelData.name
                enabled: !tool.running
                onClicked: {
                    if (!page.installed(modelData.name)) page.run(["install", modelData.name], modelData.name);
                    else if (page.status.model !== modelData.name) page.run(["use", modelData.name]);
                }
            }
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: "Fast is lighter and gets more words wrong; Accurate is the better listener and works "
                + "the processor harder while you speak. Both are Moonshine's English models."
            color: MobileTheme.secondary
            font.family: MobileTheme.fontFamily
            font.pixelSize: 13
        }
        Repeater {
            model: page.choices.filter(c => page.installed(c.name) && page.status.model !== c.name)
            delegate: TouchButton {
                required property var modelData
                width: body.width
                implicitHeight: 48
                label: "Remove " + modelData.label + " (" + page.size(modelData.name) + ")"
                enabled: !tool.running
                onClicked: page.run(["remove", modelData.name], modelData.name)
            }
        }
        Text {
            visible: page.message !== ""
            width: parent.width
            wrapMode: Text.WordWrap
            text: page.message
            color: MobileTheme.secondary
            font.family: MobileTheme.fontFamily
            font.pixelSize: 14
        }
    }
}
