#include "daemon.h"

#include "config.h"
#include "desktop.h"
#include "packet.h"
#include "phone.h"

#include <QCryptographicHash>
#include <QDir>
#include <QPointer>
#include <QJsonArray>
#include <QNetworkInformation>
#include <QStandardPaths>

namespace {

// While no paired device is reachable, announce now and then; a phone
// coming back from sleep announces itself when its network returns.
constexpr int AnnounceMs = 60000;

QString socketPath()
{
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty())
        runtime = QDir::tempPath();
    return runtime + "/omarchy-link.sock";
}

} // namespace

Role::Role(Daemon *daemon, bool phone)
    : QObject(daemon)
    , m_daemon(daemon)
    , m_theme(phone)
{
    connect(&m_theme, &Theme::changed, this, [this]() {
        for (Device *device : m_daemon->linked())
            sendTheme(device);
    });
}

QJsonObject Role::themeMessage() const
{
    // How long ago it changed, by this end's clock: the two clocks need not
    // agree (the phone's ran 45 s ahead of the laptop's on October 4).
    QJsonObject theme = m_theme.current();
    if (!theme.isEmpty())
        theme.insert("age", double(qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - qint64(theme.value("time").toDouble()))));
    return theme;
}

void Role::sendTheme(Device *device)
{
    if (!Config::get().feature("theme.send"))
        return;
    const QJsonObject theme = themeMessage();
    if (!theme.isEmpty())
        device->send("omarchy.theme", theme);
}

void Role::featureChanged(const QString &name, bool on)
{
    if (!on)
        return;
    for (Device *device : m_daemon->linked()) {
        if (name == "theme.send")
            sendTheme(device);
        else if (name == "theme.follow")
            device->send("omarchy.theme.request");
    }
}

bool Role::themePacket(Device *device, const QString &type, const QJsonObject &body)
{
    if (type == "omarchy.theme") {
        followTheme(device, body);
    } else if (type == "omarchy.theme.request") {
        // No name: the current theme. A name: that theme's files.
        const QString name = body.value("name").toString();
        if (name.isEmpty()) {
            if (!m_theme.current().isEmpty())
                device->send("omarchy.theme", themeMessage());
        } else if (Config::get().feature("theme.send")) {
            sendThemeFiles(device, name);
        }
    } else if (type == "omarchy.theme.files" || type == "omarchy.theme.chunk") {
        receiveChunk(device, body);
    } else if (type == "omarchy.theme.result") {
        // How the other end fared with a theme from here, or why the files
        // this end asked for could not come.
        const QString name = body.value("name").toString();
        const QString error = body.value("error").toString();
        m_daemon->broadcast({{"event", "theme.result"}, {"device", device->id}, {"name", name}, {"error", error}});
        if (!error.isEmpty() && m_pendingTheme.value("name").toString() == name) {
            m_pendingTheme = {};
            m_daemon->notifier()->notify({"Omarchy Link", "preferences-desktop-theme", "Could not get " + name + " from " + device->name,
                                          error, {}, {}, -1, 0});
        }
    } else {
        return false;
    }
    return true;
}

void Role::followTheme(Device *device, const QJsonObject &theme)
{
    if (!Config::get().feature("theme.follow"))
        return;
    const QJsonObject mine = m_theme.current();
    const QString name = theme.value("name").toString();
    const QString background = theme.value("background").toString();
    if (!Theme::validName(name))
        return;
    if (name == mine.value("name").toString() && background == mine.value("background").toString())
        return;
    // The more recent choice wins: the other end's age turned into a time
    // on this end's clock.
    const double changed = theme.contains("age") ? double(QDateTime::currentMSecsSinceEpoch()) - theme.value("age").toDouble()
                                                 : theme.value("time").toDouble();
    if (changed <= mine.value("time").toDouble())
        return;
    if (m_theme.has(name, background)) {
        applyTheme(device, theme);
        return;
    }
    m_pendingTheme = theme;
    device->send("omarchy.theme.request", {{"name", name}});
}

void Role::applyTheme(Device *device, const QJsonObject &theme)
{
    const QString name = theme.value("name").toString();
    QPointer<Device> target(device);
    m_theme.apply(theme, [this, target, name](const QString &error) {
        if (target)
            target->send("omarchy.theme.result", {{"name", name}, {"error", error}});
        if (!error.isEmpty())
            m_daemon->notifier()->notify({"Omarchy Link", "preferences-desktop-theme", "Could not switch to " + name, error,
                                          {}, {}, -1, 0});
    });
}

