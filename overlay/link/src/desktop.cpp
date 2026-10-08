#include "desktop.h"

#include "config.h"

#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>

#include <algorithm>

namespace {

// NotificationClosed reasons: 2 is the user dismissing it.
constexpr uint ClosedByUser = 2;
constexpr int ThreadPage = 100;

QString outboxPath()
{
    return Config::get().dir + "/outbox.json";
}

} // namespace

Desktop::Desktop(Daemon *daemon)
    : Role(daemon, false)
{
    loadOutbox();
    Notifier *notifier = m_daemon->notifier();
    connect(notifier, &Notifier::actionInvoked, this, [this](uint id, const QString &key) {
        const auto posted = m_posted.value(id);
        Device *device = m_daemon->device(posted.first);
        if (!device || !key.startsWith("a:"))
            return;
        device->send("kdeconnect.notification.action", {{"key", posted.second}, {"action", key.mid(2)}});
    });
    connect(notifier, &Notifier::closed, this, [this](uint id, uint reason) {
        if (!m_posted.contains(id))
            return;
        const auto [deviceId, noteId] = m_posted.take(id);
        Device *device = m_daemon->device(deviceId);
        auto phone = m_phones.find(deviceId);
        if (reason != ClosedByUser || !device || phone == m_phones.end())
            return;
        // Dismissed here: dismissed on the phone too, when it allows that.
        const QJsonObject note = phone->notes.value(noteId);
        if (note.value("isClearable").toBool(true) && device->send("kdeconnect.notification.request", {{"cancel", noteId}})) {
            phone->notes.remove(noteId);
            m_daemon->broadcast({{"event", "notifications"}, {"device", deviceId}, {"notifications", notes(deviceId)}});
        }
    });
}

QStringList Desktop::incoming() const
{
    return {"kdeconnect.ping", "kdeconnect.battery", "kdeconnect.notification", "kdeconnect.sms.messages",
            "kdeconnect.telephony", "omarchy.sms.failed", "omarchy.theme", "omarchy.theme.request", "omarchy.theme.files",
            "omarchy.theme.chunk", "omarchy.theme.result"};
}

QStringList Desktop::outgoing() const
{
    return {"kdeconnect.ping", "kdeconnect.battery.request", "kdeconnect.notification.request",
            "kdeconnect.notification.action", "kdeconnect.sms.request", "kdeconnect.sms.request_conversations",
            "kdeconnect.sms.request_conversation", "kdeconnect.findmyphone.request", "omarchy.sms.read", "omarchy.theme",
            "omarchy.theme.request", "omarchy.theme.files", "omarchy.theme.chunk", "omarchy.theme.result"};
}

void Desktop::connected(Device *device)
{
    const Config &config = Config::get();
    if (config.feature("battery"))
        device->send("kdeconnect.battery.request", {{"request", true}});
    if (config.feature("notifications"))
        device->send("kdeconnect.notification.request", {{"request", true}});
    if (config.feature("sms")) {
        device->send("kdeconnect.sms.request_conversations");
        sendQueued(device);
    }
    sendTheme(device);
}

void Desktop::received(Device *device, const QString &type, const QJsonObject &body)
{
    if (themePacket(device, type, body))
        return;
    const Config &config = Config::get();
    if (type == "kdeconnect.battery" && config.feature("battery")) {
        m_phones[device->id].battery = {{"charge", body.value("currentCharge").toInt(-1)},
                                        {"charging", body.value("isCharging").toBool()}};
        m_daemon->stateChanged();
    } else if (type == "kdeconnect.notification" && config.feature("notifications")) {
        notification(device, body);
    } else if (type == "kdeconnect.sms.messages" && config.feature("sms")) {
        texts(device, body);
    } else if (type == "kdeconnect.telephony" && config.feature("notifications")) {
        telephony(device, body);
    } else if (type == "omarchy.sms.failed") {
        m_daemon->broadcast({{"event", "sms.failed"}, {"device", device->id}, {"error", body.value("error")}});
    }
}

