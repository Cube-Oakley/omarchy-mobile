import QtQuick
import Quickshell
import Quickshell.Io

// Where the shade's weather is for (omarchy-mobile-weather, Open-Meteo):
// search by city or postal code and pick a match.
Flickable {
    id: page
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    contentHeight: body.height
    property var weather: ({configured: false})
    property var locations: []
    property string message: ""
    property string input: ""
    // The location field is being typed in: Settings keeps the keyboard up.
    property bool editing: false
    function run(action, values) {
        if (request.running) return;
        input = JSON.stringify(values || {}) + "\n";
        request.command = [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-weather", action];
        if (action !== "status") message = action === "search" ? "Searching…" : "Saving…";
        request.running = true;
    }
    function find() {
        editing = false;
        run("search", {query: field.text});
    }
    // Settings may open straight onto this page, visible from the start.
    Component.onCompleted: if (visible) run("status", {})
    onVisibleChanged: {
        if (visible) run("status", {});
        else editing = false;
    }
    onEditingChanged: {
        keyboard.command = [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-keyboard", editing ? "show" : "hide"];
        if (!keyboard.running) keyboard.running = true;
    }
    Process { id: keyboard }
    Process {
        id: request
        stdinEnabled: true
        stdout: StdioCollector {}
        onStarted: { write(page.input); page.input = ""; stdinEnabled = false; }
        onExited: {
            stdinEnabled = true;
            try {
                const result = JSON.parse(stdout.text);
                page.message = result.error || "";
                if (result.locations) {
                    page.locations = result.locations;
                    if (!result.locations.length) page.message = result.error || "No matching locations";
                } else {
                    page.weather = result;
                    page.locations = [];
                }
            } catch (e) { page.message = "Weather unavailable"; }
        }
    }
    Column {
        id: body
        width: page.width
        spacing: 12
        DetailRow {
            width: parent.width
            label: "Location"
            value: page.weather.location ? page.weather.location.name : "Not set"
        }
        TouchTextField {
            id: field
            width: parent.width
            placeholderText: "City or postal code"
            editing: page.editing
            onEditingRequested: page.editing = true
            onAccepted: page.find()
        }
        TouchButton {
            width: parent.width
            label: "Find location"
            enabled: !request.running && field.text.trim().length >= 2
            onClicked: page.find()
        }
        Text {
            width: parent.width
            visible: text.length > 0
            text: page.message
            wrapMode: Text.WordWrap
            color: MobileTheme.secondary
            font.family: MobileTheme.fontFamily
            font.pixelSize: 13
        }
        Repeater {
            model: page.locations
            TouchButton {
                required property var modelData
                width: body.width
                label: modelData.name
                textSize: 13
                onClicked: page.run("set", modelData)
            }
        }
        Text {
            width: parent.width
            topPadding: 8
            wrapMode: Text.WordWrap
            text: "The shade shows the weather for this place; the phone's own position is not used. Data from Open-Meteo (CC BY 4.0), updated at most every 15 minutes."
            color: MobileTheme.secondary
            font.family: MobileTheme.fontFamily
            font.pixelSize: 12
        }
    }
}
