import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Io

Flickable {
    id: page
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    contentHeight: body.height
    property var battery: ({available: false})
    function amount(value, unit, decimals) {
        return value === null || value === undefined ? "—" : Number(value).toFixed(decimals || 0) + " " + unit;
    }
    function refresh() {
        if (!probe.running) probe.running = true;
        if (!prefsLoad.running) prefsLoad.running = true;
        if (!sleepProbe.running) sleepProbe.running = true;
    }
    // Sleep while the screen is dark (prefs sleep), with a background wake
    // every sleepCheck minutes (0: only calls, texts and the power key).
    property bool sleepOn: true
    property int sleepCheck: 15
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
        } catch (e) {}
    }
    function savePrefs(change) {
        prefsSave.payload = JSON.stringify(change) + "\n";
        prefsSave.running = true;
    }
    readonly property var wakeNames: ({"power-key": "the power key", "modem": "a call or text",
                                      "network": "mobile data", "alarm": "the background check"})
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
        DetailRow { width: parent.width; visible: page.sleepOn; label: "Last sleep"; value: page.sleepSummary(page.lastSleep) }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: page.alwaysOn
                  ? "The always-on display keeps the screen lit, so the phone does not sleep while it shows. Turn it off in Display to let the phone sleep."
                  : "A few seconds after the screen goes off the phone sleeps, except during a call, while audio plays or on the charger. Calls, texts and the power button wake it; the background check wakes it briefly so Wi-Fi can reconnect."
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
    ChoicePicker {
        id: checkPicker
        parent: page
        title: "Check in the background"
        current: page.sleepCheck
        options: page.checks.map(minutes => ({value: minutes, label: page.checkName(minutes)}))
        onChosen: value => { page.sleepCheck = value; page.savePrefs({sleepCheck: value}); }
    }
}