void Desktop::notification(Device *device, const QJsonObject &body)
{
    Phone &phone = m_phones[device->id];
    const QString id = body.value("id").toString();
    uint posted = 0;
    for (auto it = m_posted.begin(); it != m_posted.end(); ++it)
        if (it.value().first == device->id && it.value().second == id)
            posted = it.key();
    if (body.value("isCancel").toBool()) {
        phone.notes.remove(id);
        if (posted) {
            m_posted.remove(posted);
            m_daemon->notifier()->close(posted);
        }
    } else {
        phone.notes.insert(id, body);
        // Ones already on the phone when it links arrive silent: listed, not
        // shown again.
        if (!body.value("silent").toBool() || posted) {
            QStringList actions;
            for (const QJsonValue &action : body.value("actions").toArray())
                actions << "a:" + action.toString() << action.toString();
            Notifier::Note note{body.value("appName").toString(), "phone", body.value("title").toString(),
                                body.value("text").toString().toHtmlEscaped(), actions,
                                {{"x-omarchy-link-device", device->id}}, -1, posted};
            const QString deviceId = device->id;
            m_daemon->notifier()->notify(note, [this, deviceId, id](uint desktopId) {
                if (desktopId)
                    m_posted.insert(desktopId, {deviceId, id});
            });
        }
    }
    m_daemon->broadcast({{"event", "notifications"}, {"device", device->id}, {"notifications", notes(device->id)}});
    m_daemon->stateChanged();
}

void Desktop::texts(Device *device, const QJsonObject &body)
{
    Phone &phone = m_phones[device->id];
    const QJsonArray messages = body.value("messages").toArray();
    const QJsonObject omarchy = body.value("omarchy").toObject();
    for (const QJsonValue &value : messages) {
        const QJsonObject message = value.toObject();
        const qint64 thread = qint64(message.value("thread_id").toDouble());
        const QJsonObject newest = phone.threads.value(thread);
        if (newest.isEmpty() || message.value("date").toDouble() >= newest.value("date").toDouble())
            phone.threads.insert(thread, message);
    }
    QJsonObject event{{"event", "sms.messages"}, {"device", device->id}, {"messages", messages}};
    // The answer to a thread request (Omarchy phones mark it; a KDE Connect
    // phone's answer is one thread's messages).
    if (omarchy.contains("threadID"))
        event.insert("threadID", omarchy.value("threadID"));
    m_daemon->broadcast(event);
    m_daemon->broadcast({{"event", "sms.conversations"}, {"device", device->id}, {"conversations", conversations(device->id)}});
    m_daemon->stateChanged();
}

void Desktop::telephony(Device *device, const QJsonObject &body)
{
    Phone &phone = m_phones[device->id];
    const QString event = body.value("event").toString();
    const QString who = body.value("contactName").toString().isEmpty() ? body.value("phoneNumber").toString()
                                                                         : body.value("contactName").toString();
    if (body.value("isCancel").toBool() || event == "talking") {
        m_daemon->notifier()->close(phone.call);
        phone.call = 0;
        return;
    }
    const QString summary = event == "ringing" ? "Call from " + who : event == "missedCall" ? "Missed call from " + who : QString();
    if (summary.isEmpty())
        return;
    const QString deviceId = device->id;
    m_daemon->notifier()->notify({device->name, "call-start", summary, {}, {}, {}, event == "ringing" ? 0 : -1, phone.call},
                                 [this, deviceId, event](uint id) {
                                     if (event == "ringing")
                                         m_phones[deviceId].call = id;
                                 });
}

