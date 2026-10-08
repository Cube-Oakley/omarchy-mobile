import QtQuick
import Quickshell
import Quickshell.Io

// The phone's end of the Omarchy link (overlay/link): runs omarchy-link,
// hands it the shell's notifications, and does what a paired computer asks:
// dismiss a notification, press one of its buttons, or ring the phone.
// Pairing prompts come from the daemon as ordinary notifications.
Item {
    id: link
    property var notifications: []
    property var state: ({})
    readonly property bool linked: (state.devices || []).some(d => d.paired && d.reachable)
    readonly property string linkedName: ((state.devices || []).find(d => d.paired && d.reachable) || {}).name || ""
    property bool serviceUp: false
    property var service: null
    property bool ringing: false
    property string tone: ""
    // Notification ID -> when it was first handed over, so a reconnect
    // resends the same times.
    property var offered: ({})
    function bin(name) { return Quickshell.env("HOME") + "/.local/bin/" + name; }

    function send(command) {
        if (!service || !serviceUp) return;
        service.write(JSON.stringify(command) + "\n");
        service.flush();
    }
    function plain(text) {
        return String(text || "").replace(/<[^>]*>/g, "").replace(/&lt;/g, "<").replace(/&gt;/g, ">")
            .replace(/&quot;/g, "\"").replace(/&#39;/g, "'").replace(/&amp;/g, "&");
    }
    function offer(notification) {
        const key = String(notification.id);
        if (!(key in offered)) {
            offered[key] = Date.now();
            notification.closed.connect(() => {
                delete link.offered[key];
                link.send({ cmd: "shell.closed", id: key });
            });
        }
        const actions = [];
        for (let i = 0; i < notification.actions.length; i++)
            actions.push({ id: notification.actions[i].identifier, label: notification.actions[i].text });
        send({ cmd: "shell.notification", id: key, appName: notification.appName || "", title: plain(notification.summary),
               text: plain(notification.body), time: offered[key], clearable: true, actions: actions });
    }
    function find(id) {
        for (let i = 0; i < notifications.length; i++)
            if (String(notifications[i].id) === id) return notifications[i];
        return null;
    }
    function handle(line) {
        let event;
        try { event = JSON.parse(line); } catch (e) { return; }
        if (event.event === "state") {
            state = event;
        } else if (event.event === "shell.dismiss") {
            const notification = find(event.id);
            if (notification) notification.dismiss();
        } else if (event.event === "shell.action") {
            const notification = find(event.id);
            if (!notification) return;
            for (let i = 0; i < notification.actions.length; i++)
                if (notification.actions[i].identifier === event.action) notification.actions[i].invoke();
        } else if (event.event === "shell.ring") {
            ringing = true;
            ringLimit.restart();
            Quickshell.execDetached([bin("omarchy-mobile-display"), "wake"]);
        } else if (event.event === "shell.ringStop") {
            ringing = false;
        }
    }
    function reconnect() {
        if (service) service.destroy();
        service = socketComponent.createObject(link);
    }

    // The daemon: restarted if it ends. A second copy started by hand makes
    // this one exit at once ("already running"); the socket still reaches it.
    Process {
        id: daemon
        command: ["sh", "-c", "exec \"$0\" daemon >>\"$1\" 2>&1", link.bin("omarchy-link"),
                  (Quickshell.env("XDG_STATE_HOME") || Quickshell.env("HOME") + "/.local/state") + "/omarchy-mobile/link.log"]
        environment: ({ QT_FORCE_STDERR_LOGGING: "1" })
        running: true
        onExited: daemonRetry.start()
    }
    Timer { id: daemonRetry; interval: 15000; onTriggered: daemon.running = true }

    Component {
        id: socketComponent
        Socket {
            path: (Quickshell.env("XDG_RUNTIME_DIR") || "/run/user/" + Quickshell.env("UID")) + "/omarchy-link.sock"
            connected: true
            parser: SplitParser { onRead: line => link.handle(line) }
            onConnectedChanged: {
                link.serviceUp = connected;
                if (!connected) return;
                link.send({ cmd: "shell.hello" });
                for (let i = 0; i < link.notifications.length; i++) link.offer(link.notifications[i]);
            }
        }
    }
    Timer {
        interval: 3000
        repeat: true
        triggeredOnStart: true
        running: !link.serviceUp
        onTriggered: link.reconnect()
    }

    // Ringing: the alarm tone and a buzz, until Stop, or for a minute.
    Process {
        command: [link.bin("omarchy-mobile-clock"), "tone"]
        running: true
        stdout: StdioCollector {
            onStreamFinished: { try { link.tone = JSON.parse(this.text).path || ""; } catch (e) {} }
        }
    }
    Process {
        id: ringer
        command: ["pw-play", "--media-role", "Alarm", link.tone]
        onExited: if (link.ringing) ringAgain.start()
    }
    Timer { id: ringAgain; interval: 10; onTriggered: if (link.ringing) ringer.running = true }
    Timer { id: ringLimit; interval: 60000; onTriggered: link.ringing = false }
    Timer {
        interval: 1600; repeat: true; triggeredOnStart: true
        running: link.ringing
        onTriggered: MobileStatus.control(["vibrate", "600", "90"])
    }
    onRingingChanged: {
        if (ringing && tone) ringer.running = true;
        if (!ringing) {
            ringer.running = false;
            send({ cmd: "shell.ringStopped" });
        }
    }
}
