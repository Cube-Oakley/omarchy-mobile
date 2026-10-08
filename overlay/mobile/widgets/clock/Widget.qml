import QtQuick
import QtQuick.Layouts
import OmarchyMobile

// The time, the date and the weather, on the wallpaper itself. Its weather
// is the shell's (omarchy-mobile-weather, fetched for the controls and the
// always-on display too); being shown asks for it only when it is old.
Item {
    id: widget
    property var host: null
    property bool active: false
    property int spanX: 4
    property int spanY: 2

    property date now: new Date()
    readonly property var weather: host && host.weather ? host.weather : ({})
    readonly property bool hasWeather: weather.available === true && !!weather.current
    readonly property var today: hasWeather && weather.daily_forecast && weather.daily_forecast.length ? weather.daily_forecast[0] : null
    readonly property bool compact: spanY < 2
    readonly property bool narrow: spanX < 3
    readonly property real timeSize: compact ? Math.min(height * 0.62, 56) : narrow ? Math.min(width * 0.36, 64) : Math.min(height * 0.42, 84)

    function glyph(kind) {
        return {
            clear: "󰖙", mostly_clear: "󰖕", clear_night: "\u{F0594}", mostly_clear_night: "\u{F0F31}",
            cloudy: "󰖐", fog: "󰖑", drizzle: "󰖗", rain: "󰖖", snow: "󰖘", showers: "󰖒", thunder: "󰖓"
        }[kind] || "󰖐";
    }
    function degrees(value) { return value === undefined || value === null ? "—" : Math.round(value) + "°"; }

    // The host fetches only when its weather is old (half an hour).
    function wantWeather() { if (active && host) host.weatherWanted(); }
    onActiveChanged: if (active) { now = new Date(); wantWeather(); }
    onHostChanged: wantWeather()
    // On the minute, and only while this page is on screen.
    Timer {
        running: widget.active
        repeat: true
        interval: 60000 - (widget.now.getSeconds() * 1000 + widget.now.getMilliseconds()) + 40
        onTriggered: { widget.now = new Date(); widget.wantWeather(); }
    }

    GridLayout {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        columns: widget.narrow ? 1 : 2
        columnSpacing: 12
        rowSpacing: 0

        // The time and the date.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: widget.narrow ? Qt.AlignHCenter : Qt.AlignVCenter | Qt.AlignLeft
            spacing: widget.compact ? 0 : 2
            Text {
                Layout.alignment: widget.narrow ? Qt.AlignHCenter : Qt.AlignLeft
                text: Qt.formatTime(widget.now, "h:mm")
                color: MobileTheme.foreground
                font.family: MobileTheme.fontFamily
                font.pixelSize: widget.timeSize
                font.bold: true
                style: Text.Raised
                styleColor: Qt.rgba(0, 0, 0, 0.75)
                TapHandler { onTapped: if (widget.host) widget.host.open("clock") }
            }
            Text {
                Layout.alignment: widget.narrow ? Qt.AlignHCenter : Qt.AlignLeft
                text: Qt.formatDate(widget.now, widget.narrow || widget.compact ? "ddd, MMM d" : "dddd, MMMM d")
                color: MobileTheme.foreground
                opacity: 0.9
                font.family: MobileTheme.fontFamily
                font.pixelSize: widget.compact ? 13 : 17
                style: Text.Raised
                styleColor: Qt.rgba(0, 0, 0, 0.75)
            }
        }

        // The weather now, and today's high and low.
        ColumnLayout {
            visible: widget.hasWeather
            Layout.alignment: widget.narrow ? Qt.AlignHCenter : Qt.AlignVCenter | Qt.AlignRight
            spacing: 0
            RowLayout {
                Layout.alignment: widget.narrow ? Qt.AlignHCenter : Qt.AlignRight
                spacing: 8
                Text {
                    text: widget.glyph(widget.weather.kind || "")
                    color: MobileTheme.accent
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: widget.compact ? 30 : 40
                    style: Text.Raised
                    styleColor: Qt.rgba(0, 0, 0, 0.75)
                }
                Text {
                    text: widget.hasWeather ? widget.degrees(widget.weather.current.temperature_2m) : ""
                    color: MobileTheme.foreground
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: widget.compact ? 26 : 34
                    font.bold: true
                    style: Text.Raised
                    styleColor: Qt.rgba(0, 0, 0, 0.75)
                }
            }
            Text {
                visible: !widget.compact
                Layout.alignment: widget.narrow ? Qt.AlignHCenter : Qt.AlignRight
                text: (widget.weather.condition || "") + (widget.weather.stale ? " · offline" : "")
                color: MobileTheme.foreground
                opacity: 0.9
                font.family: MobileTheme.fontFamily
                font.pixelSize: 13
                style: Text.Raised
                styleColor: Qt.rgba(0, 0, 0, 0.75)
            }
            Text {
                visible: widget.today !== null
                Layout.alignment: widget.narrow ? Qt.AlignHCenter : Qt.AlignRight
                text: widget.today ? "↑ " + widget.degrees(widget.today.max) + "  ↓ " + widget.degrees(widget.today.min) : ""
                color: MobileTheme.foreground
                opacity: 0.9
                font.family: MobileTheme.fontFamily
                font.pixelSize: 13
                style: Text.Raised
                styleColor: Qt.rgba(0, 0, 0, 0.75)
            }
            TapHandler { onTapped: if (widget.host) widget.host.weatherOpened() }
        }
    }
}
