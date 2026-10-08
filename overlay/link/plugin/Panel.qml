import QtQuick
import QtQuick.Controls
import Quickshell
import qs.Commons
import qs.Ui

// The phone in the bar. The icon dims while the phone is away (asleep or
// off the network) and takes the accent for unread texts or a pairing
// request. The panel: pairing, the phone's battery, its texts (a thread
// view with a reply box), its notifications, and the link's switches.
Panel {
  id: root
  moduleName: "omarchy-mobile.link"
  ipcTarget: "omarchy-mobile.link"

  readonly property color foreground: bar ? bar.foreground : Color.foreground
  readonly property color urgent: bar ? bar.urgent : Color.urgent
  readonly property color accent: Color.accent
  readonly property color dim: Qt.darker(foreground, 1.55)
  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family
  readonly property var phone: link.phone
  readonly property int unread: phone && phone.unreadTexts ? phone.unreadTexts : 0
  property bool composing: false

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  LinkClient { id: link }

  function batteryText(device) {
    if (!device || !device.battery || device.battery.charge < 0) return ""
    return device.battery.charge + "%" + (device.battery.charging ? " charging" : "")
  }

  function status() {
    if (!link.online) return "The link is not running"
    if (!phone) return link.nearby.length > 0 ? "Pair a phone to start" : "No phone paired"
    if (!phone.reachable) return "Away · asleep or off the network"
    var parts = ["Connected"]
    var battery = batteryText(phone)
    if (battery) parts.push(battery)
    return parts.join(" · ")
  }

  function relativeTime(ms) {
    if (!ms) return ""
    var delta = Math.max(0, Date.now() - ms)
    var minutes = Math.floor(delta / 60000)
    if (minutes < 1) return "now"
    if (minutes < 60) return minutes + "m"
    var hours = Math.floor(minutes / 60)
    if (hours < 24) return hours + "h"
    var days = Math.floor(hours / 24)
    if (days < 7) return days + "d"
    return Qt.formatDate(new Date(ms), "MMM d")
  }

  function nameOf(message) {
    return (message && message.omarchy && message.omarchy.name) || link.threadAddress(message) || "Unknown"
  }

  function unreadIn(message) {
    return message && message.omarchy ? (message.omarchy.unread || 0) : (message && message.read === 0 ? 1 : 0)
  }

  function startCompose() {
    link.closeThread()
    composing = true
    Qt.callLater(function() { toField.forceActiveFocus() })
  }

  function openConversation(message) {
    composing = false
    link.openThread(message)
    Qt.callLater(function() { reply.forceActiveFocus() })
  }

  function back() {
    composing = false
    link.closeThread()
    Qt.callLater(function() { keyCatcher.forceActiveFocus() })
  }

  function submit() {
    var text = reply.text
    if (text.trim() === "") return
    var address = composing ? toField.text.trim() : link.threadAddress(link.thread)
    if (address === "") {
      toField.forceActiveFocus()
      return
    }
    link.sendText(address, text)
    reply.text = ""
    if (composing) {
      // The conversation appears in the list once the phone has it.
      composing = false
      toField.text = ""
    }
  }

  onOpenedChanged: {
    if (opened) {
      link.refresh()
      Qt.callLater(function() { keyCatcher.forceActiveFocus() })
    } else {
      back()
    }
  }

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: "󰄜"
    opacity: !root.phone || root.phone.reachable ? 1 : 0.45
    active: root.unread > 0 || link.pairRequest !== null
    tooltipText: {
      if (link.pairRequest) return link.pairRequest.name + " wants to pair"
      if (!root.phone) return "Phone Link"
      var lines = [root.phone.name + " · " + root.status()]
      if (root.unread > 0) lines.push(root.unread + " unread " + (root.unread === 1 ? "text" : "texts"))
      if (root.phone.queuedTexts > 0) lines.push(root.phone.queuedTexts + " waiting to send")
      return lines.join("\n")
    }
    onPressed: function(buttonCode) {
      if (buttonCode === Qt.MiddleButton && root.phone) link.send({ cmd: "ring", device: root.phone.id })
      else root.toggle()
    }
  }

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: keyCatcher
    contentWidth: panel.fittedContentWidth(Style.space(400))
    contentHeight: link.thread || root.composing
      ? panel.cappedContentHeight(Style.space(560))
      : panel.fittedContentHeight(home.implicitHeight, Style.space(620))

    PanelKeyCatcher {
      id: keyCatcher
      anchors.fill: parent
      onCloseRequested: (link.thread || root.composing) ? root.back() : root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }
      onTextKey: function(t) {
        if (t === "n" && root.phone) root.startCompose()
        else if (t === "r") link.refresh()
      }

      // The overview.
      Flickable {
        id: homeFlick
        anchors.fill: parent
        visible: !link.thread && !root.composing
        contentWidth: width
        contentHeight: home.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Column {
          id: home
          width: homeFlick.width
          spacing: Style.space(10)

          PanelHero {
            width: parent.width
            title: root.phone ? root.phone.name : "Phone Link"
            meta: root.status()
            foreground: root.foreground
            fontFamily: root.fontFamily
            iconOpacity: !root.phone || root.phone.reachable ? 1 : 0.5
            iconComponent: Component {
              Text {
                text: "󰄜"
                color: root.foreground
                font.family: root.fontFamily
                font.pixelSize: Style.font.display
              }
            }
            trailingControl: Component {
              PanelActionButton {
                visible: !!root.phone
                iconText: "󰂚"
                tooltipText: "Ring the phone"
                foreground: root.foreground
                enabled: link.reachable
                onClicked: link.send({ cmd: "ring", device: root.phone.id })
              }
            }
          }

          Text {
            visible: link.lastError !== ""
            width: parent.width
            text: link.lastError
            color: root.urgent
            font.family: root.fontFamily
            font.pixelSize: Style.font.caption
            wrapMode: Text.WordWrap
          }

          // Someone asked to pair.
          Column {
            visible: link.pairRequest !== null
            width: parent.width
            spacing: Style.space(8)

            PanelSeparator { foreground: root.foreground }
            Text {
              width: parent.width
              text: link.pairRequest ? "Pair with " + link.pairRequest.name + "? Pair only if it shows this code:" : ""
              color: root.foreground
              font.family: root.fontFamily
              font.pixelSize: Style.font.body
              wrapMode: Text.WordWrap
            }
            Text {
              text: link.pairRequest ? link.pairRequest.code : ""
              color: root.accent
              font.family: root.fontFamily
              font.pixelSize: Style.font.title
              font.letterSpacing: 2
            }
            Row {
              spacing: Style.space(8)
              Button {
                text: "Pair"
                bordered: true
                foreground: root.foreground
                onClicked: link.send({ cmd: "accept", device: link.pairRequest.device })
              }
              Button {
                text: "Decline"
                foreground: root.foreground
                onClicked: link.send({ cmd: "reject", device: link.pairRequest.device })
              }
            }
          }

          // No phone yet: the devices on the network.
          Column {
            visible: !root.phone && link.online
            width: parent.width
            spacing: Style.space(6)

            PanelSeparator { foreground: root.foreground }
            PanelSectionHeader {
              width: parent.width
              text: "NEARBY"
              foreground: root.foreground
              fontFamily: root.fontFamily
            }
            Text {
              visible: link.nearby.length === 0
              width: parent.width
              text: "Nothing yet. The phone and this computer need to be on the same network (Wi-Fi, or the phone's USB cable), with the link running on both and its ports open in this computer's firewall."
              color: root.dim
              font.family: root.fontFamily
              font.pixelSize: Style.font.caption
              wrapMode: Text.WordWrap
            }
            Repeater {
              model: link.nearby
              Item {
                required property var modelData
                width: parent.width
                implicitHeight: Style.space(40)
                Text {
                  anchors.left: parent.left
                  anchors.right: pairButton.left
                  anchors.verticalCenter: parent.verticalCenter
                  text: modelData.pairing === "requested"
                    ? modelData.name + " · confirm " + modelData.code + " there"
                    : modelData.name + " · " + modelData.type
                  color: root.foreground
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.body
                  elide: Text.ElideRight
                }
                Button {
                  id: pairButton
                  anchors.right: parent.right
                  anchors.verticalCenter: parent.verticalCenter
                  text: modelData.pairing === "requested" ? "Cancel" : "Pair"
                  bordered: true
                  foreground: root.foreground
                  onClicked: link.send({ cmd: modelData.pairing === "requested" ? "reject" : "pair", device: modelData.id })
                }
              }
            }
          }

          // Texts.
          Column {
            visible: !!root.phone
            width: parent.width
            spacing: Style.space(4)

            PanelSeparator { foreground: root.foreground }
            Item {
              width: parent.width
              implicitHeight: Math.max(messagesHeader.implicitHeight, newButton.implicitHeight)
              PanelSectionHeader {
                id: messagesHeader
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: root.phone && root.phone.queuedTexts > 0 ? "MESSAGES · " + root.phone.queuedTexts + " WAITING" : "MESSAGES"
                foreground: root.foreground
                fontFamily: root.fontFamily
              }
              PanelActionButton {
                id: newButton
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                iconText: "󰏫"
                tooltipText: "New message"
                foreground: root.foreground
                onClicked: root.startCompose()
              }
            }
            Text {
              visible: link.conversations.length === 0
              width: parent.width
              topPadding: Style.space(6)
              text: link.reachable ? "No texts." : "Texts show here while the phone is connected."
              color: root.dim
              font.family: root.fontFamily
              font.pixelSize: Style.font.caption
            }
            Repeater {
              model: link.conversations
              ConversationRow {
                required property var modelData
                width: parent.width
                message: modelData
              }
            }
          }

          // The phone's notifications.
          Column {
            visible: !!root.phone && link.notes.length > 0
            width: parent.width
            spacing: Style.space(4)

            PanelSeparator { foreground: root.foreground }
            PanelSectionHeader {
              width: parent.width
              text: "ON THE PHONE"
              foreground: root.foreground
              fontFamily: root.fontFamily
            }
            Repeater {
              model: link.notes
              NoteRow {
                required property var modelData
                width: parent.width
                note: modelData
              }
            }
          }

          // Switches.
          Column {
            visible: link.online
            width: parent.width
            spacing: Style.space(4)

            PanelSeparator { foreground: root.foreground }
            PanelSectionHeader {
              width: parent.width
              text: "LINK"
              foreground: root.foreground
              fontFamily: root.fontFamily
            }
            Repeater {
              model: [
                { key: "notifications", label: "Phone notifications", description: "Shown here; dismissing one here dismisses it on the phone" },
                { key: "sms", label: "Texts", description: "Read and send the phone's texts from here" },
                { key: "theme.send", label: "Send my theme", description: "The phone follows when it is set to (it installs themes it lacks)" },
                { key: "theme.follow", label: "Follow the phone's theme", description: "Switch when the theme is changed on the phone" },
              ]
              Toggle {
                required property var modelData
                width: parent.width
                label: modelData.label
                description: modelData.description
                checked: link.features[modelData.key] === true
                foreground: root.foreground
                fontFamily: root.fontFamily
                onClicked: link.send({ cmd: "feature", name: modelData.key, on: !checked })
              }
            }
            Row {
              visible: !!root.phone
              spacing: Style.space(8)
              Button {
                text: "Send theme now"
                foreground: root.foreground
                enabled: link.reachable
                onClicked: link.send({ cmd: "theme.send" })
              }
              Button {
                text: "Unpair"
                foreground: root.foreground
                onClicked: link.send({ cmd: "unpair", device: root.phone.id })
              }
            }
          }
        }
      }

      // A conversation, or a new message.
      Item {
        anchors.fill: parent
        visible: !!link.thread || root.composing

        Item {
          id: threadHeader
          anchors.left: parent.left
          anchors.right: parent.right
          anchors.top: parent.top
          height: Math.max(backButton.implicitHeight, title.implicitHeight) + Style.space(6)

          PanelActionButton {
            id: backButton
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            iconText: "󰁍"
            tooltipText: "Back"
            foreground: root.foreground
            onClicked: root.back()
          }
          Text {
            id: title
            visible: !root.composing
            anchors.left: backButton.right
            anchors.leftMargin: Style.space(8)
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: link.thread ? root.nameOf(link.thread) : ""
            color: root.foreground
            font.family: root.fontFamily
            font.pixelSize: Style.font.title
            elide: Text.ElideRight
          }
          TextField {
            id: toField
            visible: root.composing
            anchors.left: backButton.right
            anchors.leftMargin: Style.space(8)
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            placeholderText: "To: phone number"
            foreground: root.foreground
            inputMethodHints: Qt.ImhDialableCharactersOnly
            Keys.onEscapePressed: root.back()
            onAccepted: reply.forceActiveFocus()
          }
        }

        ListView {
          id: messageList
          anchors.left: parent.left
          anchors.right: parent.right
          anchors.top: threadHeader.bottom
          anchors.bottom: composer.top
          anchors.topMargin: Style.space(6)
          anchors.bottomMargin: Style.space(6)
          clip: true
          spacing: Style.space(6)
          verticalLayoutDirection: ListView.BottomToTop
          model: root.composing ? [] : link.threadMessages.slice().reverse()
          ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
          delegate: Bubble {
            required property var modelData
            width: messageList.width
            message: modelData
          }

          Text {
            anchors.centerIn: parent
            visible: !root.composing && link.threadMessages.length === 0
            width: parent.width - Style.space(24)
            horizontalAlignment: Text.AlignHCenter
            text: !link.reachable ? "The phone is away; its texts show when it is connected. A text sent now goes when it is back."
                  : link.threadLoading ? "Loading…" : "No messages."
            color: root.dim
            font.family: root.fontFamily
            font.pixelSize: Style.font.caption
            wrapMode: Text.WordWrap
          }
        }

        Item {
          id: composer
          anchors.left: parent.left
          anchors.right: parent.right
          anchors.bottom: parent.bottom
          height: reply.implicitHeight

          TextField {
            id: reply
            anchors.left: parent.left
            anchors.right: sendButton.left
            anchors.rightMargin: Style.space(6)
            placeholderText: link.reachable ? "Text message" : "Text message (sent when the phone is back)"
            foreground: root.foreground
            Keys.onEscapePressed: root.back()
            onAccepted: root.submit()
          }
          PanelActionButton {
            id: sendButton
            anchors.right: parent.right
            anchors.verticalCenter: reply.verticalCenter
            iconText: "󰒊"
            tooltipText: "Send"
            foreground: root.foreground
            enabled: reply.text.trim() !== ""
            onClicked: root.submit()
          }
        }
      }
    }
  }

  component ConversationRow: Item {
    id: row
    property var message: null
    readonly property int unreadCount: root.unreadIn(message)
    implicitHeight: block.implicitHeight + Style.space(10)

    CursorSurface {
      anchors.fill: parent
      hasCursor: mouse.containsMouse
      radius: Style.cornerRadius
    }

    Column {
      id: block
      anchors.left: parent.left
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      anchors.leftMargin: Style.space(8)
      anchors.rightMargin: Style.space(8)
      spacing: Style.space(2)

      Item {
        width: parent.width
        implicitHeight: Math.max(name.implicitHeight, when.implicitHeight)
        Text {
          id: name
          anchors.left: parent.left
          anchors.right: when.left
          anchors.rightMargin: Style.space(8)
          text: root.nameOf(row.message)
          color: root.foreground
          font.family: root.fontFamily
          font.pixelSize: Style.font.body
          font.bold: row.unreadCount > 0
          elide: Text.ElideRight
        }
        Text {
          id: when
          anchors.right: parent.right
          text: (row.unreadCount > 0 ? "● " : "") + root.relativeTime(row.message ? row.message.date : 0)
          color: row.unreadCount > 0 ? root.accent : root.dim
          font.family: root.fontFamily
          font.pixelSize: Style.font.caption
        }
      }
      Text {
        width: parent.width
        text: row.message ? (row.message.type === 1 ? "" : "You: ") + String(row.message.body || "").replace(/\n/g, " ") : ""
        color: root.dim
        font.family: root.fontFamily
        font.pixelSize: Style.font.caption
        elide: Text.ElideRight
      }
    }

    MouseArea {
      id: mouse
      anchors.fill: parent
      hoverEnabled: true
      cursorShape: Qt.PointingHandCursor
      onClicked: root.openConversation(row.message)
    }
  }

  component NoteRow: Item {
    id: noteRow
    property var note: null
    implicitHeight: noteBlock.implicitHeight + Style.space(10)

    Column {
      id: noteBlock
      anchors.left: parent.left
      anchors.right: dismiss.left
      anchors.verticalCenter: parent.verticalCenter
      anchors.leftMargin: Style.space(8)
      spacing: Style.space(2)
      Text {
        width: parent.width
        text: noteRow.note ? noteRow.note.appName + " · " + noteRow.note.title : ""
        color: root.foreground
        font.family: root.fontFamily
        font.pixelSize: Style.font.body
        elide: Text.ElideRight
      }
      Text {
        width: parent.width
        visible: text !== ""
        text: noteRow.note ? String(noteRow.note.text || "").replace(/\n/g, " ") : ""
        color: root.dim
        font.family: root.fontFamily
        font.pixelSize: Style.font.caption
        elide: Text.ElideRight
        maximumLineCount: 2
        wrapMode: Text.WordWrap
      }
    }
    PanelActionButton {
      id: dismiss
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      visible: !!noteRow.note && noteRow.note.isClearable !== false
      iconText: "󰅖"
      tooltipText: "Dismiss on the phone"
      foreground: root.foreground
      enabled: link.reachable
      onClicked: link.send({ cmd: "notification.dismiss", device: root.phone.id, id: noteRow.note.id })
    }
  }

  component Bubble: Item {
    id: bubble
    property var message: null
    readonly property bool mine: !!message && message.type !== 1
    readonly property string sendState: message && message.omarchy ? (message.omarchy.state || "") : ""
    implicitHeight: card.height + (footnote.visible ? footnote.implicitHeight + Style.space(2) : 0)

    Rectangle {
      id: card
      anchors.right: bubble.mine ? parent.right : undefined
      anchors.left: bubble.mine ? undefined : parent.left
      width: Math.min(body.implicitWidth, bubble.width * 0.78) + Style.space(20)
      height: body.implicitHeight + Style.space(12)
      radius: Math.max(Style.cornerRadius, Style.space(8))
      color: bubble.mine ? Qt.alpha(root.accent, 0.22) : Qt.alpha(root.foreground, 0.08)

      Text {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: Style.space(10)
        anchors.rightMargin: Style.space(10)
        text: bubble.message ? String(bubble.message.body || "") : ""
        color: root.foreground
        font.family: root.fontFamily
        font.pixelSize: Style.font.body
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
      }
    }
    Text {
      id: footnote
      anchors.top: card.bottom
      anchors.topMargin: Style.space(2)
      anchors.right: bubble.mine ? parent.right : undefined
      anchors.left: bubble.mine ? undefined : parent.left
      visible: text !== ""
      text: bubble.sendState === "failed" ? "Not sent"
            : bubble.sendState === "waiting" ? "Sends when the phone is back"
            : bubble.sendState === "sending" ? "Sending…"
            : bubble.sendState === "delivered" ? "Delivered"
            : ""
      color: bubble.sendState === "failed" ? root.urgent : root.dim
      font.family: root.fontFamily
      font.pixelSize: Style.font.caption
    }
  }
}
