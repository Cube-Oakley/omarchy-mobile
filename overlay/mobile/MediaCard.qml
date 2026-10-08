import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Services.Mpris

// What is playing, from any MPRIS player (the browser, a music app, Android
// apps through a bridge): artwork, title and artist, a progress bar to tap
// for a position, and previous, play/pause and next. It follows the player
// that is playing, else the one that played last, and hides when none has
// a track. Tapping the artwork or the title opens the player.
Rectangle {
    id: card
    // Show only while visible to the user: the position is polled then.
    property bool active: true
    property bool compact: false
    signal openRequested(string desktopEntry)

    property var lastPlaying: null
    readonly property var players: Mpris.players.values
    readonly property var player: {
        let playing = null;
        for (const p of players) if (p.isPlaying) { playing = p; break; }
        if (playing) return playing;
        if (lastPlaying && players.indexOf(lastPlaying) >= 0 && lastPlaying.trackTitle) return lastPlaying;
        for (const p of players) if (p.trackTitle) return p;
        return null;
    }
    Connections {
        target: card.player
        function onIsPlayingChanged() { if (card.player && card.player.isPlaying) card.lastPlaying = card.player; }
    }
    onPlayerChanged: if (player && player.isPlaying) lastPlaying = player

    visible: player !== null
    implicitHeight: visible ? content.implicitHeight + 28 : 0
    radius: MobileTheme.radius(20)
    color: MobileTheme.surface
    border.width: 1
    border.color: Qt.rgba(MobileTheme.muted.r, MobileTheme.muted.g, MobileTheme.muted.b, 0.7)
    clip: true

    // MPRIS reports the position only when asked: once a second while
    // playing and on screen.
    property real position: 0
    Timer {
        interval: 1000
        repeat: true
        triggeredOnStart: true
        running: card.active && card.visible && card.player !== null && card.player.isPlaying && card.player.positionSupported
        onTriggered: { card.player.positionChanged(); card.position = card.player.position; }
    }
    Connections {
        target: card.player
        function onTrackChanged() { card.position = 0; }
        function onPositionChanged() { if (card.player) card.position = card.player.position; }
    }
    function time(seconds) {
        if (!(seconds >= 0) || !isFinite(seconds)) return "";
        const s = Math.floor(seconds);
        return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0");
    }
    function open() { if (player) openRequested(player.desktopEntry || ""); }

    ColumnLayout {
        id: content
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 14 }
        spacing: 10
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Rectangle {
                Layout.preferredWidth: card.compact ? 52 : 64
                Layout.preferredHeight: Layout.preferredWidth
                radius: MobileTheme.radius(12)
                color: MobileTheme.selection
                clip: true
                Image {
                    id: art
                    anchors.fill: parent
                    source: card.player ? card.player.trackArtUrl : ""
                    sourceSize: Qt.size(192, 192)
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: !source.toString().startsWith("image://")
                }
                Text {
                    anchors.centerIn: parent
                    visible: art.status !== Image.Ready
                    text: "\u{F075A}"
                    color: MobileTheme.accent
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: parent.height * 0.45
                }
                TapHandler { onTapped: card.open() }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text {
                    Layout.fillWidth: true
                    text: card.player ? (card.player.identity || "Media").toUpperCase() : ""
                    textFormat: Text.PlainText
                    color: MobileTheme.accent
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 11
                    font.letterSpacing: 1.2
                    font.bold: true
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    text: card.player ? (card.player.trackTitle || "Unknown title") : ""
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
                    text: card.player ? (card.player.trackArtist || card.player.trackAlbum || "") : ""
                    textFormat: Text.PlainText
                    color: MobileTheme.secondary
                    font.family: MobileTheme.fontFamily
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }
                TapHandler { onTapped: card.open() }
            }
        }
        // Progress: tap for a position (when the player can seek).
        Item {
            Layout.fillWidth: true
            implicitHeight: 26
            visible: card.player !== null && card.player.lengthSupported && card.player.length > 0
            readonly property real fraction: card.player && card.player.length > 0 ? Math.max(0, Math.min(1, card.position / card.player.length)) : 0
            Rectangle {
                id: track
                anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: 4 }
                height: 4
                radius: 2
                color: MobileTheme.selection
                Rectangle { width: parent.width * parent.parent.fraction; height: parent.height; radius: 2; color: MobileTheme.accent }
            }
            Text { anchors { left: parent.left; bottom: parent.bottom } text: card.time(card.position); color: MobileTheme.muted; font.family: MobileTheme.fontFamily; font.pixelSize: 11 }
            Text { anchors { right: parent.right; bottom: parent.bottom } text: card.player ? card.time(card.player.length) : ""; color: MobileTheme.muted; font.family: MobileTheme.fontFamily; font.pixelSize: 11 }
            TapHandler {
                enabled: card.player !== null && card.player.canSeek
                onTapped: point => {
                    const target = point.position.x / width * card.player.length;
                    card.player.position = target;
                    card.position = target;
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
            spacing: 18
            Item { Layout.fillWidth: true }
            MediaButton { glyph: "\u{F04AE}"; enabled: card.player !== null && card.player.canGoPrevious; onClicked: card.player.previous() }
            MediaButton {
                glyph: card.player && card.player.isPlaying ? "\u{F03E4}" : "\u{F040A}"
                primary: true
                enabled: card.player !== null && card.player.canTogglePlaying
                onClicked: card.player.togglePlaying()
            }
            MediaButton { glyph: "\u{F04AD}"; enabled: card.player !== null && card.player.canGoNext; onClicked: card.player.next() }
            Item { Layout.fillWidth: true }
        }
    }

    component MediaButton: Rectangle {
        id: button
        property string glyph: ""
        property bool primary: false
        signal clicked()
        implicitWidth: primary ? 58 : 46
        implicitHeight: implicitWidth
        radius: width / 2
        opacity: enabled ? 1 : 0.35
        color: primary ? (tap.pressed ? Qt.darker(MobileTheme.accent, 1.2) : MobileTheme.accent)
                       : (tap.pressed ? MobileTheme.selection : "transparent")
        Text {
            anchors.centerIn: parent
            text: button.glyph
            color: button.primary ? MobileTheme.background : MobileTheme.foreground
            font.family: MobileTheme.fontFamily
            font.pixelSize: button.primary ? 28 : 24
        }
        TapHandler { id: tap; enabled: button.enabled; onTapped: button.clicked() }
    }
}
