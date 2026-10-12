import Quickshell
import Quickshell.Io
ShellRoot {
    Process {
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-session", "launch"]
        running: true
        onExited: (code, status) => {
            if (code === 0) Qt.quit();
            else console.error("OnePlus 13 mobile session failed to start");
        }
    }
}
