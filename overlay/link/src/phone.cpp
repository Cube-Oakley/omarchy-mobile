#include "phone.h"

#include "config.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QtEndian>

namespace {

QString readTrimmed(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
}

QString batteryDir()
{
    const QDir supplies("/sys/class/power_supply");
    for (const QString &name : supplies.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (readTrimmed(supplies.filePath(name + "/type")) == "Battery")
            return supplies.filePath(name);
    return {};
}

QString dataHome()
{
    const QString value = qEnvironmentVariable("XDG_DATA_HOME");
    return value.isEmpty() ? QDir::homePath() + "/.local/share" : value;
}

// KDE Connect's message types (Android's Telephony.Sms columns).
enum SmsType { Inbox = 1, Sent = 2, Failed = 5, Queued = 6 };

} // namespace

Phone::Phone(Daemon *daemon)
    : Role(daemon, true)
{
    m_batteryTimer.setInterval(60000);
    connect(&m_batteryTimer, &QTimer::timeout, this, [this]() {
        if (m_daemon->linked().isEmpty()) {
            m_batteryTimer.stop();
            return;
        }
        const QJsonObject now = battery();
        if (now != m_lastBattery)
            sendAll("kdeconnect.battery", now, "battery");
    });

    // Texts: the messages database changes when a text arrives, is sent or
    // is delivered.
    const QString texts = dataHome() + "/omarchy-mobile/messages";
    QDir().mkpath(texts);
    m_texts.addPath(texts);
    for (const QString &file : {texts + "/messages.db", texts + "/messages.db-wal"})
        if (QFileInfo::exists(file))
            m_texts.addPath(file);
    connect(m_daemon->notifier(), &Notifier::actionInvoked, this, [this](uint id, const QString &) {
        if (id && id == m_ringNote)
            stopRinging();
    });
    connect(m_daemon->notifier(), &Notifier::closed, this, [this](uint id, uint) {
        if (id && id == m_ringNote)
            stopRinging();
    });

    m_textsSettle.setSingleShot(true);
    m_textsSettle.setInterval(400);
    connect(&m_texts, &QFileSystemWatcher::directoryChanged, &m_textsSettle, qOverload<>(&QTimer::start));
    connect(&m_texts, &QFileSystemWatcher::fileChanged, &m_textsSettle, qOverload<>(&QTimer::start));
    connect(&m_textsSettle, &QTimer::timeout, this, [this, texts]() {
        for (const QString &file : {texts + "/messages.db", texts + "/messages.db-wal"})
            if (QFileInfo::exists(file) && !m_texts.files().contains(file))
                m_texts.addPath(file);
        textsChanged();
    });
    textsChanged();
}

QStringList Phone::incoming() const
{
    return {"kdeconnect.ping", "kdeconnect.battery.request", "kdeconnect.notification.request",
            "kdeconnect.notification.action", "kdeconnect.sms.request", "kdeconnect.sms.request_conversations",
            "kdeconnect.sms.request_conversation", "kdeconnect.findmyphone.request", "omarchy.sms.read",
            "omarchy.theme", "omarchy.theme.request", "omarchy.theme.files", "omarchy.theme.chunk", "omarchy.theme.result"};
}

QStringList Phone::outgoing() const
{
    return {"kdeconnect.ping", "kdeconnect.battery", "kdeconnect.notification", "kdeconnect.sms.messages",
            "kdeconnect.telephony", "omarchy.theme", "omarchy.theme.request", "omarchy.theme.files", "omarchy.theme.chunk",
            "omarchy.theme.result"};
}

void Phone::sendAll(const QString &type, const QJsonObject &body, const QString &feature)
{
    if (!Config::get().feature(feature))
        return;
    if (type == "kdeconnect.battery")
        m_lastBattery = body;
    for (Device *device : m_daemon->linked())
        device->send(type, body);
}

void Phone::connected(Device *device)
{
    if (Config::get().feature("battery")) {
        m_lastBattery = battery();
        device->send("kdeconnect.battery", m_lastBattery);
        m_batteryTimer.start();
    }
    sendTheme(device);
}

QJsonObject Phone::battery() const
{
    const QString dir = batteryDir();
    const int charge = readTrimmed(dir + "/capacity").toInt();
    const QString status = readTrimmed(dir + "/status");
    const bool charging = status == "Charging" || status == "Full";
    return {{"currentCharge", charge}, {"isCharging", charging}, {"thresholdEvent", !charging && charge <= 15 ? 1 : 0}};
}

void Phone::received(Device *device, const QString &type, const QJsonObject &body)
{
    if (themePacket(device, type, body))
        return;
    const Config &config = Config::get();
    if (type == "kdeconnect.battery.request" && config.feature("battery")) {
        device->send("kdeconnect.battery", battery());
    } else if (type == "kdeconnect.notification.request" && config.feature("notifications")) {
        if (body.contains("cancel")) {
            const QString id = body.value("cancel").toString();
            if (m_notes.contains(id))
                m_daemon->broadcast({{"event", "shell.dismiss"}, {"id", id}});
        } else if (body.value("request").toBool()) {
            for (const QJsonObject &note : std::as_const(m_notes))
                device->send("kdeconnect.notification", notificationPacket(note, true));
        }
    } else if (type == "kdeconnect.notification.action" && config.feature("notifications")) {
        const QJsonObject note = m_notes.value(body.value("key").toString());
        for (const QJsonValue &action : note.value("actions").toArray())
            if (action.toObject().value("label") == body.value("action"))
                m_daemon->broadcast({{"event", "shell.action"}, {"id", note.value("id")}, {"action", action.toObject().value("id")}});
    } else if (type == "kdeconnect.sms.request_conversations" && config.feature("sms")) {
        sendConversations(device);
    } else if (type == "kdeconnect.sms.request_conversation" && config.feature("sms")) {
        sendConversation(device, body);
    } else if (type == "kdeconnect.sms.request" && config.feature("sms")) {
        sendText(device, body);
    } else if (type == "omarchy.sms.read" && config.feature("sms")) {
        const QString key = m_threadKeys.value(qint64(body.value("threadID").toDouble()));
        if (!key.isEmpty())
            messages({"read", key}, [](const QJsonObject &) {});
    } else if (type == "kdeconnect.findmyphone.request" && config.feature("ring")) {
        // The shell rings until Stop is pressed or the notice is dismissed.
        m_daemon->broadcast({{"event", "shell.ring"}, {"from", device->name}});
        m_daemon->notifier()->notify({"Omarchy Link", "phone", device->name + " is looking for this phone",
                                      m_daemon->hasShell() ? "Ringing" : QString(), {"stop", "Stop"},
                                      {{"urgency", QVariant::fromValue(uchar(2))}}, 0, m_ringNote},
                                     [this](uint id) { m_ringNote = id; });
    }
}

bool Phone::command(const QJsonObject &command, QJsonObject *reply)
{
    const QString cmd = command.value("cmd").toString();
    if (cmd == "shell.notification") {
        // From the shell's notification server; the link's own prompts stay
        // on the phone.
        const QString id = command.value("id").toString();
        if (id.isEmpty() || command.value("appName").toString() == "Omarchy Link")
            return true;
        const bool fresh = !m_notes.contains(id);
        m_notes.insert(id, command);
        sendAll("kdeconnect.notification", notificationPacket(command, !fresh || command.value("silent").toBool()),
                "notifications");
        *reply = {{"ok", true}};
        return true;
    }
    if (cmd == "shell.closed") {
        const QString id = command.value("id").toString();
        if (m_notes.remove(id))
            sendAll("kdeconnect.notification", {{"id", id}, {"isCancel", true}}, "notifications");
        *reply = {{"ok", true}};
        return true;
    }
    if (cmd == "shell.call") {
        // The shell's incoming call: "ringing", then "missedCall" or a cancel.
        QJsonObject body{{"event", command.value("event").toString()},
                         {"phoneNumber", command.value("number").toString()},
                         {"contactName", command.value("name").toString()}};
        if (command.value("isCancel").toBool())
            body.insert("isCancel", true);
        sendAll("kdeconnect.telephony", body, "notifications");
        *reply = {{"ok", true}};
        return true;
    }
    if (cmd == "shell.ringStopped") {
        // Stopped on the phone (or after a minute): the notice goes too.
        const uint note = m_ringNote;
        m_ringNote = 0;
        m_daemon->notifier()->close(note);
        *reply = {{"ok", true}};
        return true;
    }
    if (cmd == "theme.send") {
        for (Device *device : m_daemon->linked())
            sendTheme(device);
        *reply = {{"ok", true}};
        return true;
    }
    return false;
}

QJsonObject Phone::notificationPacket(const QJsonObject &note, bool silent) const
{
    const QString title = note.value("title").toString();
    const QString text = note.value("text").toString();
    QJsonArray actions;
    for (const QJsonValue &action : note.value("actions").toArray())
        actions.append(action.toObject().value("label"));
    QJsonObject body{
        {"id", note.value("id")},
        {"appName", note.value("appName")},
        {"title", title},
        {"text", text},
        {"ticker", text.isEmpty() ? title : title + ": " + text},
        {"isClearable", note.value("clearable").toBool(true)},
        {"time", QString::number(qint64(note.value("time").toDouble()))},
        {"silent", silent},
    };
    if (!actions.isEmpty())
        body.insert("actions", actions);
    return body;
}

// Texts, through the phone's messages helper.

void Phone::messages(const QStringList &args, std::function<void(const QJsonObject &)> done)
{
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [process, done]() {
        const QByteArray out = process->readAllStandardOutput();
        process->deleteLater();
        QJsonObject result = QJsonDocument::fromJson(out.trimmed().split('\n').last()).object();
        if (result.isEmpty())
            result = {{"ok", false}, {"error", "no answer from omarchy-mobile-messages"}};
        done(result);
    });
    connect(process, &QProcess::errorOccurred, this, [process, done](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        process->deleteLater();
        done({{"ok", false}, {"error", "omarchy-mobile-messages is not installed"}});
    });
    process->start(QDir::homePath() + "/.local/bin/omarchy-mobile-messages", args);
}

