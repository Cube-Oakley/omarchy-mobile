import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Wayland

// The banner for a new notification: it drops in under the status bar, waits
// (a hairline counts it down; a touch holds it), then rises away. Tap to open
// the shade, swipe up to hide it (it stays in the shade), sideways to dismiss
// the notification, or use its actions.
PanelWindow {
    id: toast
    property var current: null
    property var queue: []
    property bool blocked: false
    // The card is in place (false while it drops in or rises away).
    property bool shown: false
    // The banner was tapped: the shell opens the notification's app.
    signal activate(var notification)
    // Reply was tapped on a notification that takes an inline reply: the
    // shade answers it (it has the field and the keyboard).
    signal replyRequested(var notification)
    // An Android app's notification (Waydroid's bridge) carries its reply
    // action as a plain button, which would send no text; Reply here goes
    // through the shade's field instead (keyboard-bridge's ReplyService).
    // The "default" action is the tap itself, not a button.
    function androidReply(n) {
        return !!n && String(n.desktopEntry || "").startsWith("waydroid.")
            && (n.actions || []).some(a => /^reply$/i.test(a.text));
    }
    function pills(n) {
        return n ? (n.actions || []).filter(a => a.identifier !== "default"
                                                 && !(androidReply(n) && /^reply$/i.test(a.text))) : [];
    }
    function offer(notification) {
        if (blocked || !notification) return;
        if (current) {
            queue = queue.concat([notification]).slice(-2);
            return;
        }
        show(notification);
    }
    function show(notification) {
        current = notification;
        card.offset = 0;
        shown = true;
        wait.restart();
    }
    function hide() {
        if (!current) return;
        wait.stop();
        shown = false;
        gone.restart();
    }
    // Once it has risen away: the next one, if any.
    function finish() {
        current = null;
        card.offset = 0;
        if (queue.length) {
            const next = queue[0];
            queue = queue.slice(1);
            Qt.callLater(() => toast.show(next));
        }
    }
    function dismiss() {
        wait.stop();
        gone.stop();
        shown = false;
        finish();
    }
    onBlockedChanged: if (blocked) dismiss()
    visible: current !== null && !blocked
    anchors { top: true; left: true; right: true }
    // Under the status bar (its reserved space already counts).
    margins { top: 6; left: 10; right: 10 }
    implicitHeight: card.implicitHeight + 16
    exclusiveZone: 0
    exclusionMode: ExclusionMode.Ignore
    color: "transparent"
    WlrLayershell.namespace: "omarchy-mobile-toast"
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.keyboardFocus: WlrKeyboardFocus.None

    readonly property string image: {
        const picture = current ? (current.image || "") : "";
        return picture.startsWith("/") ? "file://" + picture : picture;
    }

    Rectangle {
        id: card
        width: parent.width
        implicitHeight: content.implicitHeight + 28
        height: implicitHeight
        y: toast.shown ? 4 : -height - 16
        Behavior on y {
            NumberAnimation {
                duration: toast.shown ? 380 : 220
                easing.type: toast.shown ? Easing.OutBack : Easing.InCubic
                easing.overshoot: 1.1
            }
        }
        radius: MobileTheme.radius(22)
        color: MobileTheme.surface
        border.width: 1
        border.color: Qt.rgba(MobileTheme.muted.r, MobileTheme.muted.g, MobileTheme.muted.b, 0.8)
        clip: true
        property real offset: 0
        transform: Translate { x: card.offset }
        opacity: 1 - Math.min(0.75, Math.abs(offset) / width)

        // An accent bar down the left edge.
        Rectangle {
            width: 4
            height: parent.height - 28
            anchors.verticalCenter: parent.verticalCenter
            x: 0
            radius: 2
            color: MobileTheme.accent
            opacity: 0.85
        }

        ColumnLayout {
            id: content
            anchors { left: parent.left; right: parent.right; top: parent.top; leftMargin: 16; rightMargin: 14; topMargin: 14 }
            spacing: 10
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                NotificationIcon {
                    Layout.alignment: Qt.AlignTop
                    notification: toast.current
                    implicitWidth: 40
                    implicitHeight: 40
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Text {
                            text: toast.current ? (toast.current.appName || "Notification").toUpperCase() : ""
                            textFormat: Text.PlainText
                            color: MobileTheme.accent
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 11
                            font.letterSpacing: 1.2
                            font.bold: true
                            elide: Text.ElideRight
                            Layout.maximumWidth: content.width * 0.6
                        }
                        Text {
                            text: "· now"
                            color: MobileTheme.muted
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 11
                        }
                        Item { Layout.fillWidth: true }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: toast.current ? toast.current.summary : ""
                        textFormat: Text.PlainText
                        color: MobileTheme.foreground
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 16
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: toast.current ? toast.current.body : ""
                        textFormat: Text.PlainText
                        color: MobileTheme.secondary
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                        lineHeight: 1.1
                    }
                }
                // The notification's own picture (a photo, a contact).
                Rectangle {
                    visible: toast.image !== "" && picture.status === Image.Ready
                    Layout.alignment: Qt.AlignTop
                    implicitWidth: 52
                    implicitHeight: 52
                    radius: MobileTheme.radius(12)
                    color: MobileTheme.selection
                    clip: true
                    Image {
                        id: picture
                        anchors.fill: parent
                        source: toast.image
                        sourceSize: Qt.size(156, 156)
                        fillMode: Image.PreserveAspectCrop
                        // On the GUI thread, as NotificationIcon.qml's.
                        asynchronous: false
                    }
                }
            }
            // Its actions, as pills.
            Flow {
                id: toastActions
                Layout.fillWidth: true
                Layout.leftMargin: 52
                visible: toast.current && (toast.pills(toast.current).length > 0 || toast.current.hasInlineReply || toast.androidReply(toast.current))
                spacing: 8
                Rectangle {
                    visible: toast.current !== null && (toast.current.hasInlineReply || toast.androidReply(toast.current))
                    implicitWidth: replyLabel.implicitWidth + 28
                    implicitHeight: 36
                    radius: MobileTheme.radius(18)
                    color: replyTap.pressed ? Qt.darker(MobileTheme.accent, 1.2) : MobileTheme.accent
                    Text {
                        id: replyLabel
                        anchors.centerIn: parent
                        text: "Reply"
                        color: MobileTheme.background
                        font.family: MobileTheme.fontFamily
                        font.pixelSize: 13
                        font.bold: true
                    }
                    TapHandler {
                        id: replyTap
                        onTapped: { const n = toast.current; toast.dismiss(); toast.replyRequested(n); }
                    }
                }
                Repeater {
                    model: toast.pills(toast.current)
                    Rectangle {
                        required property var modelData
                        implicitWidth: actionLabel.implicitWidth + 28
                        implicitHeight: 36
                        radius: MobileTheme.radius(18)
                        color: actionTap.pressed ? MobileTheme.selection : "transparent"
                        border.width: 1
                        border.color: MobileTheme.accent
                        Text {
                            id: actionLabel
                            anchors.centerIn: parent
                            text: modelData.text
                            color: MobileTheme.accent
                            font.family: MobileTheme.fontFamily
                            font.pixelSize: 13
                            font.bold: true
                        }
                        TapHandler { id: actionTap; onTapped: { modelData.invoke(); toast.hide(); } }
                    }
                }
            }
        }

        // The time left, as a hairline that shortens.
        Rectangle {
            id: countdown
            anchors { left: parent.left; bottom: parent.bottom; leftMargin: 18; bottomMargin: 6 }
            height: 2
            radius: 1
            color: MobileTheme.accent
            opacity: 0.55
            width: (card.width - 36) * wait.left
        }

        TapHandler {
            onPressedChanged: {
                // A finger on the banner holds it.
                if (pressed) wait.stop();
                else if (toast.shown) wait.restart();
            }
            // Not when the tap was on a button (their own handlers act).
            onTapped: eventPoint => {
                const p = toastActions.mapFromItem(card, eventPoint.position.x, eventPoint.position.y);
                if (toastActions.visible && p.x >= 0 && p.y >= 0 && p.x <= toastActions.width && p.y <= toastActions.height) return;
                const n = toast.current;
                toast.dismiss();
                toast.activate(n);
            }
        }
        // Up hides the banner (the notification stays in the shade);
        // sideways, as on Android, dismisses the notification.
        DragHandler {
            id: swipe
            target: null
            onActiveTranslationChanged: {
                if (active && Math.abs(activeTranslation.x) > Math.abs(activeTranslation.y)) card.offset = activeTranslation.x;
            }
            onActiveChanged: {
                if (active) { wait.stop(); return; }
                const across = card.offset;
                if (Math.abs(across) > card.width / 4) {
                    leave.to = (across < 0 ? -1 : 1) * (card.width + 20);
                    leave.start();
                    return;
                }
                back.start();
                if (swipe.translation.y < -28) toast.hide();
                else wait.restart();
            }
        }
        NumberAnimation {
            id: leave
            target: card; property: "offset"; duration: 160; easing.type: Easing.OutCubic
            onFinished: { if (toast.current) toast.current.dismiss(); toast.dismiss(); }
        }
        NumberAnimation { id: back; target: card; property: "offset"; to: 0; duration: 220; easing.type: Easing.OutBack }
    }

    // How long a banner stays, counted for the hairline.
    NumberAnimation {
        id: wait
        target: wait; property: "left"
        property real left: 1
        from: 1; to: 0
        duration: 5000
        onFinished: toast.hide()
    }
    Timer { id: gone; interval: 240; onTriggered: toast.finish() }
}