bool Desktop::command(const QJsonObject &command, QJsonObject *reply)
{
    const QString cmd = command.value("cmd").toString();
    const QString deviceId = command.value("device").toString();
    Device *device = m_daemon->device(deviceId);
    auto linked = [device]() { return device && device->paired() && device->reachable(); };
    if (cmd == "sms.conversations") {
        if (linked())
            device->send("kdeconnect.sms.request_conversations");
        *reply = {{"event", "sms.conversations"}, {"device", deviceId}, {"conversations", conversations(deviceId)}};
        return true;
    }
    if (cmd == "sms.thread") {
        const double thread = command.value("thread").toDouble();
        if (!linked()) {
            *reply = {{"ok", false}, {"error", "The phone is not connected."}};
            return true;
        }
        device->send("kdeconnect.sms.request_conversation", {{"threadID", thread}, {"numberToRequest", ThreadPage}});
        if (command.value("markRead").toBool(true))
            device->send("omarchy.sms.read", {{"threadID", thread}});
        *reply = {{"ok", true}};
        return true;
    }
    if (cmd == "sms.send") {
        const QString address = command.value("address").toString().trimmed();
        const QString text = command.value("text").toString();
        if (!device || !device->paired() || address.isEmpty() || text.trimmed().isEmpty()) {
            *reply = {{"ok", false}, {"error", "Needs a paired phone, a number and some text."}};
            return true;
        }
        m_outbox.append(QJsonObject{{"device", deviceId}, {"address", address}, {"text", text},
                                    {"time", double(QDateTime::currentMSecsSinceEpoch())}});
        saveOutbox();
        // Held while the phone sleeps; it goes when the phone links again.
        if (linked())
            sendQueued(device);
        *reply = {{"ok", true}, {"queued", !linked()}};
        m_daemon->stateChanged();
        return true;
    }
    if (cmd == "notifications") {
        *reply = {{"event", "notifications"}, {"device", deviceId}, {"notifications", notes(deviceId)}};
        return true;
    }
    if (cmd == "notification.dismiss" || cmd == "notification.action") {
        const QString id = command.value("id").toString();
        if (!linked()) {
            *reply = {{"ok", false}, {"error", "The phone is not connected."}};
            return true;
        }
        if (cmd == "notification.action") {
            device->send("kdeconnect.notification.action", {{"key", id}, {"action", command.value("action").toString()}});
        } else {
            device->send("kdeconnect.notification.request", {{"cancel", id}});
            m_phones[deviceId].notes.remove(id);
            for (auto it = m_posted.begin(); it != m_posted.end(); ++it)
                if (it.value().first == deviceId && it.value().second == id) {
                    m_daemon->notifier()->close(it.key());
                    m_posted.erase(it);
                    break;
                }
            m_daemon->broadcast({{"event", "notifications"}, {"device", deviceId}, {"notifications", notes(deviceId)}});
            m_daemon->stateChanged();
        }
        *reply = {{"ok", true}};
        return true;
    }
    if (cmd == "ring") {
        *reply = linked() && device->send("kdeconnect.findmyphone.request")
            ? QJsonObject{{"ok", true}}
            : QJsonObject{{"ok", false}, {"error", "The phone is not connected."}};
        return true;
    }
    if (cmd == "theme.send") {
        for (Device *each : m_daemon->linked())
            sendTheme(each);
        *reply = {{"ok", true}};
        return true;
    }
    return false;
}

QJsonObject Desktop::deviceState(Device *device) const
{
    const Phone phone = m_phones.value(device->id);
    int unread = 0;
    for (const QJsonObject &message : phone.threads)
        unread += message.value("omarchy").toObject().value("unread").toInt();
    int queued = 0;
    for (const QJsonValue &text : m_outbox)
        queued += text.toObject().value("device") == device->id;
    QJsonObject state{{"notifications", int(phone.notes.size())}, {"unreadTexts", unread}, {"queuedTexts", queued}};
    if (!phone.battery.isEmpty())
        state.insert("battery", phone.battery);
    return state;
}

QJsonArray Desktop::conversations(const QString &device) const
{
    QList<QJsonObject> list = m_phones.value(device).threads.values();
    std::sort(list.begin(), list.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value("date").toDouble() > b.value("date").toDouble();
    });
    QJsonArray result;
    for (const QJsonObject &message : list)
        result.append(message);
    return result;
}

QJsonArray Desktop::notes(const QString &device) const
{
    QList<QJsonObject> list = m_phones.value(device).notes.values();
    std::sort(list.begin(), list.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value("time").toString().toLongLong() > b.value("time").toString().toLongLong();
    });
    QJsonArray result;
    for (const QJsonObject &note : list)
        result.append(note);
    return result;
}

void Desktop::sendQueued(Device *device)
{
    QJsonArray waiting;
    bool sent = false;
    for (const QJsonValue &value : std::as_const(m_outbox)) {
        const QJsonObject text = value.toObject();
        if (text.value("device") != device->id) {
            waiting.append(text);
            continue;
        }
        if (device->send("kdeconnect.sms.request", {{"version", 2},
                                                    {"addresses", QJsonArray{QJsonObject{{"address", text.value("address")}}}},
                                                    {"messageBody", text.value("text")}}))
            sent = true;
        else
            waiting.append(text);
    }
    m_outbox = waiting;
    saveOutbox();
    if (sent)
        m_daemon->stateChanged();
}

void Desktop::loadOutbox()
{
    QFile file(outboxPath());
    if (file.open(QIODevice::ReadOnly))
        m_outbox = QJsonDocument::fromJson(file.readAll()).array();
}

void Desktop::saveOutbox() const
{
    QSaveFile file(outboxPath());
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(m_outbox).toJson(QJsonDocument::Compact));
    file.commit();
}