qint64 Phone::threadId(const QString &key)
{
    // KDE Connect numbers conversations. A phone number's key is its last
    // ten digits, used as is; a sender name gets a number from its hash,
    // above any ten-digit one and below 2^53 so JSON keeps it exact.
    static const QRegularExpression digits(QStringLiteral("^[0-9]{1,10}$"));
    qint64 id;
    if (digits.match(key).hasMatch()) {
        id = key.toLongLong();
    } else {
        const QByteArray hash = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256);
        id = qint64(qFromBigEndian<quint64>(hash.constData()) % 1000000000000000ULL) + 1000000000000LL;
    }
    m_threadKeys.insert(id, key);
    return id;
}

QJsonObject Phone::smsMessage(const QJsonObject &row, const QJsonObject &person)
{
    const QString state = row.value("state").toString();
    int type = Sent;
    if (row.value("direction").toString() == "in")
        type = Inbox;
    else if (state == "failed")
        type = Failed;
    else if (state == "sending" || state == "queued")
        type = Queued;
    const QString key = row.value("thread").toString();
    QJsonObject omarchy{{"state", state}, {"thread", key}};
    const QJsonObject who = person.isEmpty() ? row : person;
    if (who.contains("name"))
        omarchy.insert("name", who.value("name"));
    if (row.contains("unread"))
        omarchy.insert("unread", row.value("unread"));
    return {
        {"event", 1},
        {"_id", row.value("id")},
        {"thread_id", double(threadId(key))},
        {"addresses", QJsonArray{QJsonObject{{"address", row.value("number")}}}},
        {"body", row.value("body")},
        {"date", double(qint64(row.value("time").toDouble() * 1000))},
        {"type", type},
        {"read", row.value("read").toBool() ? 1 : 0},
        {"sub_id", -1},
        {"omarchy", omarchy},
    };
}

