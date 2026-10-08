import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Io

Flickable {
    id: page
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    contentHeight: body.height
    readonly property string helper: Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-bluetooth"
    property var radio: ({available: false, devices: []})
    // "power", "scan", or the address of the device an action is working on.
    property string busy: ""
    property string message: ""
    // Paired device whose Connect/Forget actions are open.
    property string selected: ""
    // A pairing's question for the user ({kind, code, name}: confirm,
    // authorize, passkey or pin), or digits to type on the device ({code}).
    property var pairRequest: null
    property var pairDisplay: null
    property string pairName: ""
    readonly property bool powered: radio.available === true && radio.powered === true
    readonly property var paired: (radio.devices || []).filter(d => d.paired)
    readonly property var nearby: (radio.devices || []).filter(d => !d.paired && d.named)
    function refresh() { if (!probe.running) probe.running = true; }
    // Replacing the model rebuilds every row, so only apply real changes.
    function apply(result) {
        if (JSON.stringify(result) !== JSON.stringify(radio)) radio = result;
    }
    function run(args, busyKey) {
        if (action.running || pairing.running) return;
        busy = busyKey;
        message = "";
        action.command = [helper].concat(args);
        action.running = true;
    }
    function pair(device) {
        if (action.running || pairing.running) return;
        busy = device.address;
        message = "";
        pairName = device.name;
        pairing.command = [helper, "pair", device.address];
        pairing.running = true;
    }
    // The helper's lines: events while it pairs, then the status.
    function pairingLine(line) {
        let data;
        try { data = JSON.parse(line); } catch (e) { return; }
        if (data.event === "request") {
            pairRequest = data;
            pairDisplay = null;
            codeEntry.clear();
            page.contentY = 0;
        } else if (data.event === "display") {
            pairDisplay = data;
            pairRequest = null;
            page.contentY = 0;
        } else if (data.event === "cancelled") {
            pairRequest = null;
            pairDisplay = null;
        } else if (data.event === undefined) {
            message = data.message || "";
            delete data.ok;
            delete data.message;
            apply(data);
        }
    }
    function respond(answer) {
        pairRequest = null;
        if (pairing.running) pairing.write(answer + "\n");
    }
    function cancelPairing() {
        pairRequest = null;
        pairDisplay = null;
        if (pairing.running) pairing.write("cancel\n");
    }
    function spaced(code) { return code.length === 6 ? code.slice(0, 3) + " " + code.slice(3) : code; }
    onVisibleChanged: if (!visible) cancelPairing()
    function describe(device) {
        if (busy === device.address) return pairing.running ? "Pairing…" : "Working…";
        if (device.connected)
            return device.battery !== null && device.battery !== undefined ? "Connected · " + device.battery + "%" : "Connected";
        return device.paired ? "Not connected" : "Pair";
    }
    Component.onCompleted: refresh()
    Process {
        id: probe
        command: [page.helper]
        stdout: StdioCollector {}
        onExited: {
            try { page.apply(JSON.parse(stdout.text)); }
            catch (e) { page.apply({available: false, reason: "Could not read Bluetooth", devices: []}); }
        }
    }
    Process {
        id: action
        stdout: StdioCollector {}
        onExited: {
            try {
                const result = JSON.parse(stdout.text);
                page.message = result.message || "";
                delete result.ok;
                delete result.message;
                page.apply(result);
            } catch (e) {
                page.message = "Bluetooth did not answer.";
            }
            page.busy = "";
        }
    }
    Process {
        id: pairing
        stdinEnabled: true
        stdout: SplitParser { onRead: line => page.pairingLine(line) }
        onExited: {
            page.pairRequest = null;
            page.pairDisplay = null;
            if (page.busy !== "" && page.message === "" && !page.paired.some(d => d.address === page.busy))
                page.message = "Pairing stopped.";
            page.busy = "";
            page.refresh();
        }
    }
    // Connections change underneath the page, and scan results arrive over time.
    Timer {
        interval: page.busy === "scan" ? 1500 : 4000
        repeat: true
        running: page.visible
        onTriggered: page.refresh()
    }
    Column {
        id: body
        width: page.width
        spacing: 12
        Text {
            width: parent.width
            text: page.busy === "power" ? (page.powered ? "Turning off…" : "Turning on…")
                : page.powered ? "On" : page.radio.available || page.radio.startable ? "Off" : "Unavailable"
            color: MobileTheme.accent
            font.family: MobileTheme.fontFamily
            font.pixelSize: 48
            font.bold: true
        }
        SettingsRow {
            width: parent.width
            label: "Bluetooth"
            value: page.powered ? "On" : "Off"
            selected: page.powered
            enabled: (page.radio.available === true || page.radio.startable === true) && !action.running && !pairing.running
            onClicked: page.run(["power", page.powered ? "off" : "on"], "power")
        }
        DetailRow { width: parent.width; visible: page.radio.available === true; label: "Visible as"; value: page.radio.name || "—" }
        // The pairing's question: a code to compare, a yes, a code to type
        // here, or digits to type on the device.
        Rectangle {
            visible: page.pairRequest !== null || page.pairDisplay !== null
            width: parent.width
            height: visible ? pairColumn.implicitHeight + 32 : 0
            radius: MobileTheme.radius(16)
            color: MobileTheme.surface
            border.width: 1
            border.color: MobileTheme.accent
            Column {
                id: pairColumn
                x: 16
                y: 16
                width: parent.width - 32
                spacing: 14
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: {
                        const name = page.pairName || "the device";
                        const request = page.pairRequest;
                        if (!request) return "Type this code on " + name + ", then press Enter";
                        if (request.kind === "confirm") return "Pair with " + name + "?";
                        if (request.kind === "authorize") return name + " wants to pair";
                        return "Enter the code shown on " + name;
                    }
                    color: MobileTheme.foreground
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 18
                    font.bold: true
                }
                Text {
                    visible: (page.pairRequest !== null && page.pairRequest.kind === "confirm") || page.pairDisplay !== null
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: page.spaced(page.pairRequest ? page.pairRequest.code || "" : page.pairDisplay ? page.pairDisplay.code : "")
                    color: MobileTheme.accent
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 44
                    font.bold: true
                    font.letterSpacing: 4
                }
                Text {
                    visible: page.pairRequest !== null && page.pairRequest.kind === "confirm"
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: "Make sure " + (page.pairName || "the device") + " shows the same code."
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 14
                }
                PinEntry {
                    id: codeEntry
                    visible: page.pairRequest !== null && (page.pairRequest.kind === "passkey" || page.pairRequest.kind === "pin")
                    width: parent.width
                    reveal: true
                    length: page.pairRequest !== null && page.pairRequest.kind === "passkey" ? 6 : 0
                    onSubmitted: value => page.respond(value)
                }
                Row {
                    width: parent.width
                    spacing: 10
                    TouchButton {
                        width: page.pairRequest !== null && (page.pairRequest.kind === "confirm" || page.pairRequest.kind === "authorize")
                            ? (parent.width - 10) / 2 : parent.width
                        label: "Cancel"
                        onClicked: page.cancelPairing()
                    }
                    TouchButton {
                        visible: page.pairRequest !== null && (page.pairRequest.kind === "confirm" || page.pairRequest.kind === "authorize")
                        width: (parent.width - 10) / 2
                        label: "Pair"
                        selected: true
                        onClicked: page.respond("yes")
                    }
                }
            }
        }
        Text {
            visible: page.powered && page.paired.length > 0
            width: parent.width
            text: "Paired devices"
            color: MobileTheme.foreground
            font.family: MobileTheme.fontFamily
            font.pixelSize: 16
            font.bold: true
            topPadding: 8
        }
        Repeater {
            model: page.powered ? page.paired : []
            delegate: Column {
                required property var modelData
                width: body.width
                spacing: 8
                SettingsRow {
                    width: parent.width
                    label: modelData.name
                    value: page.describe(modelData)
                    selected: modelData.connected
                    enabled: !action.running && !pairing.running
                    onClicked: page.selected = page.selected === modelData.address ? "" : modelData.address
                }
                Row {
                    visible: page.selected === modelData.address
                    width: parent.width
                    spacing: 10
                    TouchButton {
                        width: (parent.width - 10) / 2
                        label: modelData.connected ? "Disconnect" : "Connect"
                        enabled: !action.running
                        onClicked: page.run([modelData.connected ? "disconnect" : "connect", modelData.address], modelData.address)
                    }
                    TouchButton {
                        width: (parent.width - 10) / 2
                        label: "Forget"
                        enabled: !action.running
                        onClicked: {
                            page.selected = "";
                            page.run(["forget", modelData.address], modelData.address);
                        }
                    }
                }
            }
        }
        Text {
            visible: page.powered && (page.nearby.length > 0 || page.busy === "scan")
            width: parent.width
            text: "Available devices"
            color: MobileTheme.foreground
            font.family: MobileTheme.fontFamily
            font.pixelSize: 16
            font.bold: true
            topPadding: 8
        }
        Repeater {
            model: page.powered ? page.nearby : []
            delegate: SettingsRow {
                required property var modelData
                width: body.width
                label: modelData.name
                value: page.describe(modelData)
                enabled: !action.running && !pairing.running
                onClicked: page.pair(modelData)
            }
        }
        TouchButton {
            width: parent.width
            label: page.busy === "scan" ? "Scanning…" : "Scan for devices"
            enabled: page.powered && !action.running && !pairing.running
            onClicked: page.run(["scan", "15"], "scan")
        }
        Text {
            width: parent.width
            visible: page.message.length > 0
            wrapMode: Text.WordWrap
            text: page.message
            color: MobileTheme.foreground
            font.family: MobileTheme.fontFamily
            font.pixelSize: 14
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            topPadding: 8
            text: page.radio.available || page.radio.startable
                ? "Put a device in pairing mode, scan, then tap it. Watches and phones show a code to compare; keyboards ask you to type one."
                : page.radio.reason || "Bluetooth is not available."
            color: MobileTheme.secondary
            font.family: MobileTheme.fontFamily
            font.pixelSize: 14
        }
    }
}
