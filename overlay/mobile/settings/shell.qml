import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import OmarchyMobile

ShellRoot {
    id: root
    property string panel: Quickshell.env("OMARCHY_MOBILE_SETTINGS_PANEL") || "home"
    property bool dnd: false
    property bool speedOpen: false
    property bool dnsAuto: true
    property bool dnsEditing: false
    // Wi-Fi IPv4: Automatic (DHCP) or Static. ipEditing names the field typed in.
    property bool ipAuto: true
    property string ipEditing: ""
    function fillCurrentIp() {
        const current = (root.wifi.ipv4 || [])[0] || "";
        ipAddressField.text = current;
        ipGatewayField.text = root.wifi.gateway || "";
        ipDnsField.text = (root.wifi.dns || []).filter(d => d.indexOf(":") < 0).join(" ");
    }
    property var wifi: ({available: false})
    property var networks: []
    property string networkMessage: ""
    function goBack() {
        if (speedOpen) { speedOpen = false; return; }
        if (panel === "appearance" && appearance.pickerShown) {
            appearance.closePicker();
            return;
        }
        if (panel === "home") return;
        dnsEditing = false;
        ipEditing = "";
        panel = panel === "wifi" ? "network" : "home";
    }
    property string backStamp: ""
    Process {
        id: backRead
        command: ["cat", Quickshell.env("XDG_RUNTIME_DIR") + "/omarchy-mobile/back"]
        stdout: StdioCollector {}
        stderr: StdioCollector {}
        onExited: {
            const stamp = stdout.text.trim();
            if (!stamp || stamp === root.backStamp) return;
            const first = root.backStamp === "";
            root.backStamp = stamp;
            if (!first) root.goBack();
        }
    }
    Timer {
        interval: 180
        repeat: true
        running: root.panel !== "home" || root.speedOpen
        onTriggered: if (!backRead.running) backRead.running = true
    }
    function openPanel(name) {
        if (["home", "appearance", "clipboard", "network", "wifi", "sound", "battery", "about", "bluetooth", "display", "storage", "apps"].indexOf(name) >= 0) {
            speedOpen = false;
            dnsEditing = false;
            panel = name;
        }
        if (name === "wifi" || name === "network") net.run("status");
    }
    function setDnd(enabled) {
        dnd = enabled;
        prefsSave.payload = JSON.stringify({dnd: enabled}) + "\n";
        prefsSave.running = true;
    }
    Process {
        id: prefsLoad
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-prefs"]
        running: true
        stdout: StdioCollector {}
        onExited: {
            try { root.dnd = JSON.parse(stdout.text).dnd === true; }
            catch (e) { root.dnd = false; }
        }
    }
    Process {
        id: prefsSave
        stdinEnabled: true
        property string payload: ""
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-prefs", "set"]
        stdout: StdioCollector {}
        onStarted: { write(prefsSave.payload); prefsSave.payload = ""; stdinEnabled = false; }
        onExited: {
            stdinEnabled = true;
            try { root.dnd = JSON.parse(stdout.text).dnd === true; }
            catch (e) {}
        }
    }
    IpcHandler {
        target: "settings"
        function open(name: string): void { win.visible = true; root.openPanel(name); }
        function panel(): string { return root.panel; }
        function back(): void { root.goBack(); }
    }
    Process {
        id: clipRequest
        stdinEnabled: true
        property string payload: ""
        property var items: []
        property string message: ""
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-clipboard"]
        stdout: StdioCollector {}
        function run(action, values) {
            if (running) return;
            payload = values ? JSON.stringify(values) + "\n" : "";
            stdinEnabled = !!values;
            command = [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-clipboard", action];
            running = true;
        }
        onStarted: {
            if (payload) write(payload);
            payload = "";
            stdinEnabled = false;
        }
        onExited: {
            stdinEnabled = true;
            try {
                const result = JSON.parse(stdout.text);
                clipRequest.items = result.items || [];
                clipRequest.message = result.error || "";
            } catch (e) { clipRequest.message = "Clipboard unavailable"; }
        }
    }
    Connections {
        target: root
        function onPanelChanged() {
            if (root.panel === "clipboard") clipRequest.run("history");
            if (root.panel === "wifi" || root.panel === "network") net.run("status");
        }
    }
    Process {
        id: net
        stdinEnabled: true
        property string payload: ""
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-wifi"]
        stdout: StdioCollector {}
        function run(action, values) {
            if (running) return;
            payload = values ? JSON.stringify(values) + "\n" : "";
            stdinEnabled = !!values;
            command = action && action !== "status"
                ? [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-wifi", action]
                : [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-wifi"];
            running = true;
        }
        onStarted: {
            if (payload) write(payload);
            payload = "";
            stdinEnabled = false;
        }
        onExited: {
            stdinEnabled = true;
            try {
                const result = JSON.parse(stdout.text);
                root.networkMessage = result.error || "";
                if (result.networks) root.networks = result.networks;
                if (result.available !== undefined && result.networks === undefined) {
                    root.wifi = result;
                    if (!root.dnsEditing) root.dnsAuto = result.dns_auto !== false;
                    if (!root.ipEditing) {
                        root.ipAuto = result.ipv4_method !== "manual";
                        const fixed = result.ipv4_static || {};
                        if (!root.ipAuto && ipAddressField) {
                            ipAddressField.text = fixed.address || "";
                            ipGatewayField.text = fixed.gateway || "";
                            ipDnsField.text = (fixed.dns || []).join(" ");
                        }
                    }
                    if (dnsField && !root.dnsEditing)
                        dnsField.text = (result.dns_static || []).join(" ");
                    if (root.panel === "wifi" && result.interface)
                        Qt.callLater(() => { if (!net.running) net.run("scan", {interface: result.interface}); });
                } else if (result.ok && !result.networks && root.panel === "wifi") {
                    Qt.callLater(() => net.run("status"));
                }
            } catch (e) { root.networkMessage = "Network request failed"; }
        }
    }
    AppBack {
        windowTitle: "Settings"
        onBack: root.goBack()
    }
    FloatingWindow {
        id: win
        title: "Settings"
        implicitWidth: 480
        implicitHeight: 840
        color: MobileTheme.background
        onClosed: Qt.quit()
        onBackingWindowVisibleChanged: if (!backingWindowVisible) Qt.quit()
        function fieldWantsKeyboard() {
            return (root.dnsEditing && root.panel === "wifi" && !root.dnsAuto)
                || (root.ipEditing !== "" && root.panel === "wifi" && !root.ipAuto)
                || (root.panel === "appearance" && appearance.installerKeyboard)
                || (root.panel === "weather" && weatherPage.editing);
        }
        function suppressKeyboard() {
            if (fieldWantsKeyboard())
                return;
            Qt.inputMethod.hide();
            if (!hideKb.running)
                hideKb.running = true;
            hideAgain.restart();
        }
        Process {
            id: hideKb
            command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-keyboard", "hide"]
        }
        Timer {
            id: hideAgain
            interval: 450
            repeat: false
            onTriggered: {
                if (win.fieldWantsKeyboard())
                    return;
                Qt.inputMethod.hide();
                hideKb.running = true;
            }
        }
        Component.onCompleted: suppressKeyboard()
        Connections {
            target: Qt.inputMethod
            function onVisibleChanged() {
                if (Qt.inputMethod.visible)
                    win.suppressKeyboard();
            }
        }
        Connections {
            target: MobileTheme
            function onBusyChanged() {
                if (!MobileTheme.busy)
                    win.suppressKeyboard();
            }
            function onStateChanged() { win.suppressKeyboard(); }
        }
        Connections {
            target: root
            function onDnsEditingChanged() {
                if (win.fieldWantsKeyboard()) {
                    dnsField.forceActiveFocus();
                    Qt.inputMethod.show();
                } else {
                    win.suppressKeyboard();
                }
            }
            function onPanelChanged() {
                win.suppressKeyboard();
                if (root.panel !== "appearance")
                    appearance.closePicker();
            }
        }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 24
            spacing: 18
            PageHeader {
                Layout.fillWidth: true
                kicker: root.panel === "home" ? "OMARCHY" : "SETTINGS"
                title: root.speedOpen ? "Speed test" : ({home: "Settings", appearance: "Appearance", clipboard: "Clipboard", network: "Network", wifi: "Wi-Fi", sound: "Sound", battery: "Battery", about: "About", bluetooth: "Bluetooth", display: "Display", storage: "Storage", apps: "Apps", weather: "Weather"})[root.panel] || "Settings"
                backVisible: root.panel !== "home" || root.speedOpen
                onBackClicked: root.goBack()
            }
            Flickable {
                visible: root.panel === "home"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentHeight: home.height
                boundsBehavior: Flickable.StopAtBounds
                Column {
                    id: home
                    width: parent.width
                    spacing: 12
                    SettingsRow { width: parent.width; label: "Appearance"; value: (MobileTheme.state.name || "").replace(/-/g, " "); onClicked: root.panel = "appearance" }
                    SettingsRow { width: parent.width; label: "Display"; value: MobileTheme.square ? "Square" : "Round"; onClicked: root.panel = "display" }
                    SettingsRow { width: parent.width; label: "Sound"; value: "Volume"; onClicked: root.panel = "sound" }
                    SettingsRow { width: parent.width; label: "Network"; value: "Wi-Fi"; onClicked: root.openPanel("network") }
                    SettingsRow { width: parent.width; label: "Bluetooth"; onClicked: root.panel = "bluetooth" }
                    SettingsRow { width: parent.width; label: "Battery"; onClicked: root.panel = "battery" }
                    SettingsRow { width: parent.width; label: "Storage"; onClicked: root.panel = "storage" }
                    SettingsRow { width: parent.width; label: "Apps"; onClicked: root.panel = "apps" }
                    SettingsRow { width: parent.width; label: "Weather"; value: "Location"; onClicked: root.panel = "weather" }
                    SettingsRow { width: parent.width; label: "Clipboard"; value: "History"; onClicked: root.panel = "clipboard" }
                    SettingsRow {
                        width: parent.width
                        label: "Do Not Disturb"
                        value: root.dnd ? "On" : "Off"
                        selected: root.dnd
                        onClicked: root.setDnd(!root.dnd)
                    }
                    SettingsRow { width: parent.width; label: "About"; value: "Device"; onClicked: root.panel = "about" }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        topPadding: 12
                        text: "Quick Wi-Fi, battery and volume stay in the shade. Settings is for the deeper panels."
                        color: MobileTheme.secondary
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 13
                    }
                }
            }
            AppearancePage {
                id: appearance
                visible: root.panel === "appearance"
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
            SoundPage {
                visible: root.panel === "sound"
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
            BatteryPage {
                visible: root.panel === "battery"
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
            BluetoothPage {
                visible: root.panel === "bluetooth"
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
            DisplayPage {
                visible: root.panel === "display"
                Layout.fillWidth: true
                Layout.fillHeight: true
                onAppearanceRequested: root.panel = "appearance"
            }
            StoragePage {
                visible: root.panel === "storage"
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
            AppsPage {
                visible: root.panel === "apps"
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
            WeatherPage {
                id: weatherPage
                visible: root.panel === "weather"
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
            AboutPage {
                visible: root.panel === "about"
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
            ColumnLayout {
                visible: root.panel === "clipboard"
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 10
                RowLayout {
                    Layout.fillWidth: true
                    TouchButton { Layout.fillWidth: true; label: "Refresh"; implicitHeight: 48; onClicked: clipRequest.run("history") }
                    TouchButton { Layout.fillWidth: true; label: "Clear"; implicitHeight: 48; onClicked: clipRequest.run("clear") }
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 10
                    model: clipRequest.items
                    boundsBehavior: Flickable.StopAtBounds
                    Text {
                        anchors.centerIn: parent
                        visible: clipRequest.items.length === 0
                        text: clipRequest.message || "Clipboard history is empty.\nCopied text will appear here."
                        horizontalAlignment: Text.AlignHCenter
                        color: MobileTheme.secondary
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 15
                    }
                    delegate: Rectangle {
                    required property var modelData
                    width: ListView.view.width
                    height: clipBody.implicitHeight + 24
                    radius: MobileTheme.radius(16)
                    color: MobileTheme.surface
                    ColumnLayout {
                        id: clipBody
                        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 12 }
                        spacing: 8
                        Text { Layout.fillWidth: true; text: modelData.preview; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: MobileTheme.foreground; font.family: MobileTheme.fontFamily; font.pixelSize: 15 }
                        RowLayout {
                            Layout.fillWidth: true
                            TouchButton { Layout.fillWidth: true; implicitHeight: 44; textSize: 13; label: "Copy"; onClicked: clipRequest.run("copy", {id: modelData.id}) }
                            TouchButton { Layout.fillWidth: true; implicitHeight: 44; textSize: 13; label: "Paste"; onClicked: clipRequest.run("paste", {id: modelData.id}) }
                            TouchButton { Layout.fillWidth: true; implicitHeight: 44; textSize: 13; label: modelData.pinned ? "Unpin" : "Pin"; onClicked: clipRequest.run("pin", {id: modelData.id}) }
                            TouchButton { Layout.fillWidth: true; implicitHeight: 44; textSize: 13; label: "×"; onClicked: clipRequest.run("delete", {id: modelData.id}) }
                        }
                    }
                    }
                }
            }
            Flickable {
                visible: root.panel === "network" && !root.speedOpen
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentHeight: netHome.height
                boundsBehavior: Flickable.StopAtBounds
                Column {
                    id: netHome
                    width: parent.width
                    spacing: 12
                    SettingsRow {
                        width: parent.width
                        label: "Wi-Fi"
                        value: root.wifi.radio === false ? "Off" : root.wifi.connected ? (root.wifi.ssid || root.wifi.connection || "Connected") : "Not connected"
                        onClicked: root.openPanel("wifi")
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "Cellular, VPN and hotspot wait on modem and extra NetworkManager profiles."
                        color: MobileTheme.secondary
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 13
                    }
                }
            }
            Flickable {
                visible: root.panel === "wifi" && !root.speedOpen
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentHeight: wifiPage.height
                boundsBehavior: Flickable.StopAtBounds
                Column {
                    id: wifiPage
                    width: parent.width
                    spacing: 12
                    SettingsRow {
                        width: parent.width
                        label: "Wi-Fi radio"
                        value: root.wifi.radio === false ? "Off" : "On"
                        selected: root.wifi.radio !== false
                        onClicked: net.run("radio", {enabled: root.wifi.radio === false})
                    }
                    Text {
                        width: parent.width
                        visible: root.wifi.connected === true
                        text: root.wifi.ssid || root.wifi.connection || "Connected"
                        color: MobileTheme.accent
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 20
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    DetailRow { width: parent.width; visible: root.wifi.connected === true; label: "IPv4"; value: (root.wifi.ipv4 || []).join("\n") || "—" }
                    DetailRow { width: parent.width; visible: root.wifi.connected === true; label: "IPv6"; value: (root.wifi.ipv6 || []).join("\n") || "—" }
                    DetailRow { width: parent.width; visible: root.wifi.connected === true; label: "Gateway"; value: root.wifi.gateway || "—" }
                    DetailRow { width: parent.width; visible: root.wifi.connected === true; label: "DNS in use"; value: (root.wifi.dns || []).join("\n") || "—" }
                    TouchButton { width: parent.width; label: "Speed test"; enabled: root.wifi.connected === true; onClicked: root.speedOpen = true }
                    Text { width: parent.width; text: "IP address"; color: MobileTheme.foreground; font.family: MobileTheme.fontFamily; font.pixelSize: 19; font.bold: true }
                    SettingsRow {
                        width: parent.width
                        label: "Automatic (DHCP)"
                        value: root.ipAuto ? "On" : "Off"
                        selected: root.ipAuto
                        onClicked: {
                            root.ipAuto = !root.ipAuto;
                            if (!root.ipAuto && !ipAddressField.text) root.fillCurrentIp();
                        }
                    }
                    TouchTextField {
                        id: ipAddressField
                        width: parent.width
                        implicitHeight: 50
                        visible: !root.ipAuto
                        placeholderText: "Address, e.g. 192.168.1.50/24"
                        inputMethodHints: Qt.ImhPreferNumbers
                        color: MobileTheme.foreground
                        placeholderTextColor: MobileTheme.secondary
                        background: Rectangle { radius: MobileTheme.radius(10); color: MobileTheme.surface; border.color: MobileTheme.muted }
                        editing: root.ipEditing === "address"
                        onEditingRequested: root.ipEditing = "address"
                    }
                    TouchTextField {
                        id: ipGatewayField
                        width: parent.width
                        implicitHeight: 50
                        visible: !root.ipAuto
                        placeholderText: "Gateway, e.g. 192.168.1.1"
                        inputMethodHints: Qt.ImhPreferNumbers
                        color: MobileTheme.foreground
                        placeholderTextColor: MobileTheme.secondary
                        background: Rectangle { radius: MobileTheme.radius(10); color: MobileTheme.surface; border.color: MobileTheme.muted }
                        editing: root.ipEditing === "gateway"
                        onEditingRequested: root.ipEditing = "gateway"
                    }
                    TouchTextField {
                        id: ipDnsField
                        width: parent.width
                        implicitHeight: 50
                        visible: !root.ipAuto
                        placeholderText: "DNS, e.g. 192.168.1.1 1.1.1.1"
                        inputMethodHints: Qt.ImhPreferNumbers
                        color: MobileTheme.foreground
                        placeholderTextColor: MobileTheme.secondary
                        background: Rectangle { radius: MobileTheme.radius(10); color: MobileTheme.surface; border.color: MobileTheme.muted }
                        editing: root.ipEditing === "dns"
                        onEditingRequested: root.ipEditing = "dns"
                    }
                    Text {
                        width: parent.width
                        visible: !root.ipAuto
                        wrapMode: Text.WordWrap
                        text: "Choose an address outside the router's DHCP range, or reserve it for this phone in the router, so no other device is given it."
                        color: MobileTheme.secondary
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 13
                    }
                    Row {
                        width: parent.width
                        spacing: 10
                        TouchButton {
                            width: root.ipAuto ? 0 : (parent.width - 10) / 2
                            visible: !root.ipAuto
                            label: "Use current"
                            enabled: root.wifi.connected === true
                            onClicked: root.fillCurrentIp()
                        }
                        TouchButton {
                            width: root.ipAuto ? parent.width : (parent.width - 10) / 2
                            label: "Apply IP settings"
                            enabled: !!root.wifi.uuid && !net.running
                            onClicked: {
                                root.ipEditing = "";
                                net.run("ipv4", root.ipAuto ? {uuid: root.wifi.uuid, auto: true}
                                    : {uuid: root.wifi.uuid, auto: false, address: ipAddressField.text,
                                       gateway: ipGatewayField.text, dns: ipDnsField.text});
                            }
                        }
                    }
                    Text { width: parent.width; text: "DNS"; color: MobileTheme.foreground; font.family: MobileTheme.fontFamily; font.pixelSize: 19; font.bold: true }
                    SettingsRow {
                        width: parent.width
                        label: "Automatic DNS"
                        value: root.dnsAuto ? "On" : "Off"
                        selected: root.dnsAuto
                        onClicked: root.dnsAuto = !root.dnsAuto
                    }
                    TouchTextField {
                        id: dnsField
                        width: parent.width
                        implicitHeight: 50
                        visible: !root.dnsAuto
                        placeholderText: "1.1.1.1 8.8.8.8"
                        color: MobileTheme.foreground
                        placeholderTextColor: MobileTheme.secondary
                        background: Rectangle { radius: MobileTheme.radius(10); color: MobileTheme.surface; border.color: MobileTheme.muted }
                        editing: root.dnsEditing
                        onEditingRequested: root.dnsEditing = true
                    }
                    TouchButton {
                        width: parent.width
                        label: "Apply DNS"
                        enabled: !!root.wifi.uuid && !net.running
                        onClicked: {
                            root.dnsEditing = false;
                            net.run("dns", {uuid: root.wifi.uuid, auto: root.dnsAuto, servers: dnsField.text});
                        }
                    }
                    TouchButton {
                        width: parent.width
                        label: "Forget this network"
                        enabled: !!root.wifi.uuid && !net.running
                        onClicked: net.run("forget", {uuid: root.wifi.uuid})
                    }
                    Row {
                        width: parent.width
                        spacing: 10
                        TouchButton { width: (parent.width - 10) / 2; label: "Refresh"; enabled: !net.running; onClicked: net.run("scan", {interface: root.wifi.interface}) }
                        TouchButton { width: (parent.width - 10) / 2; label: "Disconnect"; enabled: root.wifi.connected === true && !net.running; onClicked: net.run("disconnect", {interface: root.wifi.interface}) }
                    }
                    Text {
                        width: parent.width
                        visible: root.networkMessage.length > 0
                        text: root.networkMessage
                        wrapMode: Text.WordWrap
                        color: MobileTheme.secondary
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 13
                    }
                    Repeater {
                        model: root.networks
                        TouchButton {
                            required property var modelData
                            width: wifiPage.width
                            implicitHeight: 62
                            textSize: 14
                            label: (modelData.active ? "✓  " : "") + modelData.ssid + "   ·   " + modelData.signal + "%"
                            selected: modelData.active
                            enabled: !net.running
                            onClicked: {
                                if (/802\.1X|EAP|WEP/.test(modelData.security)) {
                                    root.networkMessage = "This security type needs an advanced NetworkManager profile.";
                                    return;
                                }
                                net.run("connect", {interface: root.wifi.interface, bssid: modelData.bssid, password: ""});
                            }
                        }
                    }
                }
            }
            SpeedTestView {
                visible: root.speedOpen
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
        }
    }
}