void Phone::withThreads(std::function<void(const QJsonArray &)> done)
{
    messages({"threads"}, [this, done](const QJsonObject &result) {
        const QJsonArray threads = result.value("threads").toArray();
        for (const QJsonValue &thread : threads)
            threadId(thread.toObject().value("thread").toString());
        done(threads);
    });
}

void Phone::sendConversations(Device *device)
{
    QPointer<Device> target(device);
    withThreads([this, target](const QJsonArray &threads) {
        if (!target)
            return;
        QJsonArray list;
        for (const QJsonValue &thread : threads)
            list.append(smsMessage(thread.toObject()));
        target->send("kdeconnect.sms.messages", {{"messages", list}, {"version", 2}, {"omarchy", QJsonObject{{"conversations", true}}}});
    });
}

void Phone::sendConversation(Device *device, const QJsonObject &request)
{
    const qint64 id = qint64(request.value("threadID").toDouble());
    const qint64 before = qint64(request.value("rangeStartTimestamp").toDouble(-1));
    const int count = request.value("numberToRequest").toInt(100);
    QPointer<Device> target(device);
    auto fetch = [this, target, id, before, count]() {
        const QString key = m_threadKeys.value(id);
        if (key.isEmpty() || !target)
            return;
        // The helper gives the newest LIMIT messages; an older page needs
        // more of them, filtered here.
        const int limit = before > 0 ? 5000 : qBound(1, count, 5000);
        messages({"thread", key, QString::number(limit)}, [this, target, id, before, count](const QJsonObject &result) {
            if (!target)
                return;
            QJsonArray list;
            const QJsonArray rows = result.value("messages").toArray();
            for (const QJsonValue &row : rows)
                if (before <= 0 || qint64(row.toObject().value("time").toDouble() * 1000) < before)
                    list.append(smsMessage(row.toObject(), result));
            while (list.size() > count)
                list.removeFirst();
            target->send("kdeconnect.sms.messages", {{"messages", list}, {"version", 2},
                                                     {"omarchy", QJsonObject{{"threadID", double(id)}}}});
        });
    };
    if (m_threadKeys.contains(id))
        fetch();
    else
        withThreads([fetch](const QJsonArray &) { fetch(); });
}

