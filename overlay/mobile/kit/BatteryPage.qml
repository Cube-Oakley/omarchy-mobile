import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Io

Flickable {
    id: page
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    contentHeight: detailsOpen ? detailsBody.height : body.height
    // The Details view (omarchy-mobile-battery details): use by component,
    // process, wake source, interrupt and network over the window.
    property bool detailsOpen: false
    property var details: null
    property int detailsHours: 24
    onDetailsOpenChanged: { contentY = 0; if (detailsOpen) loadDetails(); }
    function loadDetails() {
        detailsProbe.command = [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-battery", "details", String(page.detailsHours)];
        if (!detailsProbe.running) detailsProbe.running = true;
    }
    function bytes(n) {
        if (n >= 1e9) return (n / 1e9).toFixed(2) + " GB";
        if (n >= 1e6) return (n / 1e6).toFixed(1) + " MB";
        return Math.round(n / 1e3) + " kB";
    }
    property var battery: ({available: false})
    function amount(value, unit, decimals) {
        return value === null || value === undefined ? "—" : Number(value).toFixed(decimals || 0) + " " + unit;
    }
    // Use over the last day (omarchy-mobile-battery usage): the level, the
    // time and charge in each screen state, deep sleep and the wakes.
    property var usage: null
    function duration(seconds) {
        const minutes = Math.round((seconds || 0) / 60);
        if (minutes < 60) return minutes + " min";
        return Math.floor(minutes / 60) + " h " + (minutes % 60) + " min";
    }
    function stateText(state) {
        if (!state || !state.seconds) return "—";
        const ma = state.mah && state.seconds > 300 ? " (" + Math.round(state.mah / (state.seconds / 3600)) + " mA)" : "";
        return page.duration(state.seconds) + " · " + Math.round(state.mah) + " mAh" + ma;
    }
    readonly property var wakeLabels: ({"power-key": "power key", "modem": "calls and texts", "alarm": "clock and checks",
                                        "touch": "touch", "clock": "alarms and reminders", "network": "network",
                                        "bluetooth": "Bluetooth", "unknown": "other"})
    function wakesText(u) {
        if (!u) return "—";
        const parts = Object.keys(u.wakes || {}).sort((a, b) => u.wakes[b] - u.wakes[a])
            .map(k => u.wakes[k] + " " + (page.wakeLabels[k] || k));
        if (u.minute_ticks) parts.push(u.minute_ticks + " clock redraws");
        return parts.length ? parts.join(", ") : "None";
    }
    function refresh() {
        if (!usageProbe.running) usageProbe.running = true;
        if (!probe.running) probe.running = true;
        if (!prefsLoad.running) prefsLoad.running = true;
        if (!sleepProbe.running) sleepProbe.running = true;
    }
    // Sleep while the screen is dark (prefs sleep), with a background wake
    // every sleepCheck minutes (0: only calls, texts and the power key).
    property bool sleepOn: true
    property int sleepCheck: 15
    // The background check's network (prefs sleepCheckNetwork): "wifi"
    // brings Wi-Fi up; "data" uses mobile data alone, a shorter wake.
    property string sleepCheckNetwork: "wifi"
    readonly property var checkNetworks: [{value: "wifi", label: "Wi-Fi"}, {value: "data", label: "Mobile data"}]
    function networkName(value) { return value === "data" ? "Mobile data" : "Wi-Fi"; }
    property bool alwaysOn: false
    property var lastSleep: null
    readonly property var checks: [5, 15, 30, 60, 0]
    function checkName(minutes) { return minutes ? "Every " + minutes + " min" : "Never"; }
    function applyPrefs(text) {
        try {
            const prefs = JSON.parse(text);
            if (prefs.error) return;
            page.sleepOn = prefs.sleep !== false;
            page.alwaysOn = prefs.alwaysOn === true;
            if (typeof prefs.sleepCheck === "number") page.sleepCheck = prefs.sleepCheck;
            if (prefs.sleepCheckNetwork === "wifi" || prefs.sleepCheckNetwork === "data")
                page.sleepCheckNetwork = prefs.sleepCheckNetwork;
        } catch (e) {}
    }
    function savePrefs(change) {
        prefsSave.payload = JSON.stringify(change) + "\n";
        prefsSave.running = true;
    }
    readonly property var wakeNames: ({"power-key": "the power key", "modem": "a call or text",
                                      "network": "mobile data", "alarm": "the background check",
                                      "touch": "a touch", "bluetooth": "a Bluetooth device"})
    function sleepSummary(entry) {
        if (!entry || entry.slept === undefined) return "Not yet";
        const minutes = Math.round(entry.slept / 60);
        const length = entry.slept < 60 ? Math.round(entry.slept) + " s" : minutes + " min";
        return length + ", woken by " + (page.wakeNames[entry.woke] || entry.woke);
    }
    function setLimit(percent) {
        limiter.command = [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-battery", "limit", String(percent)];
        limiter.running = true;
    }
    Process {
        id: limiter
        onExited: page.refresh()
    }
    Component.onCompleted: refresh()
    Process {
        id: detailsProbe
        stdout: StdioCollector {}
        onExited: {
            try { const d = JSON.parse(stdout.text); if (d.ok) page.details = d; } catch (e) {}
        }
    }
    Process {
        id: usageProbe
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-battery", "usage", "24"]
        stdout: StdioCollector {}
        onExited: {
            try { const u = JSON.parse(stdout.text); if (u.ok) page.usage = u; } catch (e) {}
        }
    }
    Process {
        id: probe
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-battery"]
        running: true
        stdout: StdioCollector {}
        onExited: {
            try { page.battery = JSON.parse(stdout.text); }
            catch (e) { page.battery = {available: false}; }
        }
    }
    Timer { interval: 5000; running: page.visible; repeat: true; onTriggered: page.refresh() }
    Process {
        id: prefsLoad
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-prefs"]
        stdout: StdioCollector {}
        onExited: page.applyPrefs(stdout.text)
    }
    Process {
        id: prefsSave
        stdinEnabled: true
        property string payload: ""
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-prefs", "set"]
        stdout: StdioCollector {}
        onStarted: { write(prefsSave.payload); prefsSave.payload = ""; stdinEnabled = false; }
        onExited: { stdinEnabled = true; page.applyPrefs(stdout.text); }
    }
    Process {
        id: sleepProbe
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-sleep", "status"]
        stdout: StdioCollector {}
        onExited: {
            try {
                const recent = (JSON.parse(stdout.text).recent || []).filter(entry => entry.slept !== undefined);
                page.lastSleep = recent.length ? recent[recent.length - 1] : null;
            } catch (e) { page.lastSleep = null; }
        }
    }
    Column {
        id: body
        width: page.width
        spacing: 12
        visible: !page.detailsOpen
        Text {
            width: parent.width
            text: page.amount(page.battery.capacity, "%")
            color: MobileTheme.accent
            font.family: MobileTheme.fontFamily
            font.pixelSize: 48
            font.bold: true
        }
        DetailRow { width: parent.width; label: "Battery status"; value: page.battery.status || "Unavailable" }
        DetailRow { width: parent.width; label: "Stored charge"; value: page.amount(page.battery.charge_mah, "mAh") }
        DetailRow { width: parent.width; label: "Reported full"; value: page.amount(page.battery.full_mah, "mAh") }
        DetailRow { width: parent.width; label: "Design capacity"; value: page.amount(page.battery.design_mah, "mAh") }
        DetailRow { width: parent.width; label: "Net current"; value: page.amount(page.battery.current_ma, "mA") }
        DetailRow { width: parent.width; label: "Voltage"; value: page.amount(page.battery.voltage_v, "V", 3) }
        DetailRow { width: parent.width; label: "Temperature"; value: page.amount(page.battery.temperature_c, "°C", 1) }
        Text {
            width: parent.width
            topPadding: 12
            text: "USE · LAST 24 HOURS"
            color: MobileTheme.accent
            font.family: MobileTheme.fontFamily
            font.pixelSize: 12
            font.letterSpacing: 2
            font.bold: true
        }
        // The level over the day: on battery in the accent, charging in green.
        Rectangle {
            width: parent.width
            height: 150
            radius: MobileTheme.radius(14)
            color: MobileTheme.surface
            Canvas {
                id: levelGraph
                anchors { fill: parent; margins: 12; bottomMargin: 24 }
                property var points: page.usage ? page.usage.points : []
                onPointsChanged: requestPaint()
                onWidthChanged: requestPaint()
                onPaint: {
                    const ctx = getContext("2d");
                    ctx.reset();
                    ctx.strokeStyle = MobileTheme.muted;
                    ctx.lineWidth = 1;
                    for (const level of [0, 50, 100]) {
                        const y = height - level / 100 * height;
                        ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(width, y); ctx.stroke();
                    }
                    if (!page.usage || points.length < 2) return;
                    const start = page.usage.start, span = page.usage.now - start;
                    ctx.lineWidth = 3;
                    for (let i = 1; i < points.length; i++) {
                        const a = points[i - 1], b = points[i];
                        if (b.time - a.time > 6 * 3600) continue;
                        ctx.strokeStyle = a.plugged ? MobileTheme.success : MobileTheme.accent;
                        ctx.beginPath();
                        ctx.moveTo((a.time - start) / span * width, height - a.capacity / 100 * height);
                        ctx.lineTo((b.time - start) / span * width, height - b.capacity / 100 * height);
                        ctx.stroke();
                    }
                }
            }
            Text {
                anchors { left: parent.left; bottom: parent.bottom; margins: 8; leftMargin: 12 }
                text: "24 h ago"
                color: MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 11
            }
            Text {
                anchors { right: parent.right; bottom: parent.bottom; margins: 8; rightMargin: 12 }
                text: "now"
                color: MobileTheme.secondary
                font.family: MobileTheme.fontFamily
                font.pixelSize: 11
            }
        }
        DetailRow {
            width: parent.width
            label: "Since unplugged"
            value: page.usage && page.usage.since_unplugged
                   ? page.duration(page.usage.since_unplugged.seconds) + " · " + page.usage.since_unplugged.percent + " %"
                     + (page.usage.since_unplugged.average_ma ? " · " + Math.round(page.usage.since_unplugged.average_ma) + " mA" : "")
                   : "Plugged in"
        }
        DetailRow { width: parent.width; label: "Screen on"; value: page.stateText(page.usage ? page.usage.states.on : null) }
        DetailRow { width: parent.width; label: "Always-on display"; value: page.stateText(page.usage ? page.usage.states.ambient : null) }
        DetailRow { width: parent.width; label: "Screen off"; value: page.stateText(page.usage ? page.usage.states.off : null) }
        DetailRow {
            width: parent.width
            label: "Deep sleep"
            value: page.usage && page.usage.deep.seconds
                   ? page.duration(page.usage.deep.seconds) + (page.usage.deep.average_ma ? " · " + Math.round(page.usage.deep.average_ma) + " mA" : "")
                   : "—"
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: "Woken by: " + page.wakesText(page.usage)
            color: MobileTheme.secondary
            font.family: MobileTheme.fontFamily
            font.pixelSize: 13
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: "The battery's gauge measures the whole phone, so use here is by what the screen was doing. Deep sleep is part of the screen-off and always-on time. Details break it down by component, process and wake."
            color: MobileTheme.secondary
            font.family: MobileTheme.fontFamily
            font.pixelSize: 12
        }
        SettingsRow {
            width: parent.width
            label: "Details"
            value: "By component"
            onClicked: page.detailsOpen = true
        }
        Repeater {
            model: page.battery.chargers || []
            Column {
                required property var modelData
                width: body.width
                spacing: 12
                DetailRow { width: parent.width; label: "Charger"; value: modelData.status }
                DetailRow { width: parent.width; label: "USB input limit"; value: page.amount(modelData.input_limit_ma, "mA") }
            }
        }
        SettingsRow {
            width: parent.width
            visible: page.battery.limit_supported === true
            label: "Limit charging to 80%"
            value: page.battery.limit < 100 ? "On" : "Off"
            selected: page.battery.limit < 100
            enabled: !limiter.running
            onClicked: page.setLimit(page.battery.limit < 100 ? 100 : 80)
        }
        Text {
            width: parent.width
            visible: page.battery.limit_supported === true
            wrapMode: Text.WordWrap
            text: "Keeping a lithium battery below full slows its wear. With the limit on, charging stops at 80% and resumes at 75%."
            color: MobileTheme.secondary
            font.family: MobileTheme.fontFamily
            font.pixelSize: 12
        }
        SettingsRow {
            width: parent.width
            label: "Sleep when the screen is off"
            value: page.sleepOn ? "On" : "Off"
            selected: page.sleepOn
            enabled: !prefsSave.running
            onClicked: page.savePrefs({sleep: !page.sleepOn})
        }
        SettingsRow {
            width: parent.width
            visible: page.sleepOn
            label: "Check in the background"
            value: page.checkName(page.sleepCheck)
            enabled: !prefsSave.running
            onClicked: checkPicker.open = true
        }
        SettingsRow {
            width: parent.width
            visible: page.sleepOn && page.sleepCheck > 0
            label: "Background check uses"
            value: page.networkName(page.sleepCheckNetwork)
            enabled: !prefsSave.running
            onClicked: networkPicker.open = true
        }
        Text {
            width: parent.width
            visible: page.sleepOn && page.sleepCheck > 0
            wrapMode: Text.WordWrap
            // omarchy-mobile-sleep check_minutes().
            text: "The check runs only while something uses it: an app that fetches in the background, or the weather under the always-on clock."
            color: MobileTheme.secondary
            font.family: MobileTheme.fontFamily
            font.pixelSize: 12
        }
        DetailRow { width: parent.width; visible: page.sleepOn; label: "Last sleep"; value: page.sleepSummary(page.lastSleep) }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            // A panel that keeps its picture through sleep (device ambientSleep)
            // lets the phone sleep under the clock, woken a second a minute.
            text: page.alwaysOn && (MobileTheme.state.device || {}).ambientSleep === true
                  ? "Under the always-on display the phone sleeps as with the screen off: the panel keeps showing the clock, and the phone wakes for about a second each minute to draw the next one. A long press on the fingerprint mark wakes it to unlock."
                  : page.alwaysOn
                  ? "The always-on display keeps the screen lit, so the phone does not sleep while it shows. Turn it off in Display to let the phone sleep."
                  : "A few seconds after the screen goes off the phone sleeps, except during a call, while audio plays or on the charger. Calls, texts and the power button wake it; the background check wakes it briefly to fetch new mail and messages, over Wi-Fi or, for a shorter wake that uses data, mobile data."
            color: MobileTheme.secondary
            font.family: MobileTheme.fontFamily
            font.pixelSize: 12
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: "Net current is what enters (+) or leaves (−) the battery. The USB input limit is a configured ceiling, not measured charge current."
            color: MobileTheme.secondary
            font.family: MobileTheme.fontFamily
            font.pixelSize: 12
        }
    }
    // Details.
    Column {
        id: detailsBody
        width: page.width
        spacing: 10
        visible: page.detailsOpen
        readonly property var d: page.details
        readonly property real most: d && d.components.length ? Math.max(0.01, d.components[0].mwh) : 1
        Row {
            spacing: 8
            Repeater {
                model: [6, 24, 72]
                TouchButton {
                    required property int modelData
                    width: 72; implicitHeight: 40
                    label: modelData < 48 ? modelData + " h" : modelData / 24 + " days"
                    textSize: 13
                    selected: page.detailsHours === modelData
                    onClicked: { page.detailsHours = modelData; page.loadDetails(); }
                }
            }
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: !detailsBody.d ? "Reading…"
                : detailsBody.d.metered_seconds
                  ? "The phone's power meter covered " + page.duration(detailsBody.d.metered_seconds) + " of the last "
                    + page.detailsHours + " h: " + detailsBody.d.metered_mwh.toFixed(1) + " mWh by component."
                  : "No power meter readings in this window yet: they start once the meter is running (the battery history records them as they come)."
            color: MobileTheme.secondary
            font.family: MobileTheme.fontFamily
            font.pixelSize: 13
        }
        Repeater {
            model: detailsBody.d ? detailsBody.d.components : []
            Column {
                id: part
                required property var modelData
                width: detailsBody.width
                spacing: 4
                Row {
                    width: parent.width
                    Text {
                        width: parent.width * 0.6
                        text: part.modelData.name + (part.modelData.floor ? " *" : "")
                        color: MobileTheme.foreground
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 15
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width * 0.4
                        horizontalAlignment: Text.AlignRight
                        text: part.modelData.mwh.toFixed(1) + " mWh" + (part.modelData.average_mw !== null ? " · " + part.modelData.average_mw.toFixed(0) + " mW" : "")
                        color: MobileTheme.secondary
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 13
                    }
                }
                Rectangle {
                    width: parent.width
                    height: 6
                    radius: 3
                    color: MobileTheme.surface
                    Rectangle {
                        width: parent.width * Math.min(1, part.modelData.mwh / detailsBody.most)
                        height: parent.height
                        radius: 3
                        color: MobileTheme.accent
                    }
                }
                Text {
                    width: parent.width
                    text: "on " + part.modelData.by_screen.on.toFixed(1) + " · always-on " + part.modelData.by_screen.ambient.toFixed(1)
                          + " · off " + part.modelData.by_screen.off.toFixed(1) + " mWh"
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 11
                }
            }
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            visible: !!(detailsBody.d && detailsBody.d.metered_seconds)
            text: {
                const d = detailsBody.d;
                if (!d) return "";
                const battery = d.battery_mwh.on + d.battery_mwh.ambient + d.battery_mwh.off;
                return "On battery the gauge counted " + battery.toFixed(0) + " mWh. The meter sees about half of the phone: the rest is power conversion and parts it does not cover. * These rails read 40–60 mW high while their part is off."
            }
            color: MobileTheme.secondary
            font.family: MobileTheme.fontFamily
            font.pixelSize: 12
        }
        Section { title: "CPU time" }
        Repeater {
            model: detailsBody.d ? detailsBody.d.cpu : []
            DetailRow { required property var modelData; width: detailsBody.width; label: modelData[0]; value: modelData[1].toFixed(1) + " s" }
        }
        Section { title: "Wake sources" }
        Repeater {
            model: detailsBody.d ? detailsBody.d.wakeups : []
            DetailRow {
                required property var modelData
                width: detailsBody.width
                label: modelData[0]
                value: modelData[1] + (modelData[1] === 1 ? " wake" : " wakes") + " · " + modelData[2].toFixed(1) + " s held"
            }
        }
        Section { title: "Interrupts" }
        Repeater {
            model: detailsBody.d ? detailsBody.d.irqs : []
            DetailRow { required property var modelData; width: detailsBody.width; label: modelData[0]; value: String(modelData[1]) }
        }
        Section { title: "Network" }
        Repeater {
            model: detailsBody.d ? Object.keys(detailsBody.d.net) : []
            DetailRow {
                required property string modelData
                width: detailsBody.width
                label: modelData
                value: "↓ " + page.bytes(detailsBody.d.net[modelData][0]) + "  ↑ " + page.bytes(detailsBody.d.net[modelData][1])
            }
        }
        DetailRow {
            width: detailsBody.width
            label: "Radios loaded"
            value: detailsBody.d ? "Wi-Fi " + page.duration(detailsBody.d.radio_seconds.wifi) + " · Bluetooth " + page.duration(detailsBody.d.radio_seconds.bluetooth) : "—"
        }
        Item { width: 1; height: 24 }
    }

    component Section: Text {
        property string title: ""
        width: detailsBody.width
        topPadding: 14
        text: title.toUpperCase()
        color: MobileTheme.accent
        font.family: MobileTheme.fontFamily
        font.pixelSize: 12
        font.letterSpacing: 2
        font.bold: true
    }

    ChoicePicker {
        id: checkPicker
        parent: page
        title: "Check in the background"
        current: page.sleepCheck
        options: page.checks.map(minutes => ({value: minutes, label: page.checkName(minutes)}))
        onChosen: value => { page.sleepCheck = value; page.savePrefs({sleepCheck: value}); }
    }
    ChoicePicker {
        id: networkPicker
        parent: page
        title: "Background check uses"
        current: page.sleepCheckNetwork
        options: page.checkNetworks
        onChosen: value => { page.sleepCheckNetwork = value; page.savePrefs({sleepCheckNetwork: value}); }
    }
}