void Role::sendThemeFiles(Device *device, const QString &name)
{
    // Chunks of the archive on the link itself: no second connection, so
    // no firewall port, and other packets still pass between chunks.
    constexpr int ChunkSize = 768 * 1024;
    constexpr qint64 MaxSize = 200 * 1024 * 1024;
    QPointer<Device> target(device);
    m_theme.pack(name, [this, target, name](const QByteArray &archive, const QString &error) {
        if (!target)
            return;
        if (!error.isEmpty() || archive.size() > MaxSize) {
            const QString why = error.isEmpty() ? name + " is too large to send" : error;
            target->send("omarchy.theme.result", {{"name", name}, {"error", why}});
            m_daemon->broadcast({{"event", "theme.result"}, {"device", target->id}, {"name", name}, {"error", "could not send it: " + why}});
            return;
        }
        const int chunks = int((archive.size() + ChunkSize - 1) / ChunkSize);
        target->send("omarchy.theme.files",
                     {{"name", name}, {"size", double(archive.size())}, {"chunks", chunks},
                      {"sha256", QString::fromLatin1(QCryptographicHash::hash(archive, QCryptographicHash::Sha256).toHex())}});
        for (int index = 0; index < chunks; ++index)
            target->send("omarchy.theme.chunk",
                         {{"name", name}, {"index", index},
                          {"data", QString::fromLatin1(archive.mid(qsizetype(index) * ChunkSize, ChunkSize).toBase64())}});
    });
}

void Role::receiveChunk(Device *device, const QJsonObject &body)
{
    constexpr qint64 MaxSize = 200 * 1024 * 1024;
    const QString name = body.value("name").toString();
    if (body.contains("chunks")) {
        // Only files this end asked for are taken.
        if (m_pendingTheme.value("name").toString() != name)
            return;
        m_incoming = {device->id, name, qint64(body.value("size").toDouble()), body.value("chunks").toInt(),
                      body.value("sha256").toString().toLatin1(), {}};
        if (m_incoming.size <= 0 || m_incoming.size > MaxSize)
            m_incoming = {};
        return;
    }
    if (m_incoming.device != device->id || m_incoming.name != name)
        return;
    m_incoming.data += QByteArray::fromBase64(body.value("data").toString().toLatin1());
    if (body.value("index").toInt() + 1 < m_incoming.chunks && m_incoming.data.size() < m_incoming.size)
        return;
    const Incoming incoming = m_incoming;
    m_incoming = {};
    const QJsonObject theme = m_pendingTheme;
    QPointer<Device> target(device);
    auto fail = [this, target, name](const QString &error) {
        if (target)
            target->send("omarchy.theme.result", {{"name", name}, {"error", error}});
        m_daemon->notifier()->notify({"Omarchy Link", "preferences-desktop-theme", "Could not install " + name, error, {}, {}, -1, 0});
    };
    if (incoming.data.size() != incoming.size ||
        QCryptographicHash::hash(incoming.data, QCryptographicHash::Sha256).toHex() != incoming.sha256) {
        fail("the files arrived damaged");
        return;
    }
    m_theme.unpack(name, incoming.data, [this, target, theme, fail](const QString &error) {
        if (!error.isEmpty()) {
            fail(error);
            return;
        }
        if (target && m_pendingTheme == theme) {
            m_pendingTheme = {};
            applyTheme(target, theme);
        }
    });
}

Daemon::Daemon(QObject *parent)
    : QObject(parent)
{
    m_announce.setInterval(AnnounceMs);
    connect(&m_announce, &QTimer::timeout, this, [this]() {
        if (linked().isEmpty())
            m_lan.announce();
    });
    m_stateSoon.setSingleShot(true);
    m_stateSoon.setInterval(0);
    connect(&m_stateSoon, &QTimer::timeout, this, [this]() { broadcast(state()); });
    connect(&m_lan, &Lan::linked, this, &Daemon::linkedSocket);
    connect(&m_server, &QLocalServer::newConnection, this, &Daemon::incomingClient);
    connect(&m_notifier, &Notifier::actionInvoked, this, [this](uint id, const QString &key) {
        Device *device = m_devices.value(m_prompts.take(id));
        if (!device)
            return;
        if (key == "accept")
            device->acceptPair();
        else if (key == "reject")
            device->rejectPair();
    });
}

