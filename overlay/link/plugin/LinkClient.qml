import QtQuick
import Quickshell
import Quickshell.Io

// The omarchy-link daemon, over its socket in the runtime directory: its
// state and events, and commands to it. Reconnects when the daemon restarts.
Item {
  id: root

  property bool active: true
  readonly property bool online: socketUp
  property var self: ({})
  property var features: ({})
  property var devices: []
  // The phone shown: the first paired device, else the first one seen.
  readonly property var phone: {
    for (var i = 0; i < devices.length; i++) if (devices[i].paired) return devices[i]
    return null
  }
  readonly property var nearby: devices.filter(function(d) { return !d.paired && d.reachable })
  readonly property bool reachable: !!(phone && phone.reachable)
  property var conversations: []
  property var notes: []
  property var thread: null        // the open conversation's newest message
  property var threadMessages: []
  property bool threadLoading: false
  property var pairRequest: null
  property string lastError: ""

  signal paired(string device)

  function send(command) {
    if (!socket || !socketUp) return false
    socket.write(JSON.stringify(command) + "\n")
    socket.flush()
    return true
  }

  function refresh() {
    send({ cmd: "state" })
    if (!phone) return
    send({ cmd: "sms.conversations", device: phone.id })
    send({ cmd: "notifications", device: phone.id })
  }

  function openThread(conversation) {
    thread = conversation
    threadMessages = []
    threadLoading = true
    if (phone) send({ cmd: "sms.thread", device: phone.id, thread: conversation.thread_id })
  }

  function closeThread() {
    thread = null
    threadMessages = []
    threadLoading = false
  }

  function sendText(address, text) {
    if (!phone) return
    lastError = ""
    send({ cmd: "sms.send", device: phone.id, address: address, text: text })
    // Shown at once; the phone's own copy replaces it when it comes back.
    if (thread && address === threadAddress(thread)) {
      var list = threadMessages.slice()
      list.push({ _id: "pending-" + Date.now(), body: text, type: 6, date: Date.now(), thread_id: thread.thread_id,
                  omarchy: { state: root.reachable ? "sending" : "waiting" } })
      threadMessages = list
    }
  }

  function threadAddress(message) {
    var addresses = message && message.addresses ? message.addresses : []
    return addresses.length > 0 ? addresses[0].address : ""
  }

  function handle(event) {
    if (event.event === "state") {
      self = event.self || {}
      features = event.features || {}
      var hadPhone = !!(phone && phone.reachable)
      devices = event.devices || []
      if (phone && phone.reachable && !hadPhone) {
        send({ cmd: "sms.conversations", device: phone.id })
        send({ cmd: "notifications", device: phone.id })
      }
      if (pairRequest && !devices.some(function(d) { return d.id === pairRequest.device && d.pairing === "incoming" }))
        pairRequest = null
    } else if (event.event === "sms.conversations") {
      if (phone && event.device === phone.id) conversations = event.conversations || []
    } else if (event.event === "sms.messages") {
      if (!thread || !phone || event.device !== phone.id) return
      if (event.threadID === thread.thread_id) {
        threadMessages = event.messages || []
        threadLoading = false
      } else if ((event.messages || []).some(function(m) { return m.thread_id === thread.thread_id })) {
        // A new or changed message in the open conversation: fetch it again.
        send({ cmd: "sms.thread", device: phone.id, thread: thread.thread_id })
      }
    } else if (event.event === "notifications") {
      if (phone && event.device === phone.id) notes = event.notifications || []
    } else if (event.event === "pairRequest") {
      pairRequest = event
    } else if (event.event === "paired") {
      pairRequest = null
      paired(event.device)
      refresh()
    } else if (event.event === "pairFailed") {
      pairRequest = null
      lastError = "Pairing failed: " + event.reason
    } else if (event.event === "sms.failed") {
      lastError = "Text not sent: " + event.error
    } else if (event.event === "theme.result") {
      lastError = event.error ? "The phone could not switch to " + event.name + ": " + event.error : ""
    } else if (event.reply && event.ok === false && event.error) {
      lastError = event.error
    }
  }

  // A new socket per attempt, as the lock screen does: one that failed
  // does not reliably connect again.
  property var socket: null
  property bool socketUp: false

  function reconnect() {
    if (socket) socket.destroy()
    socket = socketComponent.createObject(root)
  }

  Component {
    id: socketComponent
    Socket {
      path: (Quickshell.env("XDG_RUNTIME_DIR") || "/tmp") + "/omarchy-link.sock"
      connected: true
      onConnectedChanged: {
        root.socketUp = connected
        if (connected) {
          root.lastError = ""
          root.send({ cmd: "subscribe" })
          root.refresh()
        } else {
          root.devices = []
        }
      }
      parser: SplitParser {
        onRead: function(line) {
          try { root.handle(JSON.parse(line)) } catch (e) {}
        }
      }
    }
  }

  Timer {
    interval: 5000
    repeat: true
    triggeredOnStart: true
    running: root.active && !root.socketUp
    onTriggered: root.reconnect()
  }
}
