pragma Singleton
import QtQuick

QtObject {
    function glyph(kind) {
        return {
            clear: "󰖙",
            mostly_clear: "󰖕",
            clear_night: "\u{F0594}",
            mostly_clear_night: "\u{F0F31}",
            cloudy: "󰖐",
            fog: "󰖑",
            drizzle: "󰖗",
            rain: "󰖖",
            snow: "󰖘",
            showers: "󰖒",
            thunder: "󰖓",
            unknown: "󰖐"
        }[kind] || "󰖐";
    }
}