bool Daemon::start(QString *error)
{
    Config &config = Config::get();
    if (!config.load(error))
        return false;

    // One daemon per user: a live socket means another is running.
    const QString path = socketPath();
    QLocalSocket probe;
    probe.connectToServer(path);
    if (probe.waitForConnected(500)) {
        *error = "already running (" + path + ")";
        return false;
    }
    QLocalServer::removeServer(path);
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server.listen(path)) {
        *error = "cannot listen on " + path + ": " + m_server.errorString();
        return false;
    }

    if (config.role == "phone")
        m_role = new Phone(this);
    else
        m_role = new Desktop(this);
    m_lan.identity = [this]() {
        const Config &config = Config::get();
        return QJsonObject{
            {"deviceId", config.deviceId},
            {"deviceName", config.deviceName},
            {"deviceType", config.deviceType},
            {"protocolVersion", ProtocolVersion},
            {"incomingCapabilities", QJsonArray::fromStringList(m_role->incoming())},
            {"outgoingCapabilities", QJsonArray::fromStringList(m_role->outgoing())},
        };
    };
    for (auto it = config.trusted.begin(); it != config.trusted.end(); ++it) {
        Device *device = addDevice(it.key());
        device->name = it->name;
        device->type = it->type;
        device->certificate = it->certificate;
        device->protocolVersion = it->protocolVersion;
        device->address = QHostAddress(it->address);
        device->pair = Device::Pair::Paired;
    }
    if (!m_lan.start(error))
        return false;

    if (QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Reachability)) {
        connect(QNetworkInformation::instance(), &QNetworkInformation::reachabilityChanged, this,
                [this](QNetworkInformation::Reachability reachability) {
                    if (reachability >= QNetworkInformation::Reachability::Local)
                        QTimer::singleShot(1000, this, [this]() { m_lan.announce(); });
                });
    }
    m_lan.announce();
    m_announce.start();
    qInfo("omarchy-link: %s \"%s\" (%s) on TCP %d", qPrintable(config.role), qPrintable(config.deviceName),
          qPrintable(config.deviceId), m_lan.tcpPort());
    return true;
}

Device *Daemon::addDevice(const QString &id)
{
    auto *device = new Device(id, this);
    m_devices.insert(id, device);
    connect(device, &Device::changed, this, &Daemon::stateChanged);
    connect(device, &Device::received, this, [this, device](const QString &type, const QJsonObject &body) {
        if (type == "kdeconnect.ping") {
            m_notifier.notify({"Omarchy Link", "phone", "Ping from " + device->name,
                               body.value("message").toString(), {}, {}, -1, 0});
            return;
        }
        m_role->received(device, type, body);
    });
    connect(device, &Device::reachableChanged, this, [this, device]() {
        if (device->reachable() && device->paired())
            m_role->connected(device);
        if (!device->reachable() && !device->paired()) {
            // A device that was never paired is forgotten when it goes.
            closePrompt(device);
            m_devices.remove(device->id);
            device->deleteLater();
        }
        stateChanged();
    });
    connect(device, &Device::pairRequested, this, [this, device]() { promptPair(device); });
    connect(device, &Device::pairingDone, this, [this, device]() {
        closePrompt(device);
        m_notifier.notify({"Omarchy Link", "phone", "Paired with " + device->name, {}, {}, {}, 5000, 0});
        broadcast({{"event", "paired"}, {"device", device->id}});
        if (device->reachable())
            m_role->connected(device);
    });
    connect(device, &Device::pairingFailed, this, [this, device](const QString &reason) {
        closePrompt(device);
        broadcast({{"event", "pairFailed"}, {"device", device->id}, {"reason", reason}});
    });
    connect(device, &Device::unpaired, this, [this, device]() {
        broadcast({{"event", "unpaired"}, {"device", device->id}});
    });
    return device;
}

void Daemon::linkedSocket(QSslSocket *socket, const QJsonObject &identity, const QHostAddress &address)
{
    const QString id = identity.value("deviceId").toString();
    Device *device = m_devices.value(id);
    if (!device)
        device = addDevice(id);
    device->attach(socket, identity, address);
}

QList<Device *> Daemon::linked() const
{
    QList<Device *> result;
    for (Device *device : m_devices)
        if (device->paired() && device->reachable())
            result << device;
    return result;
}

void Daemon::promptPair(Device *device)
{
    closePrompt(device);
    const QString code = device->code();
    Notifier::Note note{"Omarchy Link", "phone", "Pair with " + device->name + "?",
                        "Pair only if " + device->name + " shows " + code + ".",
                        {"accept", "Pair", "reject", "Decline"}, {{"urgency", QVariant::fromValue(uchar(2))}}, 30000, 0};
    const QString id = device->id;
    m_notifier.notify(note, [this, id](uint notification) {
        if (notification)
            m_prompts.insert(notification, id);
    });
    broadcast({{"event", "pairRequest"}, {"device", id}, {"name", device->name}, {"code", code}});
}

