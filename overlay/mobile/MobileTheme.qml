pragma Singleton
import QtQuick
import Quickshell
import Quickshell.Io

Singleton {
    id: palette
    readonly property string fontFamily: state.fontFamily || "JetBrainsMono Nerd Font"
    FileView {
        id: cachedTheme
        path: (Quickshell.env("XDG_STATE_HOME") || Quickshell.env("HOME") + "/.local/state") + "/omarchy-mobile/palette.json"
        blockLoading: true
        printErrors: false
        // A sync anywhere (this shell, Settings, an app) rewrites this file
        // only when something changed, so watching it applies a theme at once.
        watchChanges: true
        onFileChanged: reload()
        onLoaded: {
            try { palette.apply(JSON.parse(text())); }
            catch (e) {}
        }
    }
    // A new state object re-evaluates every themed binding in the shell, which
    // dropped animation frames every few seconds; only real changes apply.
    property string appliedJson: ""
    function apply(next) {
        const json = JSON.stringify(next);
        if (json === appliedJson) return;
        appliedJson = json;
        state = next;
    }
    function initialState() {
        try { return JSON.parse(cachedTheme.text()); }
        catch (e) { return { name: "tokyo-night", colors: {}, themes: [], device: {} }; }
    }
    property var state: initialState()
    readonly property color background: state.colors.background || "#1a1b26"
    readonly property color surface: state.colors.lighter_background || "#24283b"
    readonly property color foreground: state.colors.bright_foreground || "#c0caf5"
    readonly property color secondary: state.colors.foreground || "#a9b1d6"
    readonly property color accent: state.colors.accent || "#7aa2f7"
    readonly property color selection: state.colors.selection || "#292e42"
    readonly property color muted: state.colors.muted || "#414868"
    // The theme's own red and green, for ending and answering calls.
    readonly property color danger: state.colors.red || state.colors.color1 || "#f7768e"
    readonly property color success: state.colors.green || state.colors.color2 || "#9ece6a"
    // Omarchy's corner choice. Square is the default; rounding above zero is round.
    readonly property int cornerRadius: state.radius === undefined ? 0 : state.radius
    readonly property bool square: state.corners ? state.corners === "square" : cornerRadius === 0
    function radius(size) { return square ? 0 : size; }
    property string error: ""
    property string wallpaperError: ""
    property var pendingCommand: []
    readonly property bool busy: pendingCommand.length > 0 || (update.running && update.command[1] !== "sync")
    function request(args) {
        const command = [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-theme"].concat(args);
        if (update.running) pendingCommand = command;
        else { update.command = command; update.running = true; }
    }
    function runPending() {
        if (pendingCommand.length === 0 || update.running) return;
        update.command = pendingCommand;
        pendingCommand = [];
        update.running = true;
    }
    function nextWallpaper() {
        request(["background", "next"]);
    }
    function selectWallpaper(name) {
        request(["background", "set", name]);
    }
    function select(name) {
        request(["set", name]);
    }
    Process {
        id: update
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-theme", "sync"]
        running: true
        stdout: StdioCollector {}
        stderr: StdioCollector {}
        onExited: (code, status) => {
            Qt.callLater(palette.runPending);
            if (code !== 0) {
                palette.error = stderr.text.trim() || "Theme could not be loaded";
                console.warn(palette.error);
                return;
            }
            try { palette.apply(JSON.parse(stdout.text)); palette.error = ""; }
            catch (e) { palette.error = "Theme could not be loaded"; }
        }
    }
    // Only catches changes made outside the shell, such as `omarchy theme set`
    // in a terminal. Each sync costs about 0.2 CPU-s of Python in every shell
    // instance, so it no longer runs every 3 s.
    Timer {
        interval: 30000; running: true; repeat: true
        onTriggered: if (!update.running) { update.command = [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-theme", "sync"]; update.running = true; }
    }
}
