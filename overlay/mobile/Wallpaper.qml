import QtQuick
import QtQuick.Window
import Quickshell
import Quickshell.Wayland

PanelWindow {
    id: wallpaper
    anchors { top: true; bottom: true; left: true; right: true }
    exclusionMode: ExclusionMode.Ignore
    WlrLayershell.layer: WlrLayer.Background
    WlrLayershell.namespace: "omarchy-mobile-wallpaper"
    WlrLayershell.keyboardFocus: WlrKeyboardFocus.None
    // No input mask: the wallpaper takes the touches nothing else does (the
    // gaps around windows, an empty workspace). Hyprland 0.56 follows one
    // touched surface at a time, and a finger landing on no surface loses the
    // lift of a finger already down on the shell: the shell then counts that
    // finger as held and ignores every tap after it. Holding the phone
    // sideways, the grip on the screen's edge did this to the rotate button.
    color: MobileTheme.background
    Image {
        anchors.fill: parent
        source: MobileTheme.state.wallpaper ? MobileTheme.state.wallpaper.url : ""
        sourceSize: Qt.size(Math.ceil(Screen.width * Screen.devicePixelRatio),
                            Math.ceil(Screen.height * Screen.devicePixelRatio))
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        retainWhileLoading: true
        cache: true
        autoTransform: true
        onStatusChanged: {
            if (status === Image.Error) {
                MobileTheme.wallpaperError = "Wallpaper could not be opened";
                console.warn("MOBILE_WALLPAPER_FAILED " + source);
            } else if (status === Image.Ready || status === Image.Null) {
                MobileTheme.wallpaperError = "";
                console.log("MOBILE_WALLPAPER_READY " + source);
            }
        }
    }
}