void Daemon::closePrompt(Device *device)
{
    for (auto it = m_prompts.begin(); it != m_prompts.end();) {
        if (it.value() == device->id) {
            m_notifier.close(it.key());
            it = m_prompts.erase(it);
        } else {
            ++it;
        }
    }
}

void Daemon::stateChanged()
{
    m_stateSoon.start();
}

QJsonObject Daemon::state() const
{
    const Config &config = Config::get();
    QJsonArray devices;
    for (Device *device : m_devices) {
        QJsonObject entry = device->state();
        const QJsonObject extra = m_role->deviceState(device);
        for (auto it = extra.begin(); it != extra.end(); ++it)
            entry.insert(it.key(), it.value());
        devices.append(entry);
    }
    return {
        {"event", "state"},
        {"self", QJsonObject{{"id", config.deviceId}, {"name", config.deviceName}, {"type", config.deviceType},
                             {"role", config.role}}},
        {"features", config.features()},
        {"devices", devices},
    };
}

void Daemon::incomingClient()
{
    while (QLocalSocket *socket = m_server.nextPendingConnection()) {
        m_clients.insert(socket, {});
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() { readClient(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket]() {
            m_clients.remove(socket);
            socket->deleteLater();
        });
    }
}

void Daemon::readClient(QLocalSocket *socket)
{
    while (socket->canReadLine()) {
        const QJsonObject command = QJsonDocument::fromJson(socket->readLine()).object();
        if (command.isEmpty())
            continue;
        QJsonObject reply = handle(socket, command);
        if (!reply.isEmpty()) {
            reply.insert("reply", command.value("cmd"));
            if (command.contains("tag"))
                reply.insert("tag", command.value("tag"));
            send(socket, reply);
        }
    }
    if (socket->bytesAvailable() > 1 << 20)
        socket->abort();
}

QJsonObject Daemon::handle(QLocalSocket *socket, const QJsonObject &command)
{
    const QString cmd = command.value("cmd").toString();
    Device *device = m_devices.value(command.value("device").toString());
    auto ok = [](bool done, const QString &error = "no such device") {
        return done ? QJsonObject{{"ok", true}} : QJsonObject{{"ok", false}, {"error", error}};
    };
    if (cmd == "state")
        return state();
    if (cmd == "subscribe") {
        m_clients[socket].subscribed = true;
        send(socket, state());
        return {};
    }
    if (cmd == "shell.hello") {
        m_clients[socket].subscribed = true;
        m_clients[socket].shell = true;
        send(socket, state());
        return {};
    }
    if (cmd == "announce") {
        m_lan.announce();
        return ok(true);
    }
    if (cmd == "connect") {
        const QHostAddress address(command.value("address").toString());
        if (address.isNull() || address.protocol() != QAbstractSocket::IPv4Protocol)
            return ok(false, "not an IPv4 address");
        Config::get().addAddress(address.toString());
        m_lan.announceTo(address);
        return ok(true);
    }
    if (cmd == "feature") {
        const QString name = command.value("name").toString();
        if (!Config::get().features().contains(name))
            return ok(false, "no such feature");
        Config::get().setFeature(name, command.value("on").toBool());
        m_role->featureChanged(name, command.value("on").toBool());
        stateChanged();
        return ok(true);
    }
    if (cmd == "pair" || cmd == "accept" || cmd == "reject" || cmd == "unpair" || cmd == "ping") {
        if (!device)
            return ok(false);
        if (cmd == "pair")
            device->requestPair();
        else if (cmd == "accept")
            device->acceptPair();
        else if (cmd == "reject")
            device->rejectPair();
        else if (cmd == "unpair")
            device->unpair();
        else
            return ok(device->paired() && device->send("kdeconnect.ping", {{"message", command.value("message").toString()}}),
                      "not linked");
        return QJsonObject{{"ok", true}, {"code", device->code()}};
    }
    QJsonObject reply;
    if (m_role->command(command, &reply))
        return reply;
    return ok(false, "unknown command " + cmd);
}

void Daemon::send(QLocalSocket *socket, const QJsonObject &object)
{
    socket->write(jsonLine(object));
}

void Daemon::broadcast(const QJsonObject &event)
{
    const bool forShell = event.value("event").toString().startsWith("shell.");
    const QByteArray line = jsonLine(event);
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it)
        if (forShell ? it->shell : it->subscribed)
            it.key()->write(line);
}

bool Daemon::hasShell() const
{
    for (const Client &client : m_clients)
        if (client.shell)
            return true;
    return false;
}
