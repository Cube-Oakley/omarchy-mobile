import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import Quickshell
import Quickshell.Io
import Quickshell.Wayland
import Quickshell.Hyprland
import Quickshell.Services.Notifications

ShellRoot {
    id: root
    property string page: ""
    property string startupError: ""
    property string lastDispatch: ""
    property bool openLanded: false
    property bool openingCard: false
    property string keyboardError: ""
    property var keyboardQueue: []
    property var keyboardLayer: null
    // Alphabetical, so an app stays where it was found.
    property var apps: DesktopEntries.applications.values.filter(app => !app.noDisplay && app.command.length > 0)
        .sort((a, b) => (a.name || "").toLowerCase().localeCompare((b.name || "").toLowerCase()))
    readonly property int workspace: Hyprland.focusedWorkspace ? Hyprland.focusedWorkspace.id : 1
    // A window that opens before Quickshell has loaded the workspace list
    // (an app started with the session) keeps a null workspace for good, and
    // the shell then treats the phone as home: no switcher card, no minimize.
    // Re-query workspaces, then windows, until every window has one.
    readonly property bool untrackedWindow: Hyprland.toplevels.values.some(w => !w.workspace)
    onUntrackedWindowChanged: if (untrackedWindow) workspaceRepair.begin()
    Timer {
        id: workspaceRepair
        property int attempts: 0
        function begin() { attempts = 0; Hyprland.refreshWorkspaces(); restart(); }
        Component.onCompleted: begin()
        interval: 250
        onTriggered: {
            Hyprland.refreshToplevels();
            if (root.untrackedWindow && ++attempts < 8) { Hyprland.refreshWorkspaces(); restart(); }
        }
    }
    function windowAddress(window) {
        if (!window) return "";
        const address = String(window.address).replace(/^0x/, "");
        return /^[0-9a-fA-F]+$/.test(address) ? "0x" + address : "";
    }
    function windowAction(window, action, extra) {
        const address = windowAddress(window);
        if (!address) return;
        const selector = 'window = "address:' + address + '"';
        dispatch(action + '({' + selector + (extra ? ', ' + extra : '') + '})');
    }
    function focusWindow(window) {
        const address = windowAddress(window);
        if (!address || !window.workspace || focusRequest.running) return;
        // Workspace 1 is the mobile foreground. Merely focusing a parked
        // workspace leaves atHome true, so the home input layer covers the app.
        const others = windowsOn(1).filter(w => windowAddress(w) !== address);
        let slot = window.workspace.id;
        if (slot === 1 && others.length) {
            const used = {};
            Hyprland.toplevels.values.forEach(w => { if (w.workspace) used[w.workspace.id] = true; });
            slot = 2;
            while (used[slot]) slot++;
        }
        const lines = [];
        others.forEach(w => {
            const other = windowAddress(w);
            if (other) lines.push('dispatch hl.dsp.window.move({window="address:' + other + '", workspace=' + slot + ', follow=false})');
        });
        if (window.workspace.id !== 1)
            lines.push('dispatch hl.dsp.window.move({window="address:' + address + '", workspace=1, follow=false})');
        lines.push("dispatch hl.dsp.focus({workspace=1})");
        lines.push('dispatch hl.dsp.focus({window="address:' + address + '"})');
        focusRequest.command = ["hyprctl", "--batch", lines.join("; ")];
        console.log("MOBILE_FOCUS " + address + " workspace=1");
        focusRequest.running = true;
    }
    Process {
        id: focusRequest
        stdout: StdioCollector {}
        stderr: StdioCollector {}
        onExited: (code, status) => {
            Hyprland.refreshWorkspaces();
            Hyprland.refreshToplevels();
            // Dismiss on a clean exit. hyprctl often prints nothing or an extra
            // token after a successful batch; requiring every word to be "ok"
            // left the switcher up after the app had already focused.
            const output = (stdout.text + " " + stderr.text).trim();
            if (!root.openingCard) {
                if (code === 0) root.closeDrawer();
                else root.startupError = "Could not focus that window";
                return;
            }
            if (code === 0) {
                root.startupError = "";
                if (output && output.split(/\s+/).some(word => word !== "ok"))
                    console.warn("MOBILE_FOCUS_NOTE " + output);
                root.openLanded = true;
                root.tryReveal();
            } else {
                root.openingCard = false;
                root.openLanded = false;
                overview.cancelExpand();
                root.startupError = "Could not open that app";
                console.warn("MOBILE_FOCUS_FAILED " + output + " :: " + root.lastDispatch);
            }
        }
    }

    function dispatch(action) {
        Quickshell.execDetached(["hyprctl", "dispatch", action]);
    }
    function keyboard(action) {
        keyboardQueue = keyboardQueue.concat([action]);
        runKeyboardRequest();
    }
    function runKeyboardRequest() {
        if (keyboardRequest.running || keyboardQueue.length === 0) return;
        const action = keyboardQueue[0];
        keyboardQueue = keyboardQueue.slice(1);
        keyboardRequest.command = [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-keyboard", action];
        keyboardRequest.running = true;
    }
    Process {
        id: keyboardRequest
        stderr: StdioCollector {}
        onExited: (code, status) => {
            root.keyboardError = code === 0 ? "" : "Keyboard failed to start";
            if (code !== 0) console.warn("MOBILE_KEYBOARD_FAILED exit=" + code + " " + stderr.text.trim());
            Qt.callLater(root.runKeyboardRequest);
        }
    }
    DrawerMotion {
        id: motion
        travel: drawer.height
        onClosed: root.page = ""
    }
    property bool clearing: false
    function closeDrawer() {
        root.page = "";
        motion.dismiss();
        // Present one transparent frame. Otherwise the next open can show
        // the previous switcher image for a frame before Qt draws the new one.
        clearing = true;
        clearFrame.restart();
    }
    Timer { id: clearFrame; interval: 32; onTriggered: root.clearing = false }
    readonly property bool atHome: !Hyprland.toplevels.values.some(w => w.workspace && w.workspace.id === 1)
    readonly property int workspaceLimit: MobileTheme.state.device.workspaceCount || 8
    function windowsOn(id) {
        return Hyprland.toplevels.values.filter(w => w.workspace && w.workspace.id === id);
    }
    function foregroundApp() {
        const tops = Hyprland.toplevels.values;
        for (let i = 0; i < tops.length; i++) {
            const window = tops[i];
            if (window.workspace && window.workspace.id === 1) return true;
        }
        return false;
    }
    function requestBack() {
        if (page !== "" || shade.opened) return;
        if (!foregroundApp()) return;
        const runtime = Quickshell.env("XDG_RUNTIME_DIR") || "/tmp";
        Quickshell.execDetached(["sh", "-c", "mkdir -p \"$1\" && printf '%s\\n' \"$(date +%s%N)\" > \"$1/back\"", "omarchy-back", runtime + "/omarchy-mobile"]);
        console.log("MOBILE_BACK");
    }
    function beginDrawer(name) {
        shade.close();
        if (page !== name) { motion.begin(); motion.progress = 0; page = name; }
        keyboard("hide"); motion.begin();
    }
    property bool parkedGesture: false
    property var parkedAddresses: []
    property bool gestureFromApp: false
    // Set only when the finger goes down while the switcher is already open.
    // The gesture that opens the switcher also sets page to spaces, so the
    // release check cannot use the page alone.
    property bool gestureClosesSwitcher: false
    function edgeStarted() {
        gestureClosesSwitcher = page === "spaces";
        if (gestureClosesSwitcher) return;
        parkedGesture = false;
        parkedAddresses = [];
        gestureFromApp = !atHome;
        if (atHome) return;
        overview.touchWindows(windowsOn(1));
        beginDrawer("spaces");
        if (overview.frontReady) schedulePark();
    }
    Timer { id: parkHold; interval: 48; onTriggered: root.parkForeground() }
    function schedulePark() {
        if (!gestureFromApp || parkedGesture) return;
        parkHold.restart();
    }
    function parkForeground() {
        if (parkedGesture || atHome) return;
        const command = goHome();
        if (!command || command === "stay") return;
        parkedGesture = true;
        Quickshell.execDetached(command);
    }
    function edgeHeld() {
        if (gestureClosesSwitcher) return;
        overview.browse = 0;
        const opening = page !== "spaces";
        if (opening) beginDrawer("spaces");
        if (!gestureFromApp) {
            // From home, the backdrop dims in while the cards grow and fade in
            // (the minimize animation reversed), with one easing and duration.
            // animateTo() also ends the drag beginDrawer() started, so finger
            // jitter while held cannot pull the switcher back to the finger.
            // Releasing calls this again; the reveal is already running then.
            if (opening) {
                motion.progress = 0;
                overview.revealFromHome(210);
                motion.animateTo(1);
            }
        } else {
            overview.home = 0;
            motion.animateTo(1);
        }
        if (gestureFromApp && overview.frontReady) schedulePark();
    }
    function edgeReleased(distance, velocity, held) {
        if (gestureClosesSwitcher) {
            gestureClosesSwitcher = false;
            if (!held && distance >= 48) closeDrawer();
            return;
        }
        if (held) { edgeHeld(); return; }
        if (!gestureFromApp) {
            if (distance < 64) return;
            beginDrawer("apps");
            motion.animateTo(1);
            return;
        }
        if (distance < 48 && motion.progress < 0.2) { parkHold.stop(); restoreForeground(); closeDrawer(); return; }
        parkForeground();
        overview.minimizeHome();
    }
    function goHome() {
        const home = windowsOn(1);
        if (home.length === 0) { dispatch("hl.dsp.focus({workspace = 1})"); return; }
        const used = {};
        Hyprland.toplevels.values.forEach(w => { if (w.workspace) used[w.workspace.id] = true; });
        let slot = 0;
        for (let i = 2; i <= workspaceLimit; i++) if (!used[i]) { slot = i; break; }
        if (!slot) { motion.animateTo(1); return "stay"; }
        const commands = home.map(w => {
            const address = windowAddress(w);
            return address ? 'dispatch hl.dsp.window.move({window = "address:' + address + '", workspace = ' + slot + ', follow = false})' : "";
        }).filter(Boolean);
        commands.push("dispatch hl.dsp.focus({workspace = 1})");
        overview.touchWindows(home);
        parkedAddresses = home.map(windowAddress).filter(Boolean);
        parkedSlot = slot;
        console.log("MOBILE_HOME slot=" + slot);
        return ["hyprctl", "--batch", commands.join("; ")];
    }
    property int parkedSlot: 0
    function restoreForeground() {
        if (!parkedGesture || !parkedAddresses.length) return;
        const lines = parkedAddresses.map(address => 'dispatch hl.dsp.window.move({window = "address:' + address + '", workspace = 1, follow = false})');
        lines.push("dispatch hl.dsp.focus({workspace = 1})");
        if (parkedAddresses[0]) lines.push('dispatch hl.dsp.focus({window="address:' + parkedAddresses[0] + '"})');
        parkedGesture = false;
        Quickshell.execDetached(["hyprctl", "--batch", lines.join("; ")]);
    }
    function finishHome() {
        if (page !== "spaces") return;
        if (parkedGesture) { closeDrawer(); return; }
        const command = goHome();
        if (command === "stay") return;
        if (!command) { closeDrawer(); return; }
        if (homeRequest.running) return;
        homeRequest.command = command;
        homeRequest.running = true;
    }
    Process {
        id: homeRequest
        stdout: StdioCollector {}
        stderr: StdioCollector {}
        onExited: root.closeDrawer()
    }
    function tryReveal() {
        if (!openingCard || !openLanded || overview.lift < 0.98) return;
        revealHold.restart();
    }
    Timer {
        id: revealHold
        interval: 48
        onTriggered: root.finishReveal()
    }
    function finishReveal() {
        if (!openingCard || !openLanded || overview.lift < 0.98) return;
        openingCard = false;
        openLanded = false;
        closeDrawer();
        overview.expandingId = 0;
        overview.lift = 0;
        overview.syncCards();
    }
    function cardWindows(id) {
        const cards = overview.cardsModel || [];
        for (let i = 0; i < cards.length; i++)
            if (cards[i].id === id && cards[i].windows && cards[i].windows.length)
                return cards[i].windows;
        return windowsOn(id);
    }
    function openGroup(id) {
        if (focusRequest.running || overview.expandingId) return;
        const incoming = cardWindows(id);
        if (!incoming.length) {
            console.warn("MOBILE_OPEN empty " + id);
            startupError = "That app is no longer open";
            return;
        }
        const lines = ["dispatch hl.dsp.focus({workspace=1})"];
        const wanted = {};
        incoming.forEach(w => { const address = windowAddress(w); if (address) wanted[address] = true; });
        const slot = incoming[0] && incoming[0].workspace ? incoming[0].workspace.id : id;
        if (slot !== 1) {
            windowsOn(1).forEach(w => {
                const address = windowAddress(w);
                if (address && !wanted[address]) lines.push('dispatch hl.dsp.window.move({window = "address:' + address + '", workspace = ' + slot + ', follow = false})');
            });
            incoming.forEach(w => {
                const address = windowAddress(w);
                if (address) lines.push('dispatch hl.dsp.window.move({window = "address:' + address + '", workspace = 1, follow = false})');
            });
        }
        const first = windowAddress(incoming[0]);
        if (first) lines.push('dispatch hl.dsp.focus({window="address:' + first + '"})');
        lastDispatch = lines.join("; ");
        openLanded = false;
        openingCard = true;
        // Expand first: touching reorders the cards, and a reorder outside an
        // expand rebuilds every card, blanking their pictures mid-animation.
        overview.expandCard(id);
        overview.touchWindows(incoming);
        focusRequest.command = ["hyprctl", "--batch", lastDispatch];
        focusRequest.running = true;
        console.log("MOBILE_OPEN " + id + " " + first);
    }
    function closeGroup(id) {
        cardWindows(id).forEach(w => windowAction(w, "hl.dsp.window.close", ""));
    }
    function tileGroups(source, target) {
        const moving = windowsOn(source);
        const staying = windowsOn(target);
        moving.forEach(w => windowAction(w, "hl.dsp.window.move", "workspace = " + target + ", follow = false"));
        overview.touchWindows(moving.concat(staying));
    }
    function showPage(name) {
        shade.close();
        if (page === name && motion.progress > 0) closeDrawer();
        else { page = name; keyboard("hide"); motion.animateTo(1); }
    }
    function launch(app, from, icon) {
        const command = app.runInTerminal ? ["kitty", "--"].concat(app.command) : app.command;
        const origin = from ? from.mapToItem(launchZoom, 0, 0) : null;
        const glyph = (app.name || "A").substring(0, 1);
        closeDrawer();
        if (origin) launchZoom.beginAt(origin.x, origin.y, from.width, from.height, icon || "", glyph);
        Quickshell.execDetached({ command: command, workingDirectory: app.workingDirectory || Quickshell.env("HOME") });
        console.log("MOBILE_LAUNCH " + app.id);
    }
    // The call chip and an answered call: bring Phone forward, or start it.
    function openPhone() {
        closeDrawer();
        shade.close();
        keyboard("hide");
        const open = Hyprland.toplevels.values.find(w => w.title === "Phone");
        if (open) { focusWindow(open); return; }
        // A newly launched dialer also needs a free foreground workspace,
        // otherwise answering from another app tiles the call controls.
        const park = goHome();
        if (park === "stay") { startupError = "Close an app before opening Phone"; return; }
        if (park) {
            phoneLaunchPreparation.command = park;
            phoneLaunchPreparation.running = true;
        } else launchPhone();
    }
    function launchPhone() {
        const app = root.apps.find(entry => entry.id === "phone");
        if (app) launch(app);
        else Quickshell.execDetached([Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-app", "launch", "phone"]);
        phoneFocus.attempts = 0;
        phoneFocus.restart();
    }
    Process {
        id: phoneLaunchPreparation
        onExited: (code, status) => {
            if (code === 0) root.launchPhone();
            else root.startupError = "Could not open Phone";
        }
    }
    Timer {
        id: phoneFocus
        property int attempts: 0
        interval: 100
        onTriggered: {
            const open = Hyprland.toplevels.values.find(w => w.title === "Phone" && w.workspace);
            if (open && !focusRequest.running) { root.focusWindow(open); return; }
            Hyprland.refreshWorkspaces();
            Hyprland.refreshToplevels();
            if (++attempts < 50) restart();
        }
    }
    function terminal(from) {
        const origin = from ? from.mapToItem(launchZoom, 0, 0) : null;
        closeDrawer();
        if (origin) launchZoom.beginAt(origin.x, origin.y, from.width, from.height, "", ">");
        Quickshell.execDetached(["kitty"]);
        keyboard("hide");
    }
    function selectWorkspace(number) {
        dispatch("hl.dsp.focus({workspace = " + number + "})");
        closeDrawer();
        console.log("MOBILE_WORKSPACE " + number);
    }
    function openSettings(panel, from) {
        const origin = from ? from.mapToItem(launchZoom, 0, 0) : null;
        closeDrawer();
        if (origin) launchZoom.beginAt(origin.x, origin.y, from.width, from.height, "", "S");
        keyboard("hide");
        const page = panel || "home";
        Quickshell.execDetached([Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-settings", page]);
        console.log("MOBILE_SETTINGS " + page);
    }

    Process {
        id: setup
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-session"]
        running: true
        stderr: StdioCollector { onStreamFinished: if (this.text.trim()) console.warn(this.text) }
        onExited: (code, status) => {
            if (code === 0) root.keyboard("start");
            else root.startupError = "Display setup needs attention";
        }
    }
    IpcHandler {
        target: "mobile"
        function apps(): void { root.showPage("apps"); }
        function launchApp(id: string): bool {
            const app = root.apps.find(entry => entry.id === id);
            if (!app) return false;
            root.launch(app);
            return true;
        }
        function spaces(): void { root.showPage("spaces"); }
        function themes(): void { root.openSettings("appearance"); }
        function settings(panel: string): void { root.openSettings(panel || "home"); }
        function dismiss(): void { root.closeDrawer(); shade.close(); }
        function controls(): void { shade.toggle(); }
        // The always-on display (omarchy-mobile-display ambient / on).
        function ambient(on: bool): void { if (on) { root.closeDrawer(); shade.close(); } MobileStatus.setAmbient(on); }
        function controlState(): string { return JSON.stringify({open: shade.opened, detail: shade.detail, notifications: shade.notificationCount, wifi: shade.wifi.connected, networks: shade.networks.length}); }
        function detail(name: string): void { if (["wifi", "battery", "calendar", "weather", "stats", "clipboard", "speedtest"].indexOf(name) >= 0) shade.showDetail(name); }
        function terminal(): void { root.terminal(); }
        function powerMenu(): void { root.closeDrawer(); shade.close(); systemPowerMenu.show(); }
        function phone(): void { root.openPhone(); }
        // Power and volume keys silence a ringing call, as on Android.
        function callSilence(): bool { const ringing = phoneCalls.ringing; phoneCalls.silence(); return ringing; }
        function callState(): string { return JSON.stringify({calls: phoneCalls.calls, ringing: phoneCalls.ringing}); }
        function keyboard(): void { root.keyboard("toggle"); }
        function volume(action: string): void { volumeOsd.adjust(action); }
        function crt(action: string, token: string): string {
            if (action === "cover") { crtPower.cover(token); return "covered"; }
            if (action === "off" || action === "on") { crtPower.play(action, token); return "playing"; }
            return "ignored";
        }
        function screenshotPrivacy(enabled: bool): void { shade.closeDetail(); shade.screenshotPrivacy = enabled; }
        function workspace(number: int): void { if (number > 0 && number < 100) root.selectWorkspace(number); }
        function windows(): string { return JSON.stringify(Hyprland.toplevels.values.map(w => ({address: w.address, title: w.title, workspace: w.workspace ? w.workspace.id : 0}))); }
        function focus(address: string): void {
            const window = Hyprland.toplevels.values.find(w => root.windowAddress(w) === "0x" + address.replace(/^0x/, ""));
            if (window) root.focusWindow(window);
        }
    }
    Variants {
        model: Quickshell.screens
        Wallpaper { required property var modelData; screen: modelData }
    }
    SystemClock { id: clock; precision: SystemClock.Minutes }
    VolumeOsd { id: volumeOsd }
    PowerMenu { id: systemPowerMenu }
    RotateButton {}
    AmbientDisplay { notifications: shade.notificationValues }
    // The alert slider sets the ring group, as on Android: muted in Vibrate
    // and Silent, heard in Ring, with a short buzz on reaching Vibrate. Only
    // movements count; the position the session starts with is left alone.
    property string sliderPosition: ""
    Connections {
        target: MobileStatus
        function onControlsChanged() {
            const position = MobileStatus.controls.slider || "";
            if (!position || position === root.sliderPosition) return;
            const moved = root.sliderPosition !== "";
            root.sliderPosition = position;
            if (!moved) return;
            root.applySliderMute();
            if (position === "vibrate") MobileStatus.control(["vibrate", "80", "80"]);
        }
    }
    // A move made while the last one is still applying is applied after it.
    function applySliderMute() {
        if (sliderMute.running) return;
        sliderMute.applied = root.sliderPosition;
        sliderMute.command = [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-volume",
                              "mute", "ring", sliderMute.applied === "ring" ? "off" : "on"];
        sliderMute.running = true;
    }
    Process {
        id: sliderMute
        property string applied: ""
        onExited: {
            if (applied !== root.sliderPosition) root.applySliderMute();
            else MobileStatus.refresh();
        }
    }
    Process {
        id: clipboardWatch
        command: ["wl-paste", "--watch", Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-clipboard", "ingest"]
        running: true
        onExited: clipboardWatchRetry.start()
    }
    Timer { id: clipboardWatchRetry; interval: 10000; onTriggered: clipboardWatch.running = true }
    IncomingCall {
        id: phoneCalls
        readonly property string slider: MobileStatus.controls.slider || "ring"
        quiet: MobileStatus.dnd || slider === "silent"
        vibrateOnly: slider === "vibrate"
        haptics: MobileStatus.controls.haptics !== false
        onArrived: { root.closeDrawer(); shade.close(); root.keyboard("hide"); toast.dismiss(); }
        onBuzz: MobileStatus.control(["vibrate", "600", "90"])
        onOpenPhone: root.openPhone()
    }
    // Preferences the shell follows as they change (Settings writes them).
    property var prefsData: ({})
    FileView {
        id: prefsFile
        path: (Quickshell.env("XDG_CONFIG_HOME") || Quickshell.env("HOME") + "/.config") + "/omarchy-mobile/prefs.json"
        watchChanges: true
        onFileChanged: reload()
        onLoaded: { try { root.prefsData = JSON.parse(text()); } catch (e) { root.prefsData = {}; } }
        onLoadFailed: root.prefsData = {}
    }
    Timer { interval: 30000; running: true; repeat: true; onTriggered: prefsFile.reload() }
    // Screen timeout (prefs screenTimeout, seconds; 0 is never): that long
    // without input turns the screen off, or to the always-on display, as a
    // power-key press would; never with a call. Apps that inhibit idle (a
    // video) keep it on. The screen lighting without input (a call, a wake)
    // starts the count again.
    readonly property int screenTimeout: [0, 15, 30, 60, 120, 300, 600].indexOf(prefsData.screenTimeout) >= 0
        ? prefsData.screenTimeout : 60
    property bool idleRestarting: false
    IdleMonitor {
        id: screenIdle
        enabled: root.screenTimeout > 0 && phoneCalls.calls.length === 0 && !root.idleRestarting
        timeout: root.screenTimeout
        respectInhibitors: true
        onIsIdleChanged: if (isIdle) Quickshell.execDetached([Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-display",
                                                              "idle", root.prefsData.alwaysOn === true ? "ambient" : "off"])
    }
    Timer { id: idleRestart; interval: 250; onTriggered: root.idleRestarting = false }
    // Sleep while the screen is dark (omarchy-mobile-sleep, through the
    // device's suspend adapter; Settings can turn it off).
    Process {
        id: sleepPolicy
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-sleep", "run"]
        running: true
        onExited: (code, status) => { if (code !== 3) sleepPolicyRetry.start(); }
    }
    Timer { id: sleepPolicyRetry; interval: 10000; onTriggered: sleepPolicy.running = true }
    // Texts: the watcher files new texts from the modem and notifies; the
    // unread count is kept for the status bar.
    property int unreadTexts: 0
    Process {
        id: textWatch
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-messages", "watch"]
        running: true
        stdout: SplitParser {
            onRead: line => {
                try { const data = JSON.parse(line); if (data.ok) root.unreadTexts = data.unread || 0; } catch (e) {}
            }
        }
        // Exit 3: no ModemManager, nothing to follow.
        onExited: (code, status) => { if (code !== 3) textWatchRetry.start(); }
    }
    Timer { id: textWatchRetry; interval: 5000; onTriggered: textWatch.running = true }
    NotificationToast {
        id: toast
        blocked: MobileStatus.dnd || shade.opened || shade.screenshotPrivacy || MobileStatus.ambient
        onActivate: shade.open()
    }

    NotificationShade {
        id: shade
        topInset: statusBar.height
        onOpening: { root.closeDrawer(); root.keyboard("hide"); toast.dismiss(); }
        onArrived: notification => {
            toast.offer(notification);
            // A buzz as on Android: not in Do Not Disturb or Silent, and not
            // for low-urgency notifications.
            if (!MobileStatus.dnd && MobileStatus.controls.haptics === true && root.sliderPosition !== "silent"
                    && notification.urgency !== NotificationUrgency.Low)
                MobileStatus.control(["vibrate", "150", "70"]);
        }
        onKeyboardRequested: action => root.keyboard(action)
        onVolumeRequested: action => { volumeOsd.adjust(action); MobileStatus.refresh(); }
        onSettingsRequested: panel => { shade.close(); root.openSettings(panel); }
    }
    PanelWindow {
        id: statusBar
        anchors { top: true; left: true; right: true }
        implicitHeight: 36
        color: MobileTheme.background
        WlrLayershell.namespace: "omarchy-mobile-status"
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.None
        RowLayout {
            anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 16
            Text { font.family: MobileTheme.fontFamily; text: "omarchy"; color: MobileTheme.accent; font.pixelSize: 17; font.bold: true }
            // Unread texts: tap for Messages.
            Text {
                visible: root.unreadTexts > 0
                font.family: MobileTheme.fontFamily
                text: "\u{F0369} " + root.unreadTexts
                color: MobileTheme.accent
                font.pixelSize: 14
                TapHandler { onTapped: Quickshell.execDetached([Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-app", "launch", "messages"]) }
            }
            // A call in progress: tap to return to it.
            Rectangle {
                visible: phoneCalls.ongoing !== null
                implicitWidth: callChip.implicitWidth + 18
                implicitHeight: 24
                radius: MobileTheme.radius(12)
                color: MobileTheme.success
                Text { id: callChip; anchors.centerIn: parent; font.family: MobileTheme.fontFamily; text: "\u{F03F2} " + phoneCalls.elapsed; color: MobileTheme.background; font.pixelSize: 13; font.bold: true }
                TapHandler { onTapped: root.openPhone() }
            }
            Text { font.family: MobileTheme.fontFamily; text: root.keyboardError || root.startupError || "Space " + root.workspace; color: MobileTheme.secondary; font.pixelSize: 13; Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter }
            WifiStatus { TapHandler { onTapped: shade.showDetail("wifi") } }
            BatteryStatus { TapHandler { onTapped: shade.showDetail("battery") } }
            Text { font.family: MobileTheme.fontFamily; text: Qt.formatDateTime(clock.date, "h:mm"); color: MobileTheme.foreground; font.pixelSize: 16; TapHandler { onTapped: shade.showDetail("calendar") } }
        }
        DragHandler {
            id: topPull
            target: null; xAxis.enabled: false
            property bool tracking: false
            onActiveChanged: {
                if (active) tracking = false;
                else if (tracking) { tracking = false; shade.release(); }
            }
            onActiveTranslationChanged: {
                if (!active) return;
                if (!tracking && activeTranslation.y > 8) { tracking = true; shade.begin(); }
                if (tracking) shade.move(activeTranslation.y, centroid.velocity.y);
            }
            onCanceled: { if (tracking) { tracking = false; shade.cancel(); } }
        }
    }

    // Keep navigation above the on-screen keyboard without reserving a band.
    Connections {
        target: Hyprland
        function onRawEvent(event) {
            if ((event.name === "openlayer" || event.name === "closelayer") && event.data === "wvkbd") {
                if (event.name === "openlayer") {
                    navigation.visible = false;
                    raiseNavigation.restart();
                }
                keyboardGeometryRefresh.restart();
            }
        }
    }
    Timer { id: raiseNavigation; interval: 50; onTriggered: navigation.visible = true }
    Timer {
        id: keyboardGeometryRefresh; interval: 70
        onTriggered: { if (keyboardGeometry.running) restart(); else keyboardGeometry.running = true; }
    }
    Process {
        id: keyboardGeometry
        command: ["hyprctl", "-j", "layers"]
        running: true
        stdout: StdioCollector {}
        onExited: (code, status) => {
            if (code !== 0) return;
            try {
                const monitors = JSON.parse(stdout.text);
                let found = null;
                for (const monitor of Object.values(monitors))
                    for (const level of Object.values(monitor.levels))
                        for (const layer of level)
                            if (layer.namespace === "wvkbd") found = layer;
                root.keyboardLayer = found;
            } catch (e) { console.warn("MOBILE_KEYBOARD_GEOMETRY " + e); }
        }
    }
    PanelWindow {
        id: keyboardHandle
        visible: root.keyboardLayer !== null
        anchors.bottom: true
        margins.bottom: root.keyboardLayer ? root.keyboardLayer.h : 0
        implicitWidth: 144; implicitHeight: 24
        exclusionMode: ExclusionMode.Ignore
        exclusiveZone: 0
        color: "transparent"
        WlrLayershell.namespace: "omarchy-mobile-keyboard-handle"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.None
        Rectangle {
            anchors.fill: parent; anchors.margins: 2; radius: MobileTheme.radius(10)
            color: MobileTheme.background; opacity: 0.95
            Rectangle { anchors.centerIn: parent; width: 54; height: 3; radius: 2; color: MobileTheme.foreground; opacity: 0.6 }
        }
        KeyboardDismiss {
            anchors.fill: parent
            onDismissed: {
                if (shade.keyboardOwned) shade.releaseKeyboard();
                else root.keyboard("hide");
            }
        }
    }
    PanelWindow {
        id: backEdge
        anchors { left: true; top: true; bottom: true }
        implicitWidth: 200
        color: "transparent"
        exclusionMode: ExclusionMode.Ignore
        exclusiveZone: 0
        WlrLayershell.namespace: "omarchy-mobile-back"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.None
        // Only the left strip is touchable until the swipe starts. The region
        // then opens up so the finger can travel inward without the gesture dying.
        mask: Region {
            // The resting strip has to contain a full back-swipe. The region
            // widens as soon as the finger lands, before it travels inward.
            width: root.page === "" && !shade.opened && launchZoom.t < 0.01 ? ((backSwipe.pressed || backSwipe.tracking) ? backEdge.width : 64) : 0
            height: backEdge.height
        }
        LeftBack {
            id: backSwipe
            anchors.fill: parent
            pill: MobileTheme.surface
            ink: MobileTheme.foreground
            enabled: root.page === "" && !shade.opened && launchZoom.t < 0.01
            onCommitted: root.requestBack()
        }
    }
    PanelWindow {
        id: navigation
        anchors { bottom: true; left: true; right: true }
        exclusionMode: ExclusionMode.Ignore
        exclusiveZone: 0
        implicitHeight: 24
        color: "transparent"
        // Keep drawer swipes but let the keyboard's Enter corner receive taps.
        mask: Region { width: root.keyboardLayer ? navigation.width * 2 / 3 : navigation.width; height: navigation.height }
        WlrLayershell.namespace: "omarchy-mobile-navigation"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.None
        EdgeGestures {
            handleColor: MobileTheme.foreground
            anchors.fill: parent
            onTapped: if (root.page === "" && !root.atHome) root.keyboard("show")
            onStarted: root.edgeStarted()
            onMoved: (distance, velocity) => motion.update(distance, velocity)
            onHeld: root.edgeHeld()
            onReleased: (distance, velocity, held) => root.edgeReleased(distance, velocity, held)
            onCanceled: if (motion.dragging) motion.cancel()
        }
    }

    PanelWindow {
        id: drawer
        // Keep the surface mapped. Hiding it lets the next open come back as a
        // tiny top-left buffer before the layer is configured to full screen.
        visible: true
        mask: Region {
            // While the shade is open it must receive the swipe that closes it.
            // The home surface only listens when that tray is fully out of the way.
            readonly property bool homeFree: root.atHome && root.page === "" && !shade.opened && !homeDrag.active
            width: root.page !== "" || launchZoom.t > 0.01 || homeFree || homeDrag.active || root.clearing ? drawer.width : 0
            height: root.page !== "" || launchZoom.t > 0.01 || homeFree || homeDrag.active || root.clearing ? drawer.height : 0
        }
        anchors { top: true; left: true; right: true; bottom: true }
        exclusionMode: ExclusionMode.Normal
        exclusiveZone: 0
        color: "transparent"
        WlrLayershell.namespace: "omarchy-mobile-drawer"
        WlrLayershell.layer: WlrLayer.Top
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.None
        // On the home screen this surface also takes swipes. An open app leaves
        // the mask empty so those touches reach the app.
        Rectangle { anchors.fill: parent; color: "black"; opacity: root.page === "spaces" ? motion.progress * 0.45 : motion.progress * 0.28 }
        DragHandler {
            id: homeDrag
            target: null
            enabled: homeDrag.active || (root.atHome && root.page === "" && launchZoom.t < 0.01 && !shade.opened)
            xAxis.enabled: false
            yAxis.enabled: true
            property string mode: ""
            onActiveChanged: {
                if (active) { mode = ""; return; }
                if (mode === "up") motion.finish();
                else if (mode === "down") shade.release();
                mode = "";
            }
            onActiveTranslationChanged: {
                if (!active) return;
                const dy = activeTranslation.y;
                if (mode === "" && Math.abs(dy) < 24) return;
                if (mode === "") {
                    if (shade.opened) return;
                    if (dy < 0) { mode = "up"; root.beginDrawer("apps"); }
                    else { mode = "down"; shade.begin(); }
                }
                if (mode === "up") motion.update(-dy, -centroid.velocity.y);
                else shade.move(dy, centroid.velocity.y);
            }
            onCanceled: {
                if (mode === "up") motion.cancel();
                else if (mode === "down") shade.cancel();
                mode = "";
            }
        }
        TapHandler {
            enabled: root.page === "apps"
            onTapped: point => { if (point.position.y < sheet.y) root.closeDrawer(); }
        }
        Rectangle {
            id: sheet
            visible: root.page !== ""
            width: parent.width; height: parent.height
            y: root.page === "spaces" ? 0 : (1 - motion.progress) * height
            radius: root.page === "spaces" ? 0 : (motion.progress > 0.98 ? 0 : 24)
            color: root.page === "apps" ? MobileTheme.background : "transparent"
            clip: root.page !== "spaces"
            Rectangle {
                anchors.fill: parent
                visible: root.page === "spaces"
                color: "black"
                opacity: 0.32 * Math.min(1, overview.present)
            }
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: root.page === "spaces" ? 0 : 24
                spacing: root.page === "spaces" ? 0 : 24
                Item {
                    visible: root.page === "apps"
                    Layout.fillWidth: true
                    implicitHeight: visible ? 88 : 0
                    Text { font.family: MobileTheme.fontFamily; y: 0; text: "EXPLORE"; color: MobileTheme.accent; font.pixelSize: 11; font.letterSpacing: 2.4; font.bold: true }
                    Text { font.family: MobileTheme.fontFamily; y: 23; text: "Applications"; color: MobileTheme.foreground; font.pixelSize: 34; font.bold: true }
                    TouchButton { anchors.right: parent.right; y: 21; implicitWidth: 48; implicitHeight: 48; radius: MobileTheme.radius(24); label: "×"; textSize: 26; onClicked: root.closeDrawer() }
                }
                WorkspaceOverview {
                    id: overview
                    enabled: root.page === "spaces" && motion.progress > 0.35
                    visible: root.page === "spaces"
                    active: visible
                    openness: motion.progress
                    captureScreen: drawer.screen
                    warmPaused: root.page !== "" || shade.opened || launchZoom.t > 0.01 || crtPower.playing || crtPower.mode === "parked"
                    Layout.fillWidth: true
                    Layout.fillHeight: visible
                    onOpenGroup: id => root.openGroup(id)
                    onCovered: root.tryReveal()
                    onMinimized: root.finishHome()
                    onPreviewReady: root.schedulePark()
                    onCloseGroup: id => root.closeGroup(id)
                    onTileGroups: (source, target) => root.tileGroups(source, target)
                    onDismissed: root.closeDrawer()
                }
                Flickable {
                    id: appList
                    enabled: motion.settledOpen
                    visible: root.page === "apps"
                    Layout.fillWidth: true; Layout.fillHeight: visible
                    clip: true; contentHeight: contents.height; boundsBehavior: Flickable.StopAtBounds
                    Column {
                        id: contents; width: parent.width; spacing: 22
                        RowLayout {
                            visible: root.page === "apps"; width: parent.width; spacing: 12
                            TouchButton { id: terminalButton; Layout.fillWidth: true; label: ">_  Terminal"; selected: true; onClicked: root.terminal(terminalButton) }
                            TouchButton { id: settingsButton; Layout.fillWidth: true; label: "Settings"; onClicked: root.openSettings("home", settingsButton) }
                        }
                        GridLayout {
                            width: parent.width; columns: 3; columnSpacing: 12; rowSpacing: 24
                            Repeater {
                                model: root.apps
                                Item {
                                    required property var modelData
                                    Layout.fillWidth: true; Layout.preferredWidth: 1; implicitHeight: 116
                                    Rectangle {
                                        id: appTile
                                        anchors.horizontalCenter: parent.horizontalCenter; width: 70; height: 70; radius: MobileTheme.radius(21); color: appTap.pressed ? MobileTheme.selection : MobileTheme.surface
                                        Image { id: appIcon; anchors.centerIn: parent; width: 40; height: 40; source: modelData.icon ? Quickshell.iconPath(modelData.icon, true) : ""; fillMode: Image.PreserveAspectFit }
                                        Text { font.family: MobileTheme.fontFamily; anchors.centerIn: parent; visible: appIcon.status !== Image.Ready; text: modelData.name.substring(0, 1); color: MobileTheme.accent; font.pixelSize: 28; font.bold: true }
                                    }
                                    Text { font.family: MobileTheme.fontFamily; anchors { top: parent.top; topMargin: 80; left: parent.left; right: parent.right } text: modelData.name; textFormat: Text.PlainText; color: MobileTheme.foreground; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap; maximumLineCount: 2; elide: Text.ElideRight }
                                    TapHandler { id: appTap; onTapped: root.launch(modelData, appTile, appIcon.source) }
                                }
                            }
                        }
                    }
                }
            }
            DrawerPull {
                anchors.fill: parent
                available: root.page === "apps" && motion.progress > 0.05 && !motion.dragging
                atTop: root.page === "spaces" || appList.atYBeginning
                headerBottom: 24 + 88
                blocked: root.page === "spaces" && overview.heldId !== 0
                onStarted: motion.begin()
                onMoved: (distance, velocity) => motion.update(-Math.max(0, distance), -velocity)
                onReleased: motion.finishClose()
                onCanceled: if (motion.dragging) motion.cancel()
            }
            Text {
                visible: root.page === "spaces" && root.startupError !== ""
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 40
                z: 2
                text: root.startupError
                textFormat: Text.PlainText
                color: MobileTheme.foreground
                font.family: MobileTheme.fontFamily
                font.pixelSize: 15
            }
        }
        Item {
            id: launchZoom
            anchors.fill: parent
            z: 4
            visible: growAnim.running || fadeAnim.running || t > 0.01
            property real t: 0
            property real fade: 1
            property real ix: 0
            property real iy: 0
            property real iw: 70
            property real ih: 70
            property string icon: ""
            property string letter: ""
            property var startedAddresses: []
            readonly property real tileX: overview.tileGap
            readonly property real tileY: overview.tileGap
            readonly property real tileW: Math.max(1, width - overview.tileGap * 2)
            readonly property real tileH: Math.max(1, height - overview.tileGap * 2)
            function beginAt(x, y, w, h, iconSource, glyph) {
                const tops = Hyprland.toplevels.values;
                const found = [];
                for (let i = 0; i < tops.length; i++) found.push(String(tops[i].address));
                startedAddresses = found;
                ix = x; iy = y; iw = Math.max(1, w); ih = Math.max(1, h);
                icon = iconSource || "";
                letter = glyph || "";
                launchZoom.fade = 1;
                launchZoom.t = 0;
                fadeAnim.stop();
                revealApp.stop();
                growAnim.restart();
            }
            function newcomerReady() {
                const tops = Hyprland.toplevels.values;
                for (let i = 0; i < tops.length; i++) {
                    const window = tops[i];
                    if (startedAddresses.indexOf(String(window.address)) >= 0) continue;
                    const info = window.lastIpcObject;
                    if (info && info.mapped === false) continue;
                    const size = info && info.size;
                    if (!size || size[0] < 200 || size[1] < 200) continue;
                    if (!window.workspace || window.workspace.id !== 1) continue;
                    return true;
                }
                return false;
            }
            function finish() {
                if (fadeAnim.running || t < 0.05) return;
                fadeAnim.start();
            }
            NumberAnimation { id: growAnim; target: launchZoom; property: "t"; to: 1; duration: 320; easing.type: Easing.OutCubic }
            NumberAnimation { id: fadeAnim; target: launchZoom; property: "fade"; to: 0; duration: 80; easing.type: Easing.OutCubic; onFinished: launchZoom.t = 0 }
            // The new window exists before it has painted. Hold the cover
            // until that first picture is up, then let it show through.
            Timer {
                id: readyHold
                interval: 40
                repeat: true
                running: launchZoom.t > 0.05 && !fadeAnim.running && !revealApp.running
                onTriggered: if (launchZoom.newcomerReady()) revealApp.restart()
            }
            Timer { id: revealApp; interval: 420; onTriggered: launchZoom.finish() }
            Timer { id: launchWait; interval: 1800; running: launchZoom.t > 0.05 && !fadeAnim.running; onTriggered: launchZoom.finish() }
            Rectangle {
                x: launchZoom.ix + (launchZoom.tileX - launchZoom.ix) * launchZoom.t
                y: launchZoom.iy + (launchZoom.tileY - launchZoom.iy) * launchZoom.t
                width: launchZoom.iw + (launchZoom.tileW - launchZoom.iw) * launchZoom.t
                height: launchZoom.ih + (launchZoom.tileH - launchZoom.ih) * launchZoom.t
                radius: MobileTheme.radius(21) + (MobileTheme.radius(8) - MobileTheme.radius(21)) * launchZoom.t
                color: MobileTheme.surface
                opacity: launchZoom.fade
                Image {
                    anchors.centerIn: parent
                    width: 40 + 24 * launchZoom.t
                    height: width
                    source: launchZoom.icon
                    fillMode: Image.PreserveAspectFit
                    visible: status === Image.Ready
                }
                Text {
                    anchors.centerIn: parent
                    visible: !parent.children[0].visible
                    text: launchZoom.letter
                    color: MobileTheme.accent
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 28 + 16 * launchZoom.t
                    font.bold: true
                }
            }
        }
    }
    CrtPower {
        id: crtPower
        onOpened: { root.idleRestarting = true; idleRestart.restart(); }
    }
}
