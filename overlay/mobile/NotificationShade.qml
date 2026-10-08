import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import Quickshell
import Quickshell.Io
import Quickshell.Wayland
import Quickshell.Services.Notifications
import OmarchyMobile as Kit

PanelWindow {
    id: shade
    property string detail: ""
    property bool screenshotPrivacy: false
    property real topInset: 36
    property bool keyboardOwned: false
    property string editingField: ""
    property var selectedNetwork: null
    // The notification being answered in the reply popup (inline reply).
    property var replyTarget: null
    property var networks: []
    property string networkMessage: ""
    property var weather: ({configured: false})
    property string weatherMessage: ""
    property string pendingInput: ""
    property string weatherInput: ""
    property date month: new Date()
    property var expanded: ({})
    // When each notification came (by id), for "5m" on its card; the clock
    // only ticks while the shade is open.
    property var arrivedAt: ({})
    property double now: Date.now()
    Timer { interval: 30000; repeat: true; running: shade.opened; triggeredOnStart: true; onTriggered: shade.now = Date.now() }
    function ago(notification) {
        const at = notification ? shade.arrivedAt[notification.id] : undefined;
        if (!at) return "";
        const minutes = Math.floor((shade.now - at) / 60000);
        if (minutes < 1) return "now";
        if (minutes < 60) return minutes + "m";
        if (minutes < 1440) return Math.floor(minutes / 60) + "h";
        return Qt.formatDate(new Date(at), "ddd");
    }
    function picture(notification) {
        const image = notification ? (notification.image || "") : "";
        return image.startsWith("/") ? "file://" + image : image;
    }
    property var clips: []
    property string clipMessage: ""
    property string clipInput: ""
    readonly property bool opened: motion.progress > 0
    // How far it is pulled down, 0-1 (the home screen fades out under it).
    readonly property real progress: motion.progress
    readonly property var battery: MobileStatus.battery
    readonly property var wifi: MobileStatus.wifi
    readonly property var bluetooth: MobileStatus.bluetooth
    readonly property bool bluetoothConnected: (bluetooth.devices || []).some(d => d.connected)
    readonly property var volume: MobileStatus.volume
    readonly property var stats: MobileStatus.stats
    Binding { target: MobileStatus; property: "statsShown"; value: shade.opened && shade.detail === "stats" }
    readonly property int notificationCount: notifications.trackedNotifications.values.length
    readonly property var notificationValues: notifications.trackedNotifications.values
    signal opening()
    signal arrived(var notification)
    signal keyboardRequested(string action)
    signal volumeRequested(string action)
    signal settingsRequested(string panel)
    // The media card's player, by its desktop entry (MPRIS DesktopEntry).
    signal appRequested(string desktopEntry)
    // A notification was tapped: the shell opens its app.
    signal notificationOpened(var notification)
    // A reply typed for an Android app's notification (see androidReply).
    signal androidReplyRequested(var notification, string text)
    // An Android app's notification (Waydroid's bridge) carries its reply
    // action as a plain button, which would send no text; Reply here takes the
    // text in the shade's field and the shell has Android fill it in
    // (keyboard-bridge's ReplyService). The "default" action is the tap on
    // the notification itself, not a button.
    function androidReply(n) {
        return !!n && String(n.desktopEntry || "").startsWith("waydroid.")
            && (n.actions || []).some(a => /^reply$/i.test(a.text));
    }
    function pills(n) {
        return n ? (n.actions || []).filter(a => a.identifier !== "default"
                                                 && !(androidReply(n) && /^reply$/i.test(a.text))) : [];
    }
    NotificationGroups { id: groups }
    anchors { top: true; left: true; right: true; bottom: true }
    // Keyboard exclusive-zone changes must not resize the shade behind menus.
    margins.top: topInset
    exclusionMode: ExclusionMode.Ignore
    exclusiveZone: 0
    color: "transparent"
    visible: true
    mask: Region { width: shade.opened || motion.dragging ? shade.width : 0; height: shade.opened || motion.dragging ? shade.height : 0 }
    WlrLayershell.namespace: "omarchy-mobile-shade"
    WlrLayershell.layer: WlrLayer.Top
    // On demand, not exclusive: Hyprland sends every touch to an exclusive layer,
    // so the on-screen keyboard above the shade would never get a tap.
    WlrLayershell.keyboardFocus: detail === "wifi" || detail === "reply" ? WlrKeyboardFocus.OnDemand : WlrKeyboardFocus.None
    function amount(value, unit, decimals) { return value === null || value === undefined ? "—" : Number(value).toFixed(decimals || 0) + " " + unit; }
    function begin() { opening(); motion.begin(); MobileStatus.refresh(); hotspotAction("status"); vpnAction(["status"]); }
    function move(distance, velocity) { motion.update(distance, velocity); }
    function release() { motion.finish(); }
    function cancel() { motion.cancel(); }
    function open() { opening(); motion.animateTo(1); MobileStatus.refresh(); fetchWeather("status", {}); hotspotAction("status"); vpnAction(["status"]); }
    // VPNs (omarchy-mobile-vpn): the toggle drives the one that is up, else
    // the first; one still without its password opens Settings for it.
    property var vpns: []
    readonly property var vpn: vpns.find(v => v.state !== "off") || vpns[0] || null
    // Every open checks the VPN and hotspot states in the background; only
    // an on or off the user asked for shows "…" on its tile. Shown for the
    // checks as well, both tiles blinked at each pull of the shade. A tap
    // during a check runs after it.
    property bool vpnBusy: false
    property var vpnQueued: null
    function vpnAction(args) {
        if (vpnProcess.running) { if (args[0] !== "status") vpnQueued = args; return; }
        vpnBusy = args[0] !== "status";
        vpnProcess.command = [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-vpn"].concat(args);
        vpnProcess.running = true;
    }
    function vpnToggle() {
        if (!vpn) return;
        if (vpn.state !== "off") vpnAction(["off", vpn.name]);
        else if (!vpn.saved) shade.settingsRequested("network");
        else vpnAction(["on", vpn.name]);
    }
    // The Wi-Fi hotspot (omarchy-mobile-hotspot): its state, and on or off.
    property var hotspot: ({})
    property bool hotspotBusy: false
    property string hotspotQueued: ""
    function hotspotAction(action) {
        if (hotspotProcess.running) { if (action !== "status") hotspotQueued = action; return; }
        hotspotBusy = action !== "status";
        hotspotProcess.command = [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-hotspot", action];
        hotspotProcess.running = true;
    }
    // Known before the first pull: the Hotspot and VPN tiles appear only once
    // their state is, and appearing mid-pull moved every tile after them.
    Component.onCompleted: { hotspotAction("status"); vpnAction(["status"]); }
    function close() { closeDetail(); motion.animateTo(0); }
    function toggle() { if (opened) close(); else open(); }
    function releaseKeyboard() {
        editingField = "";
        password.focus = false;
        if (keyboardOwned) keyboardRequested("hide");
        keyboardOwned = false;
    }
    function editField(name, field) {
        editingField = name;
        field.forceActiveFocus(Qt.MouseFocusReason);
        keyboardOwned = true;
        keyboardRequested("show");
    }
    function closeDetail() { releaseKeyboard(); detail = ""; selectedNetwork = null; password.text = ""; replyTarget = null; replyField.text = ""; }
    // Answer a notification that takes an inline reply (a text): the popup
    // shows the message and a field, with the keyboard up.
    function startReply(notification) {
        if (!notification || !(notification.hasInlineReply || androidReply(notification))) return;
        if (!opened) open();
        closeDetail();
        replyTarget = notification;
        detail = "reply";
        Qt.callLater(() => { if (shade.detail === "reply") shade.editField("reply", replyField); });
    }
    function sendReply() {
        const target = replyTarget, text = replyField.text;
        if (!target || !text.trim()) return;
        if (target.hasInlineReply) target.sendInlineReply(text);
        else androidReplyRequested(target, text);
        closeDetail();
        target.dismiss();
    }
    function showDetail(name) {
        if (screenshotPrivacy && (name === "wifi" || name === "weather" || name === "clipboard")) return;
        if (!opened) open();
        closeDetail(); detail = name;
        if (name === "wifi") networkAction("scan", {});
        if (name === "weather") fetchWeather("status", {});
        if (name === "calendar") month = new Date(clock.date.getFullYear(), clock.date.getMonth(), 1);
        if (name === "stats") MobileStatus.refresh();
        if (name === "clipboard") clipboardAction("history", {});
    }
    function detailTitle() {
        if (detail === "reply") return replyTarget ? "Reply to " + replyTarget.summary : "Reply";
        return {wifi: "Wi-Fi", battery: "Battery", calendar: "Calendar", weather: "Weather", stats: "Performance", clipboard: "Clipboard", speedtest: "Speed test"}[detail] || "";
    }
    function toggleGroup(key) {
        const next = Object.assign({}, expanded);
        next[key] = !next[key];
        expanded = next;
    }
    function bluetoothPower() {
        if (bluetoothProcess.running) return;
        bluetoothProcess.command = [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-bluetooth",
                                    "power", bluetooth.powered === true ? "off" : "on"];
        bluetoothProcess.running = true;
    }
    function networkAction(action, values) {
        if (networkProcess.running) return;
        if (action !== "radio" && !wifi.interface) return;
        values.interface = wifi.interface || "";
        pendingInput = JSON.stringify(values) + "\n";
        networkProcess.command = [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-wifi", action];
        networkMessage = action === "scan" ? "Looking for nearby networks…" : action === "connect" ? "Connecting…" : "Updating…";
        networkProcess.running = true;
    }
    function clipboardAction(action, values) {
        if (clipProcess.running) return;
        clipInput = values && Object.keys(values).length ? JSON.stringify(values) + "\n" : "";
        clipProcess.command = [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-clipboard", action];
        clipProcess.running = true;
    }
    function openCalendar(day) {
        Quickshell.execDetached([Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-app", "launch", "calendar", "day:" + day]);
        close();
    }
    function fetchWeather(action, values) {
        if (weatherProcess.running) return;
        weatherInput = JSON.stringify(values) + "\n";
        weatherProcess.command = [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-weather", action];
        weatherMessage = "Updating…";
        weatherProcess.running = true;
    }
    function weatherName(code) {
        if (code === 0) return "Clear skies";
        if (code <= 3) return "Cloudy";
        if (code <= 48) return "Fog";
        if (code <= 67) return "Rain";
        if (code <= 77) return "Snow";
        if (code <= 82) return "Showers";
        if (code <= 86) return "Snow showers";
        return "Thunderstorms";
    }
    DrawerMotion {
        id: motion; travel: sheet.height
        onClosed: shade.closeDetail()
        onSettledOpenChanged: if (settledOpen) shade.fetchWeather("status", {})
    }
    SystemClock { id: clock; precision: SystemClock.Minutes }
    Timer { interval: 5000; repeat: true; running: shade.opened; onTriggered: MobileStatus.refresh() }
    Process {
        id: networkProcess; stdinEnabled: true
        stdout: StdioCollector {}
        onStarted: { write(shade.pendingInput); shade.pendingInput = ""; stdinEnabled = false; }
        onExited: (code, status) => {
            stdinEnabled = true;
            try {
                const result = JSON.parse(stdout.text);
                shade.networkMessage = result.ok ? "" : result.error || "Network request failed";
                if (result.networks) shade.networks = result.networks;
                else if (result.ok) { shade.selectedNetwork = null; password.text = ""; if (shade.detail === "wifi") shade.releaseKeyboard(); }
            } catch (e) { shade.networkMessage = "Network request failed"; }
            MobileStatus.refresh();
        }
    }
    Process {
        id: bluetoothProcess
        onExited: MobileStatus.refresh()
    }
    Process {
        id: vpnProcess
        stdout: StdioCollector {}
        onExited: {
            try {
                const result = JSON.parse(stdout.text);
                if (result.ok) shade.vpns = result.vpns || [];
                else { shade.networkMessage = result.error || "The VPN did not start"; shade.vpnQueued = shade.vpnQueued || ["status"]; }
            } catch (e) {}
            shade.vpnBusy = false;
            if (shade.vpnQueued) { const args = shade.vpnQueued; shade.vpnQueued = null; shade.vpnAction(args); }
        }
    }
    Process {
        id: hotspotProcess
        stdout: StdioCollector {}
        onExited: {
            try {
                const result = JSON.parse(stdout.text);
                if (result.ok) shade.hotspot = result;
                else shade.networkMessage = result.error || "The hotspot did not start";
            } catch (e) {}
            shade.hotspotBusy = false;
            if (shade.hotspotQueued) { const action = shade.hotspotQueued; shade.hotspotQueued = ""; shade.hotspotAction(action); }
        }
    }
    Process {
        id: clipProcess; stdinEnabled: true
        stdout: StdioCollector {}
        onStarted: {
            if (shade.clipInput) write(shade.clipInput);
            shade.clipInput = "";
            stdinEnabled = false;
        }
        onExited: {
            stdinEnabled = true;
            try {
                const result = JSON.parse(stdout.text);
                shade.clipMessage = result.error || "";
                if (result.items) shade.clips = result.items;
            } catch (e) { shade.clipMessage = "Clipboard unavailable"; }
        }
    }
    Process {
        id: weatherProcess; stdinEnabled: true
        stdout: StdioCollector {}
        onStarted: { write(shade.weatherInput); shade.weatherInput = ""; stdinEnabled = false; }
        onExited: (code, status) => {
            stdinEnabled = true;
            try {
                const result = JSON.parse(stdout.text);
                shade.weatherMessage = result.error || "";
                if (result.configured !== undefined) shade.weather = result;
            } catch (e) { shade.weatherMessage = "Weather unavailable"; }
        }
    }
    NotificationServer {
        id: notifications
        keepOnReload: true
        actionsSupported: true
        bodySupported: true
        imageSupported: true
        persistenceSupported: true
        inlineReplySupported: true
        onNotification: notification => {
            notification.tracked = true;
            shade.arrivedAt[notification.id] = Date.now();
            // Bound session history; old cards are explicitly dismissed.
            const all = trackedNotifications.values;
            if (all.length > 100) all[0].dismiss();
            shade.arrived(notification);
        }
    }
    Rectangle {
        anchors.fill: parent; color: "black"; opacity: motion.progress * 0.4
        MouseArea { anchors.fill: parent; onClicked: shade.close() }
    }
    Rectangle {
        id: sheet
        width: parent.width; height: parent.height
        y: -height * (1 - motion.progress)
        radius: motion.progress > 0.98 ? 0 : 28
        color: MobileTheme.background
        border.color: MobileTheme.muted; border.width: 1
        clip: true
        MouseArea { anchors.fill: parent; onClicked: {} }
        ColumnLayout {
            anchors.fill: parent; anchors.margins: 22; spacing: 16
            Item {
                Layout.fillWidth: true; implicitHeight: 72
                Text { font.family: MobileTheme.fontFamily; y: 0; text: Qt.formatDateTime(clock.date, "dddd, MMMM d").toUpperCase(); color: MobileTheme.accent; font.pixelSize: 11; font.letterSpacing: 1.5 }
                Text { font.family: MobileTheme.fontFamily; y: 24; text: "At a glance"; color: MobileTheme.foreground; font.pixelSize: 32; font.bold: true }
                // Performance: CPU and memory use, cores, temperatures and top processes.
                TouchButton { anchors.right: parent.right; anchors.rightMargin: 108; y: 19; implicitWidth: 46; implicitHeight: 46; label: "\u{F035B}"; onClicked: shade.showDetail("stats") }
                TouchButton { anchors.right: parent.right; anchors.rightMargin: 54; y: 19; implicitWidth: 46; implicitHeight: 46; label: "⧉"; onClicked: shade.showDetail("clipboard") }
                TouchButton { anchors.right: parent.right; y: 19; implicitWidth: 46; implicitHeight: 46; label: "⌃"; onClicked: shade.close() }
            }
            GridLayout {
                Layout.fillWidth: true; columns: 4; columnSpacing: 10; rowSpacing: 10
                QuickToggle {
                    Layout.fillWidth: true; Layout.preferredWidth: 1
                    symbol: "󰖩"; label: "Wi-Fi"
                    active: shade.wifi.radio !== false
                    enabled: shade.wifi.available === true || shade.wifi.radio === false
                    onClicked: shade.networkAction("radio", {enabled: shade.wifi.radio === false})
                }
                QuickToggle {
                    Layout.fillWidth: true; Layout.preferredWidth: 1
                    symbol: shade.bluetooth.powered !== true ? "󰂲" : shade.bluetoothConnected ? "󰂱" : "󰂯"
                    // Starting the stack takes a few seconds when no controller exists yet.
                    label: bluetoothProcess.running ? "…" : "Bluetooth"
                    active: shade.bluetooth.powered === true
                    enabled: (shade.bluetooth.available === true || shade.bluetooth.startable === true) && !bluetoothProcess.running
                    onClicked: shade.bluetoothPower()
                }
                QuickToggle {
                    Layout.fillWidth: true; Layout.preferredWidth: 1
                    symbol: shade.volume.muted ? "󰝟" : "󰕾"; label: shade.volume.muted ? "Muted" : "Sound"
                    active: shade.volume.available === true && !shade.volume.muted
                    enabled: shade.volume.available === true
                    onClicked: shade.volumeRequested("mute")
                }
                QuickToggle {
                    Layout.fillWidth: true; Layout.preferredWidth: 1
                    visible: shade.hotspot.available === true
                    symbol: "󰀃"; label: shade.hotspotBusy ? "…" : "Hotspot"
                    active: shade.hotspot.active === true
                    enabled: !shade.hotspotBusy
                    onClicked: shade.hotspotAction(shade.hotspot.active === true ? "off" : "on")
                }
                QuickToggle {
                    Layout.fillWidth: true; Layout.preferredWidth: 1
                    visible: shade.vpn !== null
                    symbol: "\u{F0582}"
                    label: shade.vpnBusy ? "…" : shade.vpn && shade.vpn.state === "connecting" ? "Connecting" : "VPN"
                    active: shade.vpn !== null && shade.vpn.state === "on"
                    enabled: !shade.vpnBusy
                    onClicked: shade.vpnToggle()
                }
                QuickToggle {
                    Layout.fillWidth: true; Layout.preferredWidth: 1
                    symbol: "󰂛"; label: "DND"
                    active: MobileStatus.dnd
                    onClicked: MobileStatus.setDnd(!MobileStatus.dnd)
                }
                QuickToggle {
                    readonly property var torch: MobileStatus.controls.torch || ({})
                    Layout.fillWidth: true; Layout.preferredWidth: 1
                    visible: torch.available === true
                    symbol: torch.on ? "󰉄" : "󰉅"; label: "Torch"
                    active: torch.on === true
                    onClicked: MobileStatus.control(["torch", "toggle"])
                }
                // The keyboard for an app that does not ask for one (a
                // terminal). A tap on the bottom edge did this until taps
                // there went to the app, as on Android (October 7).
                QuickToggle {
                    Layout.fillWidth: true; Layout.preferredWidth: 1
                    symbol: "󰌌"; label: "Keyboard"
                    onClicked: { shade.close(); shade.keyboardRequested("show"); }
                }
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 12
                visible: !!MobileStatus.controls.backlight
                // Tap for automatic brightness, where there is a light sensor.
                Item {
                    readonly property bool auto: MobileStatus.controls.auto === true
                    implicitWidth: 40; implicitHeight: 40
                    Text {
                        anchors.centerIn: parent
                        font.family: MobileTheme.fontFamily; font.pixelSize: 20
                        text: parent.auto ? "󰃡" : "󰃞"
                        color: parent.auto ? MobileTheme.accent : MobileTheme.secondary
                    }
                    TapHandler {
                        enabled: MobileStatus.controls.light === true
                        onTapped: MobileStatus.control(["auto", parent.auto ? "off" : "on"])
                    }
                }
                Slider {
                    id: brightness
                    Layout.fillWidth: true
                    // Released level not yet confirmed by the helper, or -1.
                    property real pending: -1
                    from: 1; to: 100; stepSize: 1
                    onMoved: brightnessThrottle.want(Math.round(value), false)
                    onPressedChanged: if (!pressed) {
                        pending = value;
                        brightnessSettle.restart();
                        brightnessThrottle.want(Math.round(value), true);
                    }
                    background: Rectangle {
                        x: brightness.leftPadding; y: brightness.topPadding + (brightness.availableHeight - height) / 2
                        implicitWidth: 200; implicitHeight: 12
                        width: brightness.availableWidth; height: 12; radius: MobileTheme.radius(6)
                        color: MobileTheme.muted
                        Rectangle {
                            width: parent.width * brightness.visualPosition; height: parent.height
                            radius: MobileTheme.radius(6); color: MobileTheme.accent
                        }
                    }
                    handle: Rectangle {
                        x: brightness.leftPadding + brightness.visualPosition * (brightness.availableWidth - width)
                        y: brightness.topPadding + (brightness.availableHeight - height) / 2
                        implicitWidth: 22; implicitHeight: 34
                        radius: MobileTheme.radius(11)
                        color: MobileTheme.foreground
                    }
                    // Follow the screen's level, but neither pull the handle
                    // mid-drag nor snap it back before the new level lands.
                    Binding {
                        target: brightness
                        property: "value"
                        value: MobileStatus.controls.backlight ? MobileStatus.controls.backlight.percent : 50
                        when: !brightness.pressed && brightness.pending < 0
                        restoreMode: Binding.RestoreNone
                    }
                    Connections {
                        target: MobileStatus
                        function onControlsChanged() {
                            const light = MobileStatus.controls.backlight;
                            if (brightness.pending >= 0 && light && Math.abs(light.percent - brightness.pending) <= 1)
                                brightness.pending = -1;
                        }
                    }
                    Timer { id: brightnessSettle; interval: 2000; onTriggered: brightness.pending = -1 }
                    // A level at once when the drag starts, then about one per
                    // frame, then the last. Each is a panel command; from
                    // kernel #194 they wait for frames instead of flickering
                    // (80 ms apart before it).
                    Timer {
                        id: brightnessThrottle
                        property int wanted: -1
                        property int sent: -1
                        property bool final: false
                        function want(level, last) {
                            wanted = level;
                            final = final || last;
                            if (!running) send();
                        }
                        function send() {
                            sent = wanted;
                            MobileStatus.brightness(wanted, final);
                            final = false;
                            start();
                        }
                        interval: 16
                        onTriggered: if (wanted !== sent || final) send()
                    }
                }
                Text { font.family: MobileTheme.fontFamily; text: "󰃠"; color: MobileTheme.secondary; font.pixelSize: 20 }
            }
            GridLayout {
                Layout.fillWidth: true; columns: 2; columnSpacing: 12; rowSpacing: 12
                ShadeTile {
                    Layout.fillWidth: true; Layout.preferredWidth: 1
                    symbol: "⌁"; title: !shade.screenshotPrivacy && shade.wifi.connected ? shade.wifi.ssid || shade.wifi.connection : "Wi-Fi"
                    subtitle: shade.wifi.radio === false ? "Radio off" : shade.wifi.connected ? "Connected · " + shade.amount(shade.wifi.signal, "%") : "Choose a network"
                    highlight: shade.wifi.connected === true; onClicked: shade.showDetail("wifi")
                }
                ShadeTile {
                    Layout.fillWidth: true; Layout.preferredWidth: 1
                    symbol: shade.battery.charging ? "ϟ" : "▯"; title: shade.amount(shade.battery.capacity, "%")
                    subtitle: shade.amount(shade.battery.charge_mah, "mAh") + " · " + shade.amount(shade.battery.current_ma, "mA")
                    onClicked: shade.showDetail("battery")
                }
                ShadeTile {
                    Layout.fillWidth: true; Layout.preferredWidth: 1
                    symbol: "◷"; title: Qt.formatDateTime(clock.date, "h:mm AP")
                    subtitle: Qt.formatDateTime(clock.date, "MMM d, yyyy") + " · Calendar"; onClicked: shade.showDetail("calendar")
                }
                ShadeTile {
                    Layout.fillWidth: true; Layout.preferredWidth: 1
                    symbol: WeatherIcons.glyph(shade.weather.kind || "")
                    title: shade.weather.available ? shade.amount(shade.weather.current.temperature_2m, "°F") : "Weather"
                    subtitle: shade.weather.available ? (shade.weather.condition || shade.weatherName(shade.weather.current.weather_code)) + (shade.weather.stale ? " · Offline" : "") : "Set a location"
                    onClicked: shade.showDetail("weather")
                }
            }
            // What is playing (MPRIS), above the notifications as on Android.
            MediaCard {
                Layout.fillWidth: true
                active: shade.opened && !shade.screenshotPrivacy
                visible: player !== null && !shade.screenshotPrivacy
                onOpenRequested: entry => { shade.close(); shade.appRequested(entry); }
            }
            RowLayout {
                Layout.fillWidth: true
                Text { font.family: MobileTheme.fontFamily; text: "Notifications"; color: MobileTheme.foreground; font.pixelSize: 19; font.bold: true; Layout.fillWidth: true }
                TouchButton {
                    visible: !shade.screenshotPrivacy && shade.notificationCount > 0; implicitWidth: 90; implicitHeight: 42; label: "Clear all"; textSize: 13
                    onClicked: notifications.trackedNotifications.values.slice().forEach(n => n.dismiss())
                }
            }
            ListView {
                id: notices
                Layout.fillWidth: true; Layout.fillHeight: true
                clip: true; spacing: 10
                model: shade.screenshotPrivacy ? [] : groups.grouped(notifications.trackedNotifications.values.slice().reverse())
                boundsBehavior: Flickable.StopAtBounds
                Text { font.family: MobileTheme.fontFamily; anchors.centerIn: parent; visible: shade.screenshotPrivacy || shade.notificationCount === 0; text: shade.screenshotPrivacy ? "Notification contents hidden\nfor this screenshot." : "All caught up\nYour notifications will appear here."; horizontalAlignment: Text.AlignHCenter; color: MobileTheme.secondary; font.pixelSize: 15; lineHeight: 1.5 }
                delegate: Rectangle {
                    id: card
                    required property var modelData
                    readonly property bool many: modelData.items.length > 1
                    readonly property bool open: !many || shade.expanded[modelData.key] === true
                    readonly property var latest: modelData.items[0]
                    width: notices.width; height: noticeContent.implicitHeight + 30; radius: MobileTheme.radius(20); color: MobileTheme.surface
                    border.width: 1; border.color: Qt.rgba(MobileTheme.muted.r, MobileTheme.muted.g, MobileTheme.muted.b, 0.7)
                    // Swiped left or right, as on Android: the card follows the
                    // finger, and past a third of its width (or flung) it leaves
                    // and its notifications are dismissed; short of that it
                    // springs back.
                    property real offset: 0
                    transform: Translate { x: card.offset }
                    opacity: 1 - Math.min(0.75, Math.abs(offset) / width)
                    DragHandler {
                        id: cardSwipe
                        target: null
                        yAxis.enabled: false
                        onActiveTranslationChanged: if (active) card.offset = activeTranslation.x
                        onActiveChanged: {
                            if (active) { settle.stop(); return; }
                            const speed = centroid.velocity.x;
                            const flung = Math.abs(speed) > 900 && Math.abs(card.offset) > 32 && Math.sign(speed) === Math.sign(card.offset);
                            if (Math.abs(card.offset) > card.width / 3 || flung) {
                                leave.to = (card.offset < 0 ? -1 : 1) * (card.width + 24);
                                leave.start();
                            } else {
                                settle.start();
                            }
                        }
                    }
                    NumberAnimation { id: leave; target: card; property: "offset"; duration: 160; easing.type: Easing.OutCubic; onFinished: groups.dismissAll(card.modelData.items) }
                    NumberAnimation { id: settle; target: card; property: "offset"; to: 0; duration: 220; easing.type: Easing.OutBack }
                    ColumnLayout {
                        id: noticeContent; anchors { left: parent.left; right: parent.right; top: parent.top; margins: 15 } spacing: 8
                        RowLayout {
                            spacing: 10
                            NotificationIcon { notification: card.latest; appName: card.modelData.appName; implicitWidth: 30; implicitHeight: 30 }
                            Text { font.family: MobileTheme.fontFamily; text: modelData.appName.toUpperCase(); textFormat: Text.PlainText; color: MobileTheme.accent; font.pixelSize: 11; font.letterSpacing: 1.2; font.bold: true; elide: Text.ElideRight; Layout.maximumWidth: noticeContent.width * 0.5 }
                            Text { font.family: MobileTheme.fontFamily; text: (many ? modelData.items.length + " · " : "") + shade.ago(card.latest); color: MobileTheme.muted; font.pixelSize: 11; Layout.fillWidth: true; elide: Text.ElideRight }
                            GhostButton { visible: many; glyph: open ? "▾" : "▸"; onClicked: shade.toggleGroup(modelData.key) }
                            GhostButton { glyph: "×"; onClicked: groups.dismissAll(modelData.items) }
                        }
                        Text {
                            visible: !open
                            font.family: MobileTheme.fontFamily; Layout.fillWidth: true
                            text: latest ? latest.summary : ""
                            textFormat: Text.PlainText; wrapMode: Text.WordWrap
                            color: MobileTheme.foreground; font.bold: true; font.pixelSize: 16
                            TapHandler { onTapped: shade.notificationOpened(card.latest) }
                        }
                        Repeater {
                            model: open ? modelData.items : []
                            ColumnLayout {
                                required property var modelData
                                Layout.fillWidth: true; spacing: 6
                                RowLayout {
                                    visible: many
                                    Layout.fillWidth: true
                                    Text {
                                        font.family: MobileTheme.fontFamily; Layout.fillWidth: true; text: modelData.summary; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: MobileTheme.foreground; font.bold: true; font.pixelSize: 15
                                        TapHandler { onTapped: shade.notificationOpened(modelData) }
                                    }
                                    Text { font.family: MobileTheme.fontFamily; text: shade.ago(modelData); color: MobileTheme.muted; font.pixelSize: 11 }
                                    GhostButton { glyph: "×"; size: 32; onClicked: modelData.dismiss() }
                                }
                                Text {
                                    visible: !many; font.family: MobileTheme.fontFamily; Layout.fillWidth: true; text: modelData.summary; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: MobileTheme.foreground; font.bold: true; font.pixelSize: 16
                                    TapHandler { onTapped: shade.notificationOpened(modelData) }
                                }
                                RowLayout {
                                    Layout.fillWidth: true; spacing: 12
                                    visible: modelData.body.length > 0 || shade.picture(modelData) !== ""
                                    Text {
                                        font.family: MobileTheme.fontFamily; Layout.fillWidth: true; Layout.alignment: Qt.AlignTop; text: modelData.body; textFormat: Text.PlainText; wrapMode: Text.Wrap; maximumLineCount: 5; elide: Text.ElideRight; color: MobileTheme.secondary; font.pixelSize: 14; lineHeight: 1.1; visible: text.length > 0
                                        TapHandler { onTapped: shade.notificationOpened(modelData) }
                                    }
                                    // The notification's own picture (a photo, a contact).
                                    Rectangle {
                                        visible: shade.picture(modelData) !== "" && thumb.status === Image.Ready
                                        Layout.alignment: Qt.AlignTop
                                        implicitWidth: 56; implicitHeight: 56; radius: MobileTheme.radius(12); color: MobileTheme.selection; clip: true
                                        Image { id: thumb; anchors.fill: parent; source: shade.picture(modelData); sourceSize: Qt.size(168, 168); fillMode: Image.PreserveAspectCrop; asynchronous: false }
                                    }
                                }
                                Flow {
                                    id: actionRow
                                    Layout.fillWidth: true; spacing: 8
                                    readonly property var notification: modelData
                                    Rectangle {
                                        visible: actionRow.notification.hasInlineReply || shade.androidReply(actionRow.notification)
                                        implicitWidth: replyText.implicitWidth + 30; implicitHeight: 38
                                        radius: MobileTheme.radius(19)
                                        color: replyTap.pressed ? Qt.darker(MobileTheme.accent, 1.2) : MobileTheme.accent
                                        Text { id: replyText; anchors.centerIn: parent; text: "Reply"; color: MobileTheme.background; font.family: MobileTheme.fontFamily; font.pixelSize: 13; font.bold: true }
                                        TapHandler { id: replyTap; onTapped: shade.startReply(actionRow.notification) }
                                    }
                                    Repeater {
                                        model: shade.pills(modelData)
                                        Rectangle {
                                            required property var modelData
                                            implicitWidth: pillText.implicitWidth + 30; implicitHeight: 38
                                            radius: MobileTheme.radius(19)
                                            color: pillTap.pressed ? MobileTheme.selection : "transparent"
                                            border.width: 1; border.color: MobileTheme.accent
                                            Text { id: pillText; anchors.centerIn: parent; text: modelData.text; color: MobileTheme.accent; font.family: MobileTheme.fontFamily; font.pixelSize: 13; font.bold: true }
                                            TapHandler { id: pillTap; onTapped: modelData.invoke() }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            Item {
                Layout.fillWidth: true; implicitHeight: 22
                Rectangle { anchors.centerIn: parent; width: 64; height: 4; radius: 2; color: MobileTheme.secondary; opacity: 0 }
            }
        }
        DrawerPull {
            anchors.fill: parent
            directionSign: -1
            available: shade.detail === "" && motion.progress > 0.05 && !motion.dragging
            atTop: notices.atYEnd || notices.contentHeight <= notices.height
            headerBottom: notices.y + 22
            blocked: shade.detail !== ""
            onStarted: motion.begin()
            onMoved: (distance, velocity) => motion.update(Math.min(0, distance), velocity)
            onReleased: motion.finishClose()
            onCanceled: if (motion.dragging) motion.cancel()
        }
    }
    Rectangle {
        anchors.fill: parent; visible: shade.detail !== ""; color: "#88000000"
        MouseArea { anchors.fill: parent; onClicked: shade.closeDetail() }
    }
    Rectangle {
        id: popup
        visible: shade.detail !== ""
        width: parent.width - 32
        height: Math.min(popupContent.implicitHeight + 44, parent.height - (shade.keyboardOwned ? ((MobileTheme.state.device || {}).keyboardHeight || 280) + 48 : 48))
        anchors.horizontalCenter: parent.horizontalCenter
        y: shade.selectedNetwork || shade.detail === "reply" ? 24 : Math.max(24, (parent.height - height) / 2 - 30)
        color: MobileTheme.background; radius: MobileTheme.radius(26); border.width: 1; border.color: MobileTheme.accent
        MouseArea { anchors.fill: parent; onClicked: {} }
        Flickable {
            anchors.fill: parent; anchors.margins: 22; clip: true
            contentHeight: popupContent.implicitHeight; boundsBehavior: Flickable.StopAtBounds
            ColumnLayout {
                id: popupContent; width: parent.width; spacing: 16
                RowLayout {
                    Layout.fillWidth: true
                    Text { font.family: MobileTheme.fontFamily; text: shade.detailTitle(); color: MobileTheme.foreground; font.pixelSize: 26; font.bold: true; Layout.fillWidth: true }
                    TouchButton { implicitWidth: 44; implicitHeight: 44; label: "×"; onClicked: shade.detail === "speedtest" ? shade.showDetail("wifi") : shade.closeDetail() }
                }
                ColumnLayout {
                    visible: shade.detail === "reply"; Layout.fillWidth: true; spacing: 12
                    Text {
                        Layout.fillWidth: true
                        text: shade.replyTarget ? shade.replyTarget.body : ""
                        textFormat: Text.PlainText; wrapMode: Text.Wrap; maximumLineCount: 6; elide: Text.ElideRight
                        color: MobileTheme.secondary; font.family: MobileTheme.fontFamily; font.pixelSize: 15
                    }
                    TouchTextField { font.family: MobileTheme.fontFamily;
                        id: replyField; Layout.fillWidth: true; implicitHeight: 50
                        placeholderText: shade.replyTarget && shade.replyTarget.inlineReplyPlaceholder ? shade.replyTarget.inlineReplyPlaceholder : "Message"
                        color: MobileTheme.foreground; placeholderTextColor: MobileTheme.secondary
                        background: Rectangle { radius: MobileTheme.radius(10); color: MobileTheme.surface; border.color: MobileTheme.muted }
                        editing: shade.editingField === "reply" && shade.detail === "reply"
                        onEditingRequested: shade.editField("reply", replyField)
                        onAccepted: shade.sendReply()
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        TouchButton { Layout.fillWidth: true; label: "Cancel"; onClicked: shade.closeDetail() }
                        TouchButton { Layout.fillWidth: true; label: "Send"; selected: true; enabled: replyField.text.trim().length > 0; onClicked: shade.sendReply() }
                    }
                }
                ColumnLayout {
                    visible: shade.detail === "battery"; Layout.fillWidth: true; spacing: 15
                    Text { font.family: MobileTheme.fontFamily; text: shade.amount(shade.battery.capacity, "%"); color: MobileTheme.accent; font.pixelSize: 48; font.bold: true }
                    DetailRow { Layout.fillWidth: true; label: "Battery status"; value: shade.battery.status || "Unavailable" }
                    DetailRow { Layout.fillWidth: true; label: "Stored charge"; value: shade.amount(shade.battery.charge_mah, "mAh") }
                    DetailRow { Layout.fillWidth: true; label: "Reported full"; value: shade.amount(shade.battery.full_mah, "mAh") }
                    DetailRow { Layout.fillWidth: true; label: "Design capacity"; value: shade.amount(shade.battery.design_mah, "mAh") }
                    DetailRow { Layout.fillWidth: true; label: "Net current"; value: shade.amount(shade.battery.current_ma, "mA") }
                    DetailRow { Layout.fillWidth: true; label: "Voltage"; value: shade.amount(shade.battery.voltage_v, "V", 3) }
                    DetailRow { Layout.fillWidth: true; label: "Temperature"; value: shade.amount(shade.battery.temperature_c, "°C", 1) }
                    Repeater {
                        model: shade.battery.chargers || []
                        ColumnLayout {
                            required property var modelData
                            Layout.fillWidth: true; spacing: 15
                            DetailRow { Layout.fillWidth: true; label: "Charger"; value: modelData.status }
                            DetailRow { Layout.fillWidth: true; label: "USB input limit"; value: shade.amount(modelData.input_limit_ma, "mA") }
                        }
                    }
                    Text { font.family: MobileTheme.fontFamily; Layout.fillWidth: true; text: "Net current is what enters (+) or leaves (−) the battery. The USB input limit is a configured ceiling, not measured charge current. Battery and charger report their states independently."; wrapMode: Text.WordWrap; color: MobileTheme.secondary; font.pixelSize: 12 }
                }
                ColumnLayout {
                    visible: shade.detail === "wifi"; Layout.fillWidth: true; spacing: 12
                    Text { font.family: MobileTheme.fontFamily; Layout.fillWidth: true; text: shade.wifi.connected ? shade.wifi.ssid || shade.wifi.connection : "Not connected"; textFormat: Text.PlainText; color: MobileTheme.accent; font.pixelSize: 20; elide: Text.ElideRight }
                    DetailRow { Layout.fillWidth: true; label: "IPv4"; value: (shade.wifi.ipv4 || []).join("\n") || "—" }
                    DetailRow { Layout.fillWidth: true; label: "IPv6"; value: (shade.wifi.ipv6 || []).join("\n") || "—" }
                    DetailRow { Layout.fillWidth: true; label: "Gateway"; value: shade.wifi.gateway || "—" }
                    DetailRow { Layout.fillWidth: true; label: "DNS servers"; value: (shade.wifi.dns || []).join("\n") || "—" }
                    RowLayout {
                        Layout.fillWidth: true
                        TouchButton { Layout.fillWidth: true; label: "Refresh"; enabled: !networkProcess.running; onClicked: shade.networkAction("scan", {}) }
                        TouchButton { Layout.fillWidth: true; label: "Disconnect"; enabled: shade.wifi.connected === true && !networkProcess.running; onClicked: shade.networkAction("disconnect", {}) }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        TouchButton { Layout.fillWidth: true; label: "Speed test"; enabled: shade.wifi.connected === true; onClicked: shade.showDetail("speedtest") }
                        TouchButton { Layout.fillWidth: true; label: "Advanced"; onClicked: shade.settingsRequested("wifi") }
                    }
                    Text { font.family: MobileTheme.fontFamily; Layout.fillWidth: true; visible: shade.networkMessage.length > 0; text: shade.networkMessage; wrapMode: Text.WordWrap; color: MobileTheme.secondary; font.pixelSize: 13 }
                    ColumnLayout {
                        visible: shade.selectedNetwork !== null; Layout.fillWidth: true; spacing: 10
                        Text { font.family: MobileTheme.fontFamily; text: shade.selectedNetwork ? shade.selectedNetwork.ssid : ""; textFormat: Text.PlainText; color: MobileTheme.foreground; font.bold: true; Layout.fillWidth: true; elide: Text.ElideRight }
                        TouchTextField { font.family: MobileTheme.fontFamily;
                            id: password; Layout.fillWidth: true; implicitHeight: 50
                            placeholderText: "Password (blank uses a saved connection)"; echoMode: TextInput.Password
                            color: MobileTheme.foreground; placeholderTextColor: MobileTheme.secondary
                            background: Rectangle { radius: MobileTheme.radius(10); color: MobileTheme.surface; border.color: MobileTheme.muted }
                            editing: shade.editingField === "password" && shade.detail === "wifi"
                            onEditingRequested: shade.editField("password", password)
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            TouchButton { Layout.fillWidth: true; label: "Cancel"; onClicked: { shade.selectedNetwork = null; password.text = ""; if (shade.detail === "wifi") shade.releaseKeyboard(); } }
                            TouchButton { Layout.fillWidth: true; label: "Connect"; enabled: !networkProcess.running; selected: true; onClicked: { shade.networkAction("connect", {bssid: shade.selectedNetwork.bssid, password: password.text}); password.text = ""; shade.releaseKeyboard(); } }
                        }
                    }
                    Repeater {
                        model: shade.selectedNetwork ? [] : shade.networks
                        TouchButton {
                            required property var modelData
                            Layout.fillWidth: true; implicitHeight: 62; textSize: 14
                            label: (modelData.active ? "✓  " : "") + modelData.ssid + "   ·   " + modelData.signal + "%  " + modelData.security
                            selected: modelData.active; enabled: !networkProcess.running
                            onClicked: {
                                if (/802\.1X|EAP|WEP/.test(modelData.security)) { shade.networkMessage = "This security type needs an advanced NetworkManager profile."; return; }
                                // Clear first: selecting empties the model, which destroys this delegate.
                                password.text = ""; shade.selectedNetwork = modelData;
                            }
                        }
                    }
                }
                ColumnLayout {
                    visible: shade.detail === "speedtest"; Layout.fillWidth: true; spacing: 12
                    Kit.SpeedTestView { Layout.fillWidth: true }
                }
                ColumnLayout {
                    visible: shade.detail === "calendar"; Layout.fillWidth: true; spacing: 16
                    RowLayout {
                        Layout.fillWidth: true
                        TouchButton { implicitWidth: 44; implicitHeight: 44; label: "‹"; onClicked: shade.month = new Date(shade.month.getFullYear(), shade.month.getMonth() - 1, 1) }
                        Text { font.family: MobileTheme.fontFamily; Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; text: Qt.formatDateTime(shade.month, "MMMM yyyy"); color: MobileTheme.accent; font.pixelSize: 18 }
                        TouchButton { implicitWidth: 44; implicitHeight: 44; label: "›"; onClicked: shade.month = new Date(shade.month.getFullYear(), shade.month.getMonth() + 1, 1) }
                    }
                    GridLayout {
                        Layout.fillWidth: true; columns: 7; columnSpacing: 3; rowSpacing: 7
                        Repeater {
                            model: ["S", "M", "T", "W", "T", "F", "S"]
                            Text { font.family: MobileTheme.fontFamily; required property string modelData; text: modelData; Layout.fillWidth: true; Layout.preferredWidth: 1; horizontalAlignment: Text.AlignHCenter; color: MobileTheme.secondary; font.pixelSize: 13 }
                        }
                        Repeater {
                            model: 42
                            Rectangle {
                                required property int index
                                readonly property date day: new Date(shade.month.getFullYear(), shade.month.getMonth(), index - new Date(shade.month.getFullYear(), shade.month.getMonth(), 1).getDay() + 1)
                                readonly property bool today: Qt.formatDateTime(day, "yyyy-MM-dd") === Qt.formatDateTime(clock.date, "yyyy-MM-dd")
                                Layout.fillWidth: true; Layout.preferredWidth: 1; implicitHeight: 39; radius: MobileTheme.radius(12)
                                color: today ? MobileTheme.accent : "transparent"
                                Text { font.family: MobileTheme.fontFamily; anchors.centerIn: parent; text: parent.day.getDate(); color: parent.today ? MobileTheme.background : MobileTheme.foreground; opacity: parent.day.getMonth() === shade.month.getMonth() ? 1 : 0.3; font.pixelSize: 15 }
                                // A day opens the Calendar app on it.
                                TapHandler { onTapped: shade.openCalendar(Qt.formatDateTime(parent.day, "yyyy-MM-dd")) }
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        TouchButton { Layout.fillWidth: true; label: "Today"; onClicked: shade.month = new Date(clock.date.getFullYear(), clock.date.getMonth(), 1) }
                        TouchButton { Layout.fillWidth: true; label: "Open Calendar"; onClicked: shade.openCalendar(Qt.formatDateTime(clock.date, "yyyy-MM-dd")) }
                    }
                }
                ColumnLayout {
                    visible: shade.detail === "stats"; Layout.fillWidth: true; spacing: 14
                    RowLayout {
                        Layout.fillWidth: true; spacing: 12
                        Rectangle {
                            Layout.fillWidth: true; implicitHeight: 92; radius: MobileTheme.radius(16); color: MobileTheme.surface
                            Column {
                                anchors.centerIn: parent; spacing: 4
                                Text { anchors.horizontalCenter: parent.horizontalCenter; text: shade.amount(shade.stats.cpu, "%", 1); color: MobileTheme.accent; font.family: MobileTheme.fontFamily; font.pixelSize: 26; font.bold: true }
                                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "CPU"; color: MobileTheme.secondary; font.family: MobileTheme.fontFamily; font.pixelSize: 12 }
                            }
                        }
                        Rectangle {
                            Layout.fillWidth: true; implicitHeight: 92; radius: MobileTheme.radius(16); color: MobileTheme.surface
                            Column {
                                anchors.centerIn: parent; spacing: 4
                                Text { anchors.horizontalCenter: parent.horizontalCenter; text: shade.stats.memory && shade.stats.memory.percent !== undefined ? shade.amount(shade.stats.memory.percent, "%", 1) : "—"; color: MobileTheme.accent; font.family: MobileTheme.fontFamily; font.pixelSize: 26; font.bold: true }
                                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Memory"; color: MobileTheme.secondary; font.family: MobileTheme.fontFamily; font.pixelSize: 12 }
                            }
                        }
                    }
                    DetailRow { Layout.fillWidth: true; label: "Used / total"; value: shade.stats.memory ? shade.amount(shade.stats.memory.used_mb, "MB", 0) + " / " + shade.amount(shade.stats.memory.total_mb, "MB", 0) : "—" }
                    DetailRow { Layout.fillWidth: true; label: "Load"; value: shade.stats.load ? shade.stats.load.join("  ") : "—" }
                    DetailRow { Layout.fillWidth: true; label: "Cores"; value: shade.stats.core_count ? String(shade.stats.core_count) : "—" }
                    Row {
                        Layout.fillWidth: true; spacing: 4
                        Repeater {
                            model: shade.detail === "stats" ? (shade.stats.cores || []) : []
                            Rectangle {
                                required property var modelData
                                width: 18; height: 36; radius: 4; color: MobileTheme.muted
                                Rectangle {
                                    anchors.bottom: parent.bottom; width: parent.width; radius: 4
                                    height: parent.height * Math.max(0, Math.min(1, (modelData || 0) / 100))
                                    color: MobileTheme.accent
                                }
                            }
                        }
                    }
                    DetailRow { Layout.fillWidth: true; label: "Battery draw"; value: shade.amount(shade.battery.current_ma, "mA") + " · " + shade.amount(shade.battery.temperature_c, "°C", 1) }
                    Text { font.family: MobileTheme.fontFamily; text: "Temperatures"; color: MobileTheme.foreground; font.pixelSize: 16; font.bold: true }
                    Repeater {
                        model: shade.detail === "stats" ? (shade.stats.thermals || []) : []
                        DetailRow { required property var modelData; Layout.fillWidth: true; label: modelData.name; value: shade.amount(modelData.celsius, "°C", 1) }
                    }
                    Text { visible: !shade.stats.thermals || shade.stats.thermals.length === 0; font.family: MobileTheme.fontFamily; Layout.fillWidth: true; text: "No thermal zones exported."; color: MobileTheme.secondary; font.pixelSize: 13 }
                    Text { font.family: MobileTheme.fontFamily; text: "Using the most CPU"; color: MobileTheme.foreground; font.pixelSize: 16; font.bold: true }
                    Repeater {
                        model: shade.detail === "stats" ? (shade.stats.processes || []) : []
                        DetailRow { required property var modelData; Layout.fillWidth: true; label: modelData.name; value: shade.amount(modelData.cpu, "%", 1) + " · " + shade.amount(modelData.rss_mb, "MB", 0) }
                    }
                    Text { font.family: MobileTheme.fontFamily; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: MobileTheme.secondary; font.pixelSize: 12
                        text: "Process ranking is CPU time share, not measured milliwatts. Energy counters are not available on this path." }
                }
                ColumnLayout {
                    visible: shade.detail === "weather"; Layout.fillWidth: true; spacing: 12
                    Text {
                        font.family: MobileTheme.fontFamily; Layout.fillWidth: true
                        text: shade.weather.location ? shade.weather.location.name : "No location yet"
                        textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: MobileTheme.accent; font.pixelSize: 17
                        TapHandler { onTapped: shade.settingsRequested("weather") }
                    }
                    RowLayout {
                        visible: shade.weather.available === true
                        Layout.fillWidth: true; spacing: 14
                        Text { text: WeatherIcons.glyph(shade.weather.kind || ""); color: MobileTheme.accent; font.family: MobileTheme.fontFamily; font.pixelSize: 52 }
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 2
                            Text { font.family: MobileTheme.fontFamily; text: shade.weather.available ? shade.amount(shade.weather.current.temperature_2m, "°F") : ""; color: MobileTheme.foreground; font.pixelSize: 32; font.bold: true }
                            Text { font.family: MobileTheme.fontFamily; Layout.fillWidth: true; wrapMode: Text.WordWrap; text: shade.weather.condition || shade.weatherName(shade.weather.current && shade.weather.current.weather_code); color: MobileTheme.secondary; font.pixelSize: 15 }
                        }
                    }
                    // The next 24 hours; swipe sideways for the rest.
                    ListView {
                        id: hourly
                        visible: shade.weather.available === true && count > 0
                        Layout.fillWidth: true; implicitHeight: 104
                        orientation: ListView.Horizontal; spacing: 8; clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        model: shade.weather.available ? (shade.weather.hourly_forecast || []) : []
                        delegate: Rectangle {
                            required property var modelData
                            width: 64; height: hourly.height; radius: MobileTheme.radius(14); color: MobileTheme.surface
                            Column {
                                anchors.centerIn: parent; spacing: 4
                                Text { anchors.horizontalCenter: parent.horizontalCenter; text: modelData.label; color: MobileTheme.secondary; font.family: MobileTheme.fontFamily; font.pixelSize: 12 }
                                Text { anchors.horizontalCenter: parent.horizontalCenter; text: WeatherIcons.glyph(modelData.kind); color: MobileTheme.accent; font.family: MobileTheme.fontFamily; font.pixelSize: 24 }
                                Text { anchors.horizontalCenter: parent.horizontalCenter; text: modelData.temperature == null ? "—" : Math.round(modelData.temperature) + "°"; color: MobileTheme.foreground; font.family: MobileTheme.fontFamily; font.pixelSize: 15; font.bold: true }
                                Text { anchors.horizontalCenter: parent.horizontalCenter; visible: modelData.precipitation >= 10; text: modelData.precipitation + "%"; color: MobileTheme.secondary; font.family: MobileTheme.fontFamily; font.pixelSize: 11 }
                            }
                        }
                    }
                    DetailRow { visible: shade.weather.available === true; Layout.fillWidth: true; label: "Feels like"; value: shade.weather.available ? shade.amount(shade.weather.current.apparent_temperature, "°F") : "—" }
                    DetailRow { visible: shade.weather.available === true; Layout.fillWidth: true; label: "Humidity"; value: shade.weather.available ? shade.amount(shade.weather.current.relative_humidity_2m, "%") : "—" }
                    DetailRow { visible: shade.weather.available === true; Layout.fillWidth: true; label: "Wind"; value: shade.weather.available ? shade.amount(shade.weather.current.wind_speed_10m, "mph") : "—" }
                    Repeater {
                        model: shade.weather.available ? (shade.weather.daily_forecast || []) : []
                        Rectangle {
                            required property var modelData
                            Layout.fillWidth: true; implicitHeight: 62; radius: MobileTheme.radius(14); color: MobileTheme.surface
                            RowLayout {
                                anchors.fill: parent; anchors.margins: 12; spacing: 10
                                Text { text: WeatherIcons.glyph(modelData.kind); color: MobileTheme.accent; font.family: MobileTheme.fontFamily; font.pixelSize: 26 }
                                ColumnLayout {
                                    Layout.fillWidth: true; spacing: 1
                                    Text { font.family: MobileTheme.fontFamily; text: modelData.label; color: MobileTheme.foreground; font.pixelSize: 15; font.bold: true }
                                    Text { font.family: MobileTheme.fontFamily; text: modelData.weekday + " · " + modelData.condition; color: MobileTheme.secondary; font.pixelSize: 12 }
                                }
                                Text { font.family: MobileTheme.fontFamily; text: (modelData.min == null ? "—" : Math.round(modelData.min) + "°") + " / " + (modelData.max == null ? "—" : Math.round(modelData.max) + "°"); color: MobileTheme.foreground; font.pixelSize: 15 }
                            }
                        }
                    }
                    Text { font.family: MobileTheme.fontFamily; Layout.fillWidth: true; text: shade.weatherMessage; visible: text.length > 0; wrapMode: Text.WordWrap; color: MobileTheme.secondary; font.pixelSize: 13 }
                    TouchButton { visible: shade.weather.configured !== true; Layout.fillWidth: true; label: "Choose a location"; onClicked: shade.settingsRequested("weather") }
                    Text { font.family: MobileTheme.fontFamily; Layout.fillWidth: true; text: "Weather data: Open-Meteo · CC BY 4.0 · updates at most every 15 minutes\nChange the place in Settings → Weather"; wrapMode: Text.WordWrap; color: MobileTheme.secondary; font.pixelSize: 11 }
                }
                ColumnLayout {
                    visible: shade.detail === "clipboard"; Layout.fillWidth: true; spacing: 12
                    Text { font.family: MobileTheme.fontFamily; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: MobileTheme.secondary; font.pixelSize: 13
                        text: "History is stored on this phone. Desktop sync is opt-in and not enabled yet." }
                    RowLayout {
                        Layout.fillWidth: true
                        TouchButton { Layout.fillWidth: true; label: "Refresh"; enabled: !clipProcess.running; onClicked: shade.clipboardAction("history", {}) }
                        TouchButton { Layout.fillWidth: true; label: "Clear"; enabled: !clipProcess.running; onClicked: shade.clipboardAction("clear", {}) }
                    }
                    Text { font.family: MobileTheme.fontFamily; Layout.fillWidth: true; visible: shade.clipMessage.length > 0; text: shade.clipMessage; wrapMode: Text.WordWrap; color: MobileTheme.secondary; font.pixelSize: 13 }
                    Text { font.family: MobileTheme.fontFamily; Layout.fillWidth: true; visible: shade.clips.length === 0; text: "Nothing copied yet."; color: MobileTheme.secondary; font.pixelSize: 15 }
                    Repeater {
                        model: shade.clips
                        Rectangle {
                            required property var modelData
                            Layout.fillWidth: true; implicitHeight: clipCard.implicitHeight + 24; radius: MobileTheme.radius(14); color: MobileTheme.surface
                            ColumnLayout {
                                id: clipCard; anchors { left: parent.left; right: parent.right; top: parent.top; margins: 12 } spacing: 8
                                Text { font.family: MobileTheme.fontFamily; Layout.fillWidth: true; text: modelData.preview; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: MobileTheme.foreground; font.pixelSize: 15 }
                                RowLayout {
                                    Layout.fillWidth: true
                                    TouchButton { Layout.fillWidth: true; implicitHeight: 42; textSize: 13; label: "Copy"; onClicked: shade.clipboardAction("copy", {id: modelData.id}) }
                                    TouchButton { Layout.fillWidth: true; implicitHeight: 42; textSize: 13; label: "Paste"; onClicked: shade.clipboardAction("paste", {id: modelData.id}) }
                                    TouchButton { Layout.fillWidth: true; implicitHeight: 42; textSize: 13; label: modelData.pinned ? "Unpin" : "Pin"; onClicked: shade.clipboardAction("pin", {id: modelData.id}) }
                                    TouchButton { implicitWidth: 42; implicitHeight: 42; label: "×"; onClicked: shade.clipboardAction("delete", {id: modelData.id}) }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    component GhostButton: Rectangle {
        id: ghost
        property string glyph: ""
        property int size: 36
        signal clicked()
        implicitWidth: size
        implicitHeight: size
        radius: MobileTheme.radius(size / 2)
        color: ghostTap.pressed ? MobileTheme.selection : "transparent"
        Text { anchors.centerIn: parent; text: ghost.glyph; color: MobileTheme.secondary; font.family: MobileTheme.fontFamily; font.pixelSize: ghost.size * 0.5 }
        TapHandler { id: ghostTap; onTapped: ghost.clicked() }
    }
}