void Phone::sendText(Device *device, const QJsonObject &request)
{
    QStringList numbers;
    for (const QJsonValue &address : request.value("addresses").toArray())
        numbers << address.toObject().value("address").toString();
    if (numbers.isEmpty() && request.contains("phoneNumber"))
        numbers << request.value("phoneNumber").toString();
    const QString text = request.value("messageBody").toString();
    QPointer<Device> target(device);
    auto refuse = [this, target](const QString &why) {
        m_daemon->notifier()->notify({"Omarchy Link", "phone", "A text from the computer was not sent", why, {}, {}, -1, 0});
        if (target)
            target->send("omarchy.sms.failed", {{"error", why}});
    };
    if (numbers.size() != 1 || numbers.first().isEmpty() || text.isEmpty()) {
        refuse(numbers.size() > 1 ? "Group texts are not supported yet." : "It had no number or no text.");
        return;
    }
    messages({"send", numbers.first(), text}, [refuse](const QJsonObject &result) {
        if (!result.value("ok").toBool())
            refuse(result.value("error").toString());
    });
}

void Phone::textsChanged()
{
    // Push the newest message of each conversation that changed since the
    // last look; the computer asks for the whole thread if it has it open.
    withThreads([this](const QJsonArray &threads) {
        QJsonArray changed;
        QHash<QString, QString> seen;
        for (const QJsonValue &value : threads) {
            const QJsonObject thread = value.toObject();
            const QString key = thread.value("thread").toString();
            const QString mark = QString::number(thread.value("id").toInteger()) + ':' + thread.value("state").toString() +
                                 ':' + QString::number(thread.value("unread").toInt());
            seen.insert(key, mark);
            if (m_textsPrimed && m_threadSeen.value(key) != mark)
                changed.append(smsMessage(thread));
        }
        m_threadSeen = seen;
        m_textsPrimed = true;
        if (!changed.isEmpty())
            sendAll("kdeconnect.sms.messages", {{"messages", changed}, {"version", 2}}, "sms");
    });
}

void Phone::stopRinging()
{
    const uint note = m_ringNote;
    m_ringNote = 0;
    m_daemon->notifier()->close(note);
    m_daemon->broadcast({{"event", "shell.ringStop"}});
}
