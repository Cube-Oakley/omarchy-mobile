//@ pragma IconTheme Yaru
// App icons come from Yaru (the pragma above), as on desktop Omarchy; its app
// icons are the same in every colour variant.
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
    // The first child, and it must stay first: on a reload Quickshell pairs
    // the shell's children with the old ones by position, and a lock that is
    // not paired with its predecessor aborts the shell (LockScreen.qml).
    LockScreen {
        id: lockScreen
        reloadableId: "omarchy-mobile-lockscreen"
        prefs: root.prefsData
        screenOn: root.screenLit
        dark: root.screenDark
        ambient: MobileStatus.ambient
        onKeyboardRequested: show => root.keyboard(show ? "show" : "hide")
    }
    property string page: ""
    property string startupError: ""
    property string lastDispatch: ""
    property bool openLanded: false
    property bool openingCard: false
    property string keyboardError: ""
    property var keyboardQueue: []
    property var keyboardLayer: null
    // Alphabetical, so an app stays where it was found. Android apps
    // (Waydroid's waydroid.PACKAGE entries) only once chosen in Settings >
    // Apps, which marks the entry; Waydroid's own full-Android window is left
    // out, since each Android app opens as its own window.
    property var apps: DesktopEntries.applications.values.filter(app => app.command.length > 0
            && (app.id.startsWith("waydroid.") ? app.categories.indexOf("X-Omarchy-Mobile-Shown") >= 0
                                               : !app.noDisplay && app.id !== "Waydroid"))
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

    // Over Hyprland's socket: starting hyprctl forked the whole shell on the
    // UI thread, a dropped frame as a closed card finished flying off.
    function dispatch(action) {
        Hyprland.dispatch(action);
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
    // What decides where touches go, for the IPC and the stuck check.
    function inputState() {
        return {
            page: page, progress: motion.progress, dragging: motion.dragging, moving: motion.moving,
            launchT: launchZoom.t, launchFade: launchZoom.fade, grow: growAnim.running, fade: fadeAnim.running,
            clearing: clearing, homeDrag: homeDrag.active, shade: shade.opened, atHome: atHome,
            parked: parkedGesture, fromApp: gestureFromApp, closesSwitcher: gestureClosesSwitcher,
            edge: navEdge.dragActive, edgeTracking: navEdge.tracking,
            back: backSwipe.down || backSwipe.pressed || backSwipe.tracking, keyboard: !!keyboardLayer,
            openingCard: openingCard
        };
    }
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
    // Each open app keeps a workspace of its own from 2 up, as many as are
    // open. A cap of 8 (the device's workspaceCount) left a ninth app on
    // workspace 1 with nowhere to park: it stayed behind the home screen,
    // or the next app opened beside it (October 4).
    readonly property int workspaceLimit: 64
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
    // An Android text field took focus: Android shows the keyboard bridge
    // (devices/pixel7pro/waydroid/keyboard-bridge), which says so in
    // Android's log; Waydroid's watcher (waydroid-container.sh) writes it to
    // $XDG_RUNTIME_DIR/omarchy-mobile/android-keyboard. The phone's keyboard
    // follows it while an Android app is in front: leaving the app with a
    // field still focused hides it, and coming back brings it back.
    readonly property bool androidInFront: Hyprland.toplevels.values.some(w => w.workspace && w.workspace.id === 1
                                                                          && isAndroidWindow(w))
    property bool androidKeyboardShown: false
    FileView {
        path: Quickshell.env("XDG_RUNTIME_DIR") + "/omarchy-mobile/android-keyboard"
        watchChanges: true
        printErrors: false
        onFileChanged: reload()
        onLoaded: root.androidKeyboardShown = text().trim() === "shown"
        onLoadFailed: root.androidKeyboardShown = false
    }
    readonly property bool androidWantsKeyboard: androidKeyboardShown && androidInFront
    // The phone's keyboard stands in for Android's while nothing covers the
    // app: the shade, the drawer and the lock screen hide it, and when they
    // go it comes back if Android still has its keyboard up. Following
    // androidWantsKeyboard alone, it stayed hidden after a reply from the
    // shade, and Android's keyboard space was left black and dead (October 7).
    readonly property bool androidKeyboardUp: androidWantsKeyboard && !shade.opened && page === ""
                                              && !lockScreen.locked
    onAndroidKeyboardUpChanged: keyboard(androidKeyboardUp ? "show" : "hide")
    // Android's screen is on only while an Android app is in front of a lit,
    // unlocked screen. Otherwise every Android app is in Android's
    // background, so it notifies (an app Android thinks is on screen does
    // not), and Android dozes. The watcher runs the request (Android's power
    // keys) as root; a request with no watcher reading is dropped after 2 s.
    // A launch wakes it too, for a few seconds: Android draws nothing while
    // asleep, so an app started then never got a window to bring to front.
    readonly property bool androidAwake: (androidInFront || androidLaunching.running) && screenLit && !lockScreen.locked
    Timer { id: androidLaunching; interval: 10000 }
    onAndroidInFrontChanged: if (androidInFront) androidLaunching.stop()
    // An Android app that leaves the screen with its keyboard up closes the
    // keyboard first (the bridge's HideReceiver, never Back, which could
    // reach the app), so it lays itself out at full height again: the
    // switcher's card showed it squeezed above an empty keyboard space, as
    // Android kept its last frame. The home and switcher gesture asks at its
    // start, while the card's picture is still live (overview.holdLive);
    // Android's sleep asks too, for any other way out, and sleeps a moment
    // later.
    function hideAndroidKeyboard() {
        if (!androidKeyboardShown) return;
        androidPower("hide-keyboard");
        overview.holdLive();
    }
    onAndroidAwakeChanged: {
        if (androidAwake) { androidSleep.stop(); androidPower("awake"); sleepScreenCleanup.restart(); }
        else androidSleep.restart();
    }
    // The blank screen Android showed before it slept (keyboard-bridge's
    // SleepActivity, out of sight) closes itself once an app covers it, but
    // Waydroid keeps its window: close that once Android is awake again.
    Timer {
        id: sleepScreenCleanup; interval: 3000
        onTriggered: {
            if (!root.androidAwake) return;
            Hyprland.toplevels.values.filter(w => root.windowClass(w) === "waydroid.org.omarchy.mobile.keyboard")
                .forEach(w => root.windowAction(w, "hl.dsp.window.close", ""));
        }
    }
    // Going to sleep waits a moment: a switch between two Android apps
    // leaves workspace 1 empty for a few frames.
    Timer {
        id: androidSleep; interval: 1500
        onTriggered: {
            if (root.androidAwake) return;
            if (root.androidKeyboardShown) { root.hideAndroidKeyboard(); androidSleepAfterHide.restart(); }
            else root.androidPower("asleep");
        }
    }
    Timer { id: androidSleepAfterHide; interval: 600; onTriggered: if (!root.androidAwake) root.androidPower("asleep") }
    function androidPower(state) {
        const fifo = Quickshell.env("XDG_RUNTIME_DIR") + "/omarchy-mobile/android-power";
        Quickshell.execDetached(["sh", "-c", "[ -p \"$1\" ] && timeout 2 sh -c 'printf \"%s\\n\" \"$2\" > \"$1\"' power \"$1\" \"$2\"",
                                 "android-power", fifo, state]);
        console.log("MOBILE_ANDROID " + state);
    }
    // A tapped notification opens its app, as on Android: its "default"
    // action when it has one (an Android app's own: Waydroid's bridge fires
    // the notification's intent, which opens the conversation), else the app
    // of its desktop entry. An Android app's window comes forward, and
    // Android is woken to draw it.
    function openNotification(notification) {
        if (!notification) return false;
        const entry = String(notification.desktopEntry || "");
        const open = (notification.actions || []).find(a => a.identifier === "default");
        const app = apps.find(item => item.id === entry || item.id === entry + ".desktop");
        if (!open && !app) return false;
        closeDrawer();
        shade.close();
        if (entry.startsWith("waydroid.")) {
            androidLaunching.restart();
            launchedId = entry;
            launchedAt = Date.now();
            if (open) open.invoke();
            if (!focusApp(entry) && !open && app) launch(app);
        } else if (open) open.invoke();
        else launch(app);
        console.log("MOBILE_NOTIFICATION_OPEN " + entry);
        return true;
    }
    // A reply typed in the shade for an Android app's notification: the
    // watcher has keyboard-bridge's ReplyService fill in the notification's
    // reply action (title and text in base64, one FIFO line).
    function androidReply(notification, text) {
        const fifo = Quickshell.env("XDG_RUNTIME_DIR") + "/omarchy-mobile/android-power";
        const entry = String(notification.desktopEntry || "");
        Quickshell.execDetached(["sh", "-c",
            "[ -p \"$1\" ] && line=\"reply $2 $(printf %s \"$3\" | base64 -w0) $(printf %s \"$4\" | base64 -w0)\" && " +
            "timeout 2 sh -c 'printf \"%s\\n\" \"$2\" > \"$1\"' reply \"$1\" \"$line\"",
            // An empty title would leave a field empty in the line; a space
            // matches no notification, so only a lone one is answered.
            "android-reply", fifo, entry.replace(/^waydroid\./, ""), notification.summary || " ", text]);
        console.log("MOBILE_ANDROID_REPLY " + entry);
    }
    function requestBack() {
        if (page !== "" || shade.opened) return;
        if (!foregroundApp()) return;
        // Android apps (Waydroid's waydroid.PACKAGE windows) get the evdev
        // Back key (158), which Android's Generic.kl maps to BACK. It is held
        // briefly: Android ignored a press and release sent together, and
        // Escape, which only falls back to BACK, some apps keep for themselves.
        // A new window's lastIpcObject is empty until the next refresh.
        const android = Hyprland.toplevels.values.some(w => w.workspace && w.workspace.id === 1
            && String((w.wayland && w.wayland.appId) || (w.lastIpcObject && w.lastIpcObject.class) || "")
                .startsWith("waydroid."));
        if (android) {
            androidBack();
            console.log("MOBILE_BACK android");
            return;
        }
        const runtime = Quickshell.env("XDG_RUNTIME_DIR") || "/tmp";
        Quickshell.execDetached(["sh", "-c", "mkdir -p \"$1\" && printf '%s\\n' \"$(date +%s%N)\" > \"$1/back\"", "omarchy-back", runtime + "/omarchy-mobile"]);
        console.log("MOBILE_BACK");
    }
    function androidBack() {
        Quickshell.execDetached(["sh", "-c", "{ echo 'down 158'; sleep 0.08; echo 'up 158'; } | \"$1\"", "omarchy-back",
                                 Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-vkbd"]);
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
        if (androidWantsKeyboard) hideAndroidKeyboard();
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
    // An app on a parked workspace that takes the focus by itself (Settings
    // shown again while parked, an app raising itself) makes Hyprland go to
    // that workspace, where the shell does not treat it as the open app: the
    // next app opened lands there too, and a swipe opens the drawer over it.
    // It comes forward to workspace 1 instead, as from the switcher; focus left
    // on an empty parked workspace goes back to 1.
    Connections {
        target: Hyprland
        function onFocusedWorkspaceChanged() { pullForward.restart(); }
    }
    Timer {
        id: pullForward
        interval: 150
        onTriggered: root.bringForward()
    }
    function bringForward() {
        const workspace = Hyprland.focusedWorkspace;
        if (!workspace || workspace.id === 1 || focusRequest.running || homeRequest.running || root.parkedGesture
                || root.openingCard)
            return;
        const incoming = windowsOn(workspace.id);
        const lines = ["dispatch hl.dsp.focus({workspace=1})"];
        windowsOn(1).forEach(w => {
            const address = windowAddress(w);
            if (address) lines.push('dispatch hl.dsp.window.move({window = "address:' + address + '", workspace = ' + workspace.id + ', follow = false})');
        });
        incoming.forEach(w => {
            const address = windowAddress(w);
            if (address) lines.push('dispatch hl.dsp.window.move({window = "address:' + address + '", workspace = 1, follow = false})');
        });
        const first = incoming.length ? windowAddress(incoming[0]) : "";
        if (first) lines.push('dispatch hl.dsp.focus({window="address:' + first + '"})');
        console.log("MOBILE_FORWARD " + workspace.id + " " + incoming.length);
        Quickshell.execDetached(["hyprctl", "--batch", lines.join("; ")]);
    }
    function restoreForeground() {
        if (!parkedGesture || !parkedAddresses.length) return;
        // A window that opened on 1 meanwhile (started from outside the
        // shell, or an incoming call) is the open app now. Moving the parked
        // one back would tile the two into one card, and closing that card
        // closed both.
        if (windowsOn(1).some(w => parkedAddresses.indexOf(windowAddress(w)) < 0)) {
            parkedGesture = false;
            return;
        }
        const lines =parkedAddresses.map(address => 'dispatch hl.dsp.window.move({window = "address:' + address + '", workspace = 1, follow = false})');
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
        if (moving.concat(staying).some(isAndroidWindow)) {
            console.log("MOBILE_TILE refused: Android");
            return;
        }
        moving.forEach(w => windowAction(w, "hl.dsp.window.move", "workspace = " + target + ", follow = false"));
        overview.touchWindows(moving.concat(staying));
    }
    // Android apps never share a workspace (October 6). Waydroid sizes its
    // one display from its windows: two Android windows tiled in front shrank
    // it and grew it back, Android hung in the display change (no Android
    // window closed any more), and its watchdog's restart left it with no
    // network. Its hwcomposer now keeps the display's first size
    // (devices/pixel7pro/waydroid/patch-hwc-hotplug.py), so a tiled Android
    // window would only show cut off, and the shell keeps them apart anyway.
    // Waydroid opens an app's window by itself, on the workspace in front,
    // when that app comes to the front of Android: a new Android window stays
    // in front, unless the shell has just launched an app. Since October 7
    // no two windows share a workspace (separateWindows below).
    function windowClass(window) {
        return String((window.wayland && window.wayland.appId)
                      || (window.lastIpcObject && window.lastIpcObject.class)
                      || openedClasses[windowAddress(window)] || "");
    }
    function isAndroidWindow(window) {
        const id = windowClass(window);
        return id.startsWith("waydroid.") && id !== "waydroid.InputMethod";
    }
    property string launchedId: ""
    property double launchedAt: 0
    property var openedClasses: ({})
    property var openedAt: ({})
    function windowOpened(data) {
        // ADDRESS,WORKSPACE,CLASS,TITLE
        const parts = data.split(",");
        if (parts.length < 3) return;
        const address = "0x" + parts[0];
        Object.keys(openedAt).forEach(key => {
            if (Date.now() - openedAt[key] > 60000) { delete openedAt[key]; delete openedClasses[key]; }
        });
        openedClasses[address] = parts[2];
        openedAt[address] = Date.now();
        // Android's blank sleep screen maps out of sight without focus, and
        // Hyprland then focused the window focused before, at home the
        // Android app just parked: its workspace became the active one, and
        // the next app opened there. Workspace 1 stays the foreground.
        if (parts[2] === "waydroid.org.omarchy.mobile.keyboard") dispatch("hl.dsp.focus({workspace = 1})");
        separateCheck.restart();
    }
    Timer {
        id: separateCheck
        // Closed windows' last frames come and go in tens of ms.
        interval: 400
        property int waits: 0
        onTriggered: root.separateWindows()
    }
    // One app on each workspace (October 7): the phone shows one app at a
    // time, and windows put beside another were tiled with it: an app opened
    // over the session's terminal, Calculator and a terminal parked together,
    // an Android app opened into a quarter of the screen (Android then kept
    // that size). Every window that opens or is moved beside another goes to
    // a free workspace of its own; floating windows (dialogs) stay with their
    // app. Tiling is for later (WorkspaceOverview.tiling).
    function isFloating(window) {
        return !!(window.lastIpcObject && window.lastIpcObject.floating);
    }
    function separateWindows() {
        Hyprland.refreshToplevels();
        const used = {};
        Hyprland.toplevels.values.forEach(w => { if (w.workspace) used[w.workspace.id] = true; });
        Object.keys(used).forEach(key => {
            const id = Number(key);
            // Special workspaces (negative) hold windows kept out of sight.
            if (id < 1) return;
            const group = windowsOn(id).filter(w => windowClass(w) !== "waydroid.InputMethod" && !isFloating(w));
            if (group.length < 2) return;
            // Not while a switch or a home move is under way: they move
            // windows themselves.
            if ((focusRequest.running || homeRequest.running || openingCard) && separateCheck.waits < 10) {
                separateCheck.waits++;
                separateCheck.restart();
                return;
            }
            separateCheck.waits = 0;
            const age = w => Date.now() - (openedAt[windowAddress(w)] || 0);
            // The phone's own apps all have the class org.quickshell: the
            // one launched is the one that opened since. Matched by class
            // only, a Calculator opened over an Android app was moved away,
            // and the Android app stayed in front (October 7).
            const launched = id === 1 && Date.now() - launchedAt < 20000
                ? group.filter(w => windowClass(w) === launchedId
                               || (!launchedId.startsWith("waydroid.") && !isAndroidWindow(w)
                                   && (openedAt[windowAddress(w)] || 0) >= launchedAt)) : [];
            // Android opens a window only for the app in front of it (a
            // notification's action, a link from another app): that one stays.
            const androidNew = id === 1 ? group.filter(w => isAndroidWindow(w) && age(w) <= 3000)
                                                .sort((a, b) => age(a) - age(b)) : [];
            let staying;
            if (launched.length) staying = launched;
            else if (androidNew.length) staying = [androidNew[0]];
            else {
                // In front, a window that has just opened (an app's new
                // window); elsewhere, or with none, the one there first.
                const fresh = id === 1 ? group.filter(w => age(w) <= 3000).sort((a, b) => age(a) - age(b)) : [];
                staying = fresh.length ? [fresh[0]] : [group.slice().sort((a, b) => age(b) - age(a))[0]];
            }
            const moving = group.filter(w => staying.indexOf(w) < 0);
            // Each to a free workspace of its own.
            const lines = [];
            let slot = 1;
            const nextSlot = () => {
                while (++slot <= workspaceLimit) if (!used[slot]) { used[slot] = true; return slot; }
                return 0;
            };
            const place = (windows, target) => windows.forEach(w => {
                const address = windowAddress(w);
                if (address && target)
                    lines.push('dispatch hl.dsp.window.move({window = "address:' + address + '", workspace = ' + target + ', follow = false})');
            });
            moving.forEach(w => place([w], nextSlot()));
            if (!lines.length) return;
            if (id === 1) {
                const first = windowAddress(staying[0]);
                if (first) lines.push('dispatch hl.dsp.focus({window="address:' + first + '"})');
            }
            overview.touchWindows(group);
            console.log("MOBILE_SEPARATE " + id + " " + moving.map(windowClass).join(" "));
            Quickshell.execDetached(["hyprctl", "--batch", lines.join("; ")]);
        });
    }
    function showPage(name) {
        shade.close();
        if (page === name && motion.progress > 0) closeDrawer();
        else { page = name; keyboard("hide"); motion.animateTo(1); }
    }
    // The browser and web apps keep one window, whose app ID is their desktop
    // entry's: a second launch brings it forward, as does the browser itself
    // when another app hands it a link (the request FIFO's "focus ID").
    // Android apps too (waydroid.PACKAGE): launching one again through
    // Waydroid left its parked window tiled beside the app in front.
    function focusApp(id) {
        if (!/^(omarchy-mobile-(browser|webapp-[a-z0-9-]+)|waydroid\.[A-Za-z0-9_.]+)$/.test(id)) return false;
        const open = Hyprland.toplevels.values.find(w => (w.lastIpcObject && w.lastIpcObject.class === id)
                                                         || (w.wayland && w.wayland.appId === id));
        if (!open) return false;
        closeDrawer();
        shade.close();
        focusWindow(open);
        return true;
    }
    function launch(app, from, icon) {
        if (focusApp(app.id)) return;
        const command = app.runInTerminal ? ["kitty", "--"].concat(app.command) : app.command;
        // From another surface (the home screen's), through scene coordinates:
        // both surfaces cover the same area.
        const origin = !from ? null : from.Window.window === launchZoom.Window.window
            ? from.mapToItem(launchZoom, 0, 0) : launchZoom.mapFromItem(null, from.mapToItem(null, 0, 0));
        const glyph = (app.name || "A").substring(0, 1);
        closeDrawer();
        if (origin) launchZoom.beginAt(origin.x, origin.y, from.width, from.height, icon || "", glyph);
        Quickshell.execDetached({ command: command, workingDirectory: app.workingDirectory || Quickshell.env("HOME") });
        launchedId = app.id;
        launchedAt = Date.now();
        if (app.id.startsWith("waydroid.")) androidLaunching.restart();
        console.log("MOBILE_LAUNCH " + app.id);
    }
    // An app opened for another ("over APP [ARG]" on the request FIFO: the
    // browser's file picker opening the camera): it covers the app in front,
    // which is parked as when going home, and that app comes back when the
    // opened one closes itself while in front, as Android returns to the
    // activity that asked. Left any other way (home, the switcher), nothing
    // is brought back.
    property var overReturn: []
    property string overApp: ""
    property string overArgument: ""
    function launchOver(id, argument) {
        if (!/^[a-z0-9][a-z0-9-]*$/.test(id) || argument.length > 200) return;
        closeDrawer();
        shade.close();
        keyboard("hide");
        overApp = id;
        overArgument = argument;
        const park = goHome();
        if (park === "stay") { startupError = "Close an app before opening another"; return; }
        overReturn = park ? parkedAddresses.slice() : [];
        if (park) {
            overPreparation.command = park;
            overPreparation.running = true;
        } else startOver();
    }
    function startOver() {
        Quickshell.execDetached([Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-app", "launch", overApp]
                                .concat(overArgument ? [overArgument] : []));
        console.log("MOBILE_LAUNCH_OVER " + overApp);
        overWatch.window = "";
        overWatch.inFront = false;
        overWatch.ticks = 0;
        overWatch.restart();
    }
    Process {
        id: overPreparation
        onExited: (code, status) => { if (code === 0) root.startOver(); else root.overReturn = []; }
    }
    Timer {
        id: overWatch
        property string window: ""
        property bool inFront: false
        property int ticks: 0
        interval: 400
        repeat: true
        onTriggered: {
            if (!root.overReturn.length) { stop(); return; }
            if (!window) {
                // Its window: the first on the foreground workspace (20 s at most).
                const opened = root.windowsOn(1)[0];
                if (opened) { window = root.windowAddress(opened); inFront = true; }
                else if (++ticks > 50) { stop(); root.overReturn = []; }
                // A start can come to nothing (quickshell -n while the app's
                // last instance is still closing): once more after 3 s.
                else if (ticks === 8) {
                    Quickshell.execDetached([Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-app", "launch", root.overApp]
                                            .concat(root.overArgument ? [root.overArgument] : []));
                    console.log("MOBILE_LAUNCH_OVER again " + root.overApp);
                }
                return;
            }
            const still = Hyprland.toplevels.values.find(w => root.windowAddress(w) === window);
            if (still) { inFront = still.workspace && still.workspace.id === 1; return; }
            stop();
            const back = Hyprland.toplevels.values.find(w => root.overReturn.indexOf(root.windowAddress(w)) >= 0);
            root.overReturn = [];
            if (inFront && back && root.windowsOn(1).length === 0) root.focusWindow(back);
        }
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
        // A share request kept by omarchy-mobile-share (its ID).
        function share(id: string): void { shareSheet.show(id); }
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
        function ambient(on: bool): void { root.ambientRequest(on); }
        // After a minute wake of a phone asleep under the always-on clock.
        function ambientTick(): void { ambientDisplay.tick(); }
        function controlState(): string { return JSON.stringify({open: shade.opened, detail: shade.detail, notifications: shade.notificationCount, wifi: shade.wifi.connected, networks: shade.networks.length}); }
        function detail(name: string): void { if (["wifi", "battery", "calendar", "weather", "stats", "clipboard", "speedtest"].indexOf(name) >= 0) shade.showDetail(name); }
        function terminal(): void { root.terminal(); }
        function lock(): void { root.closeDrawer(); shade.close(); lockScreen.lockNow(); }
        // For deploys: an installed file is a new inode, which Quickshell no
        // longer watches, so nothing reloads on its own.
        function reload(): void { Quickshell.reload(false); }
        // The PIN or password page of a locked screen (it does not unlock).
        function lockKeypad(): void { if (lockScreen.locked) lockScreen.bouncer = true; }
        function lockState(): string { return JSON.stringify({locked: lockScreen.locked, credential: lockScreen.credential, service: lockScreen.serviceUp, fingers: lockScreen.fingers.length, auth: lockScreen.authRunning, bouncer: lockScreen.bouncer}); }
        function powerMenu(): void { root.closeDrawer(); shade.close(); systemPowerMenu.show(); }
        function phone(): void { root.openPhone(); }
        // Power and volume keys silence a ringing call, as on Android.
        function callSilence(): bool { const ringing = phoneCalls.ringing; phoneCalls.silence(); return ringing; }
        function callState(): string { return JSON.stringify({calls: phoneCalls.calls, ringing: phoneCalls.ringing}); }
        function keyboard(): void { root.keyboard("toggle"); }
        function volume(action: string): void { root.volumeKey(action); }
        function screenshot(): void { root.screenshot(0); }
        function record(): void { root.toggleRecording(); }
        function crt(action: string, token: string): string { return root.crtRequest(action, token); }
        function screenshotPrivacy(enabled: bool): void { shade.closeDetail(); shade.screenshotPrivacy = enabled; }
        function workspace(number: int): void { if (number > 0 && number < 100) root.selectWorkspace(number); }
        function inputState(): string { return JSON.stringify(root.inputState()); }
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
    PowerMenu {
        id: systemPowerMenu
        recording: root.screenRecording
        onOpenedChanged: if (opened) screenStatus.running = true
        // Once the menu has faded, so neither is of the menu.
        onScreenshotRequested: root.screenshot(500)
        onRecordRequested: {
            if (root.screenRecording) root.toggleRecording();
            else recordLater.restart();
        }
    }
    Timer { id: recordLater; interval: 500; onTriggered: root.toggleRecording() }
    ShareSheet { id: shareSheet }
    RotateButton {}
    AmbientDisplay {
        id: ambientDisplay
        notifications: shade.notificationValues
        // The fingerprint sensor's mark, when a finger can unlock from here.
        fingerprint: root.ambientTouch && lockScreen.locked && lockScreen.useFinger
            ? {sensor: lockScreen.sensor, panel: lockScreen.panel} : null
        onFingerDown: (x, y) => lockScreen.ambientFinger(x, y)
        onFingerUp: lockScreen.ambientFingerUp()
        weather: root.ambientWeather ? shade.weather : null
    }
    // The weather under the always-on clock (prefs ambientWeather): fetched as
    // the screen goes to it (omarchy-mobile-weather keeps 15 minutes), and
    // while the phone sleeps under it once an hour old, at the background
    // check (omarchy-mobile-sleep writes the check file with the network up),
    // never a wake of its own.
    readonly property bool ambientWeather: root.prefsData.ambientWeather === true && root.prefsData.alwaysOn === true
    FileView {
        path: Quickshell.env("XDG_RUNTIME_DIR") + "/omarchy-mobile-check"
        watchChanges: true
        printErrors: false
        onFileChanged: {
            if (!root.ambientWeather || root.screenLit) return;
            if (Date.now() / 1000 - (shade.weather.fetched || 0) > 3600) shade.fetchWeather("background", {});
        }
    }
    // The display's requests (omarchy-mobile-display), from IPC or the
    // request FIFO.
    function ambientRequest(on) {
        if (on) { root.closeDrawer(); shade.close(); }
        if (on && root.ambientWeather) shade.fetchWeather("status", {});
        MobileStatus.setAmbient(on);
    }
    function crtRequest(action, token) {
        // The screen going dark locks it (when a PIN or password is set),
        // once the close has played: locking first would show the lock
        // screen for a moment before the close.
        if (action === "off") { root.screenLit = false; root.closeDrawer(); shade.close(); root.lockPending = true; lockFallback.restart(); }
        else if (action === "on" || action === "open") { root.screenLit = true; root.screenDark = false; }
        if (action === "cover") { crtPower.cover(token); return "covered"; }
        if (action === "open") { crtPower.open(token); return "opened"; }
        if (action === "off" || action === "on") { crtPower.play(action, token); return "playing"; }
        return "ignored";
    }
    // omarchy-mobile-display writes its requests here, one line each ("crt
    // MODE TOKEN", "ambient true|false"): quickshell ipc starts a process of
    // its own, about 0.4 s a call on the little cores, which are all that run
    // while the screen is dark. One reader only: a reload does not stop the
    // old generation's, which would take every other request to nowhere, so
    // each reader stops the one before it (its PID in FIFO.reader). It holds
    // the FIFO open for writing too, so it never reads an end of file.
    readonly property string requestFifo: Quickshell.env("XDG_RUNTIME_DIR") + "/omarchy-mobile-shell.fifo"
    Process {
        id: requests
        command: ["sh", "-c", "[ -p \"$1\" ] || { rm -f \"$1\"; mkfifo -m 600 \"$1\"; }; "
                  + "old=$(cat \"$1.reader\" 2>/dev/null); [ \"$(cat /proc/$old/comm 2>/dev/null)\" = cat ] && kill \"$old\"; "
                  + "echo $$ > \"$1.reader\"; exec cat <> \"$1\"",
                  "requests", root.requestFifo]
        running: true
        stdout: SplitParser {
            onRead: line => {
                const word = line.trim().split(/\s+/);
                if (word[0] === "crt" && word.length === 3) root.crtRequest(word[1], word[2]);
                else if (word[0] === "ambient" && word.length === 2) root.ambientRequest(word[1] === "true");
                else if (word[0] === "focus" && word.length === 2) root.focusApp(word[1]);
                // The argument comes percent-encoded (it may hold spaces).
                else if (word[0] === "over" && word.length >= 2 && word.length <= 3) {
                    let argument = "";
                    try { argument = decodeURIComponent(word[2] || ""); } catch (e) { return; }
                    root.launchOver(word[1], argument);
                }
                else if (word[0] === "share" && word.length === 2) shareSheet.show(word[1]);
            }
        }
        onExited: requestsRetry.start()
    }
    Timer { id: requestsRetry; interval: 1000; onTriggered: requests.running = true }
    PowerKey {
        lit: root.screenLit
        ambient: MobileStatus.ambient
        alwaysOn: root.prefsData.alwaysOn === true
        ringing: phoneCalls.ringing || alarms.ringing
        onSilence: { if (phoneCalls.ringing) phoneCalls.silence(); else alarms.snooze(); }
        onPowerMenu: { root.closeDrawer(); shade.close(); systemPowerMenu.show(); }
        onVolume: action => root.volumeKey(action)
        onScreenshot: root.screenshot(0)
    }
    // A volume key: it silences a ringing call and snoozes a ringing alarm,
    // as on Android, else it changes the volume.
    function volumeKey(action) {
        if (phoneCalls.ringing && action !== "up") phoneCalls.silence();
        else if (alarms.ringing) alarms.snooze();
        else volumeOsd.adjust(action);
    }

    // Lit, as far as the shell knows: the screen animation tells it (crt).
    property bool screenLit: true
    // Dark from the end of a close to the next open: the lock screen draws
    // black then. Not from the close's request, which plays the screen as it
    // was: blanking it first showed black, then the lock screen again in the
    // close.
    property bool screenDark: false
    property bool lockPending: false
    function lockAfterClose() {
        if (!root.lockPending) return;
        root.lockPending = false;
        root.screenDark = true;
        lockScreen.lockNow();
    }
    Connections {
        target: crtPower
        // After the cover has drawn black (a few frames).
        function onFinished(id) { if (root.lockPending) lockAfterCover.restart(); }
    }
    Timer { id: lockAfterCover; interval: 80; onTriggered: root.lockAfterClose() }
    // Locks anyway if the close never reports.
    Timer { id: lockFallback; interval: 1500; onTriggered: root.lockAfterClose() }
    // The fingerprint sensor in the always-on display (Settings → Security,
    // off by default): the touch controller then stays on under the clock
    // (about 20 mW on the Pixel), which the device's display hook reads from
    // this file.
    readonly property bool ambientTouch: root.prefsData.fingerprintAmbient === true && lockScreen.enabled
        && lockScreen.fingers.length > 0
    FileView {
        id: ambientTouchFlag
        path: Quickshell.env("XDG_RUNTIME_DIR") + "/omarchy-mobile-ambient-touch"
        printErrors: false
        blockLoading: true
    }
    onAmbientTouchChanged: ambientTouchFlag.setText(root.ambientTouch ? "1\n" : "0\n")
    Component.onCompleted: {
        ambientTouchFlag.setText(root.ambientTouch ? "1\n" : "0\n");
        pullForward.restart();
        // This shell starts lit: a display left dark or always-on by the
        // shell before it (a crash, a restart, a reload) follows.
        Quickshell.execDetached([Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-display", "resync"]);
    }
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
    // Every helper the shell keeps running starts through setpriv
    // --pdeathsig TERM: the kernel stops it when the shell goes, however it
    // goes. A shell that died with Hyprland, or was killed, left its helpers
    // running (October 8: eleven sets of the phone, messages and media
    // watchers and their monitors, from one evening's session restarts, used
    // about a tenth of a core all day).
    Process {
        id: clipboardWatch
        command: ["setpriv", "--pdeathsig", "TERM", "wl-paste", "--watch", Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-clipboard", "ingest"]
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
    // Calendar reminders (omarchy-mobile-calendar watch): notifications at
    // their times; the sleep policy reads the next one and wakes for it.
    Process {
        id: calendarWatch
        command: ["setpriv", "--pdeathsig", "TERM", Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-calendar", "watch"]
        running: true
        onExited: calendarWatchRetry.start()
    }
    Timer { id: calendarWatchRetry; interval: 5000; onTriggered: calendarWatch.running = true }
    // Alarms and timers (omarchy-mobile-clock): they ring over everything.
    Alarms {
        id: alarms
        inCall: phoneCalls.ongoing !== null || phoneCalls.incoming !== null
        onArrived: { root.closeDrawer(); shade.close(); root.keyboard("hide"); toast.dismiss(); }
        onBuzz: MobileStatus.control(["vibrate", "700", "100"])
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
    // IdleMonitor keeps the timeout it started with; a restart takes a new one.
    onScreenTimeoutChanged: { root.idleRestarting = true; idleRestart.restart(); }
    property bool idleRestarting: false
    IdleMonitor {
        id: screenIdle
        enabled: root.screenTimeout > 0 && phoneCalls.calls.length === 0 && !alarms.ringing && !root.idleRestarting
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
        command: ["setpriv", "--pdeathsig", "TERM", Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-sleep", "run"]
        running: true
        // Always again: 3 is another copy holding the lock, which may end
        // too (October 8: a retry that met the lock stopped the retries, and
        // the phone would not have slept until the shell restarted).
        onExited: (code, status) => {
            sleepPolicyRetry.interval = code === 3 ? 60000 : 10000;
            sleepPolicyRetry.start();
        }
    }
    Timer { id: sleepPolicyRetry; interval: 10000; onTriggered: sleepPolicy.running = true }
    // What Android plays, as an MPRIS player for the media card
    // (omarchy-mobile-android-media; it waits on a file, no wakeups).
    Process {
        id: androidMedia
        command: ["setpriv", "--pdeathsig", "TERM", Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-android-media", "run"]
        running: true
        onExited: androidMediaRetry.start()
    }
    Timer { id: androidMediaRetry; interval: 30000; onTriggered: androidMedia.running = true }
    // The cameras as webcams for the browser and Android apps
    // (omarchy-camera-bridge on the device's v4l2loopback nodes, when it has
    // them): each waits for an app to capture before it turns its camera on.
    Process {
        id: frontCameraBridge
        command: ["setpriv", "--pdeathsig", "TERM", Quickshell.env("HOME") + "/.local/bin/omarchy-camera-bridge", "/dev/video20", "front"]
        running: true
        onExited: frontCameraBridgeRetry.start()
    }
    Timer { id: frontCameraBridgeRetry; interval: 60000; onTriggered: frontCameraBridge.running = true }
    Process {
        id: backCameraBridge
        command: ["setpriv", "--pdeathsig", "TERM", Quickshell.env("HOME") + "/.local/bin/omarchy-camera-bridge", "/dev/video21", "back"]
        running: true
        onExited: backCameraBridgeRetry.start()
    }
    Timer { id: backCameraBridgeRetry; interval: 60000; onTriggered: backCameraBridge.running = true }
    // Texts: the watcher files new texts from the modem and notifies; the
    // unread count is kept for the status bar.
    property int unreadTexts: 0
    Process {
        id: textWatch
        command: ["setpriv", "--pdeathsig", "TERM", Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-messages", "watch"]
        running: true
        stdout: SplitParser {
            onRead: line => {
                try {
                    const data = JSON.parse(line);
                    if (data.ok) root.unreadTexts = data.unread || 0;
                    else console.warn("MOBILE_TEXTS_ERROR " + data.error);
                } catch (e) {}
            }
        }
        // Exit 3: no ModemManager, nothing to follow. Logged: on October 2
        // it was restarted every few seconds for two hours, cause unseen.
        onExited: (code, status) => {
            if (code !== 3) { console.warn("MOBILE_TEXTS_EXIT " + code); textWatchRetry.start(); }
        }
    }
    Timer { id: textWatchRetry; interval: 5000; onTriggered: textWatch.running = true }
    NotificationToast {
        id: toast
        blocked: MobileStatus.dnd || shade.opened || shade.screenshotPrivacy || MobileStatus.ambient
        onActivate: notification => { if (!root.openNotification(notification)) shade.open(); }
        onReplyRequested: notification => shade.startReply(notification)
    }

    NotificationShade {
        id: shade
        topInset: statusBar.height
        onOpening: { root.closeDrawer(); root.keyboard("hide"); toast.dismiss(); }
        onArrived: notification => {
            toast.offer(notification);
            phoneLink.offer(notification);
            // A buzz as on Android: not in Do Not Disturb or Silent, and not
            // for low-urgency notifications.
            if (!MobileStatus.dnd && MobileStatus.controls.haptics === true && root.sliderPosition !== "silent"
                    && notification.urgency !== NotificationUrgency.Low)
                MobileStatus.control(["vibrate", "150", "70"]);
        }
        onKeyboardRequested: action => root.keyboard(action)
        onVolumeRequested: action => { volumeOsd.adjust(action); MobileStatus.refresh(); }
        onSettingsRequested: panel => { shade.close(); root.openSettings(panel); }
        onAppRequested: entry => {
            const app = root.apps.find(item => item.id === entry || item.id === entry + ".desktop");
            if (app) root.launch(app);
        }
        onNotificationOpened: notification => root.openNotification(notification)
        onAndroidReplyRequested: (notification, text) => root.androidReply(notification, text)
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
            // A screen recording in progress: tap to stop it.
            Rectangle {
                visible: root.screenRecording
                implicitWidth: recordChip.implicitWidth + 18
                implicitHeight: 24
                radius: MobileTheme.radius(12)
                color: MobileTheme.danger
                Text { id: recordChip; anchors.centerIn: parent; font.family: MobileTheme.fontFamily; text: "\u{F044A} REC"; color: MobileTheme.background; font.pixelSize: 13; font.bold: true }
                TapHandler { onTapped: root.toggleRecording() }
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
            if (event.name === "openwindow") root.windowOpened(event.data);
            else if (event.name === "movewindowv2") separateCheck.restart();
            if ((event.name === "openlayer" || event.name === "closelayer")
                    && (event.data === "wvkbd" || event.data === "omarchy-mobile-keyboard")) {
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
                            if (layer.namespace === "wvkbd" || layer.namespace === "omarchy-mobile-keyboard") found = layer;
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
                // Android's input method closes on Back, and the phone's
                // keyboard follows it (androidWantsKeyboard). Hidden alone,
                // it left Android's keyboard space empty under the app, and a
                // tap on the same field did not bring the keys back.
                else if (root.androidWantsKeyboard) {
                    root.androidBack();
                    console.log("MOBILE_KEYBOARD_DISMISS android");
                } else root.keyboard("hide");
            }
        }
    }
    // Back from either side edge, as on Android. At home Back has nothing to
    // do, and the edges belong to page swipes.
    readonly property bool backEnabled: page === "" && !shade.opened && launchZoom.t < 0.01 && !atHome
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
        // On the phone's Hyprland (patch 0003) the strip is where a back swipe
        // may start, as on Android: a touch there goes to the app under it,
        // and comes here only once it moves inward like a swipe (the app gets
        // a touch cancel), so taps and scrolls at the edge reach the app. A
        // compositor without the patch gives the strip every touch in it.
        mask: Region {
            // The region widens as the finger lands (LeftBack.down), before a
            // drag is even recognized, so the resting strip is only the edge:
            // at 64 px it took taps meant for apps (a calendar's Sundays).
            width: root.backEnabled ? ((backSwipe.down || backSwipe.pressed || backSwipe.tracking) ? backEdge.width : 40) : 0
            height: backEdge.height
        }
        LeftBack {
            id: backSwipe
            anchors.fill: parent
            pill: MobileTheme.surface
            ink: MobileTheme.foreground
            enabled: root.backEnabled
            onCommitted: root.requestBack()
        }
    }
    // The same from the right edge (layer omarchy-mobile-back-right, which
    // Hyprland patch 0003 knows too).
    PanelWindow {
        id: backEdgeRight
        anchors { right: true; top: true; bottom: true }
        implicitWidth: 200
        color: "transparent"
        exclusionMode: ExclusionMode.Ignore
        exclusiveZone: 0
        WlrLayershell.namespace: "omarchy-mobile-back-right"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.None
        mask: Region {
            readonly property real strip: root.backEnabled
                ? ((backSwipeRight.down || backSwipeRight.pressed || backSwipeRight.tracking) ? backEdgeRight.width : 40) : 0
            x: backEdgeRight.width - strip
            width: strip
            height: backEdgeRight.height
        }
        LeftBack {
            id: backSwipeRight
            fromRight: true
            anchors.fill: parent
            pill: MobileTheme.surface
            ink: MobileTheme.foreground
            enabled: root.backEnabled
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
            id: navEdge
            handleColor: MobileTheme.foreground
            anchors.fill: parent
            // Without Hyprland patch 0003 only: with it, taps on the strip go
            // to the app below (the shade's Keyboard tile shows the keyboard).
            onTapped: if (root.page === "" && !root.atHome) root.keyboard("show")
            onStarted: root.edgeStarted()
            onMoved: (distance, velocity) => motion.update(distance, velocity)
            onHeld: root.edgeHeld()
            onReleased: (distance, velocity, held) => root.edgeReleased(distance, velocity, held)
            onCanceled: if (motion.dragging) motion.cancel()
        }
    }

    // The home screen (HomeScreen.qml) on its own surface on the Bottom
    // layer: over the wallpaper, under the controls shade and the drawer at
    // every step of their motion. It is gone while an app is in front, and
    // takes touches only at home. (It shared the drawer's surface first, above
    // the shade, which then opened behind the widgets.)
    PanelWindow {
        id: homeLayer
        // Kept mapped, as the drawer: a surface mapped again comes back as a
        // small buffer until it is configured.
        visible: true
        anchors { top: true; left: true; right: true; bottom: true }
        exclusionMode: ExclusionMode.Normal
        exclusiveZone: 0
        color: "transparent"
        mask: Region {
            readonly property bool open: (root.atHome && root.page === "" && !shade.opened) || homeDrag.active
            width: open ? homeLayer.width : 0
            height: open ? homeLayer.height : 0
        }
        WlrLayershell.namespace: "omarchy-mobile-home"
        WlrLayershell.layer: WlrLayer.Bottom
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.None
        HomeScreen {
            id: home
            anchors.fill: parent
            apps: root.apps
            shown: root.atHome && (root.page === "" || root.page === "apps")
            visible: shown
            opacity: root.page === "apps" ? 1 - motion.progress : 1
            scale: root.page === "apps" ? 1 - 0.06 * motion.progress : 1
            active: shown && root.page === "" && root.screenLit && !shade.opened
            weather: shade.weather
            onLaunchApp: (app, from, icon) => root.launch(app, from, icon)
            onWeatherWanted: if (Date.now() / 1000 - (shade.weather.fetched || 0) > 1800) shade.fetchWeather("status", {})
            onWeatherOpened: shade.showDetail("weather")
        }
        // Up opens the drawer, down the controls shade.
        DragHandler {
            id: homeDrag
            target: null
            enabled: homeDrag.active || (root.atHome && root.page === "" && launchZoom.t < 0.01 && !shade.opened && !home.editing)
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
    }

    PanelWindow {
        id: drawer
        // Keep the surface mapped. Hiding it lets the next open come back as a
        // tiny top-left buffer before the layer is configured to full screen.
        visible: true
        mask: Region {
            // Home's own touches go to the home surface below (homeLayer).
            width: root.page !== "" || launchZoom.t > 0.01 || homeDrag.active || root.clearing ? drawer.width : 0
            height: root.page !== "" || launchZoom.t > 0.01 || homeDrag.active || root.clearing ? drawer.height : 0
        }
        anchors { top: true; left: true; right: true; bottom: true }
        exclusionMode: ExclusionMode.Normal
        exclusiveZone: 0
        color: "transparent"
        WlrLayershell.namespace: "omarchy-mobile-drawer"
        WlrLayershell.layer: WlrLayer.Top
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.None
        Rectangle { anchors.fill: parent; color: "black"; opacity: root.page === "spaces" ? motion.progress * 0.45 : motion.progress * 0.28 }
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
                    clip: true; contentHeight: contents.height
                    // Android 12's stretch at the ends. A drag past an end
                    // stretches the list away from it: the content stays at
                    // the bound and verticalOvershoot reports the pull. A
                    // fling that reaches an end stretches it by the speed
                    // left at the bound, and the list springs back.
                    boundsBehavior: Flickable.DragOverBounds; boundsMovement: Flickable.StopAtBounds
                    property real absorb: 0
                    property real absorbPeak: 0
                    property bool absorbAtTop: false
                    readonly property real stretch: Math.max(absorb, 0.08 * (1 - Math.exp(-Math.abs(verticalOvershoot) / 250)))
                    // Each fling in the log (speed, distance, time) for tuning.
                    property real flickFrom: 0
                    property real flickSpeed: 0
                    property double flickAt: 0
                    onFlickStarted: { flickFrom = contentY; flickSpeed = verticalVelocity; flickAt = Date.now(); console.log("MOBILE_FLICK start v=" + Math.round(verticalVelocity) + " y=" + Math.round(contentY) + " of " + Math.round(contentHeight - height)); }
                    onFlickEnded: {
                        console.log("MOBILE_FLICK end moved=" + Math.round(contentY - flickFrom) + " ms=" + (Date.now() - flickAt) + " y=" + Math.round(contentY));
                        // Constant deceleration: v² - 2·a·distance is left at the bound.
                        const speed = Math.min(Math.abs(flickSpeed), maximumFlickVelocity);
                        const left = speed * speed - 2 * flickDeceleration * Math.abs(contentY - flickFrom);
                        if (left > 0 && (atYBeginning || atYEnd)) {
                            absorbAtTop = flickSpeed < 0;
                            absorbPeak = 0.06 * Math.min(1, Math.sqrt(left) / 6000);
                            absorbAnimation.restart();
                        }
                    }
                    SequentialAnimation {
                        id: absorbAnimation
                        NumberAnimation { target: appList; property: "absorb"; to: appList.absorbPeak; duration: 90; easing.type: Easing.OutQuad }
                        NumberAnimation { target: appList; property: "absorb"; to: 0; duration: 380; easing.type: Easing.OutCubic }
                    }
                    Column {
                        id: contents; width: parent.width; spacing: 22
                        transform: Scale {
                            origin.y: (appList.absorb > 0 ? appList.absorbAtTop : appList.verticalOvershoot < 0) ? 0 : contents.height
                            yScale: 1 + appList.stretch
                        }
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
                                    AppIcon {
                                        id: appTile
                                        anchors.horizontalCenter: parent.horizontalCenter; width: 66; height: 66
                                        icon: modelData.icon || ""; name: modelData.name || ""; pressed: appTap.pressed
                                    }
                                    Text { font.family: MobileTheme.fontFamily; anchors { top: parent.top; topMargin: 80; left: parent.left; right: parent.right } text: modelData.name; textFormat: Text.PlainText; color: MobileTheme.foreground; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap; maximumLineCount: 2; elide: Text.ElideRight }
                                    // A long press puts the app on the home screen.
                                    TapHandler {
                                        id: appTap
                                        longPressThreshold: 0.5
                                        onTapped: root.launch(modelData, appTile, appTile.found ? appTile.source : "")
                                        onLongPressed: { home.addApp(modelData.id); root.closeDrawer(); }
                                    }
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
                giveUp.restart();
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
            // An app that never shows a new window here (one that hands its
            // request to a running copy, or fails to start) would otherwise
            // leave the cover up for good, over every app, taking the touches.
            Timer { id: giveUp; interval: 6000; onTriggered: launchZoom.finish() }
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
    // Black under the session lock while it comes up. From the request to
    // lock until the lock surface has drawn, Hyprland draws the desktop
    // without the layers let above the lock, so the CRT's parked black
    // vanished for a few frames and the wallpaper showed through (a flash
    // after the close). This cover is not above the lock, so it is drawn
    // then; clear while the CRT plays, black once it is parked.
    PanelWindow {
        id: crtCover
        visible: crtPower.mode !== ""
        anchors { top: true; bottom: true; left: true; right: true }
        exclusionMode: ExclusionMode.Ignore
        exclusiveZone: 0
        color: crtPower.mode === "parked" ? "black" : "transparent"
        mask: Region {}
        WlrLayershell.namespace: "omarchy-mobile-crt-cover"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.None
    }
    CrtPower {
        id: crtPower
        onOpened: { root.idleRestarting = true; idleRestart.restart(); }
    }
    // Touches held by something that shows nothing: the launch cover faded
    // out but not reset, a drawer page at rest closed, or a drag whose lift
    // was lost. Each leaves the open app untouchable or the home swipe dead
    // (October 4: the terminal in front, "stuck behind", cleared only by a
    // reload). Two checks a second apart in that state log it and clear it.
    Timer {
        id: stuckCheck
        interval: 1000
        repeat: true
        running: true
        property int strikes: 0
        onTriggered: {
            const cover = launchZoom.t > 0.01 && launchZoom.fade < 0.05 && !growAnim.running && !fadeAnim.running;
            const page = root.page !== "" && motion.progress < 0.01 && !motion.moving && !navEdge.dragActive && !homeDrag.active;
            const clearingLeft = root.clearing && !clearFrame.running;
            // A home-screen drag with an app in front, or an edge drag still for
            // 10 s: a finger whose lift never came. The home drag also opens
            // the drawer's touch area over the whole screen. Qt keeps such a
            // drag active even when its handler is disabled; a reload, which
            // cleared it on October 4, makes new handlers.
            const home = homeDrag.active && !root.atHome && !motion.moving;
            const edge = navEdge.dragActive && Date.now() - navEdge.lastMove > 10000;
            if (!cover && !page && !clearingLeft && !home && !edge) { strikes = 0; return; }
            if (++strikes < 2) return;
            strikes = 0;
            console.warn("MOBILE_STUCK " + JSON.stringify(root.inputState()));
            if (cover) { fadeAnim.stop(); growAnim.stop(); launchZoom.t = 0; launchZoom.fade = 1; }
            if (page) root.closeDrawer();
            if (clearingLeft) root.clearing = false;
            if (home || edge) Quickshell.reload(false);
        }
    }
    // The link to a paired computer: notifications, texts, ringing, theme.
    // New children go at the end: the shell's reload pairs them by position.
    LinkAdapter {
        id: phoneLink
        notifications: shade.notificationValues
    }
    // Screenshots and screen recordings (omarchy-mobile-screen): Power +
    // Volume Down (PowerKey) and the power menu take them.
    property bool screenRecording: false
    function screenshot(delay) {
        Quickshell.execDetached([Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-screen", "shot", String(delay || 0)]);
        if (!MobileStatus.dnd && MobileStatus.controls.haptics === true)
            MobileStatus.control(["vibrate", "40", "60"]);
    }
    function toggleRecording() {
        Quickshell.execDetached([Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-screen", "record", "toggle"]);
        screenStatusLater.restart();
    }
    Timer { id: screenStatusLater; interval: 1500; onTriggered: screenStatus.running = true }
    // A recording also ends from its notification's Stop or its time limit.
    Timer { interval: 3000; repeat: true; running: root.screenRecording; onTriggered: screenStatus.running = true }
    Process {
        id: screenStatus
        command: [Quickshell.env("HOME") + "/.local/bin/omarchy-mobile-screen", "record", "status"]
        stdout: StdioCollector {
            onStreamFinished: { try { root.screenRecording = JSON.parse(this.text).recording === true; } catch (e) {} }
        }
    }
}
