#pragma once

#include "device.h"
#include "lan.h"
#include "notifier.h"
#include "theme.h"

#include <QHash>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMap>
#include <QObject>
#include <QTimer>

class Daemon;

// What the link does on this end: the phone half or the computer half.
class Role : public QObject
{
    Q_OBJECT
public:
    Role(Daemon *daemon, bool phone);
    virtual QStringList incoming() const = 0;
    virtual QStringList outgoing() const = 0;
    // A paired device became reachable (or was just paired).
    virtual void connected(Device *device) = 0;
    virtual void received(Device *device, const QString &type, const QJsonObject &body) = 0;
    // A command from a local client; false when it is not this role's.
    virtual bool command(const QJsonObject &command, QJsonObject *reply) = 0;
    virtual QJsonObject deviceState(Device *) const { return {}; }
    // A switch was turned on or off: the theme goes (or is asked for) now.
    void featureChanged(const QString &name, bool on);

protected:
    // The theme goes to each device that links, and whenever it changes
    // here. One from the other end is followed when this end's switch is
    // on and it is the more recent choice; when it (or its background) is
    // missing here, its files are asked for and come in chunks on the link.
    void sendTheme(Device *device);
    QJsonObject themeMessage() const;
    bool themePacket(Device *device, const QString &type, const QJsonObject &body);

    Daemon *m_daemon;
    Theme m_theme;

private:
    void followTheme(Device *device, const QJsonObject &theme);
    void applyTheme(Device *device, const QJsonObject &theme);
    void sendThemeFiles(Device *device, const QString &name);
    void receiveChunk(Device *device, const QJsonObject &body);

    struct Incoming {
        QString device;
        QString name;
        qint64 size = 0;
        int chunks = 0;
        QByteArray sha256;
        QByteArray data;
    };
    Incoming m_incoming;
    QJsonObject m_pendingTheme;
};

class Daemon : public QObject
{
    Q_OBJECT
public:
    explicit Daemon(QObject *parent = nullptr);
    bool start(QString *error);

    QList<Device *> linked() const; // paired and reachable
    Device *device(const QString &id) const { return m_devices.value(id); }
    Notifier *notifier() { return &m_notifier; }
    // An event for local clients: "shell.*" events go to the phone shell,
    // the rest to every subscriber.
    void broadcast(const QJsonObject &event);
    void stateChanged();
    bool hasShell() const;

private:
    struct Client {
        QByteArray buffer;
        bool subscribed = false;
        bool shell = false;
    };

    Device *addDevice(const QString &id);
    void linkedSocket(QSslSocket *socket, const QJsonObject &identity, const QHostAddress &address);
    void incomingClient();
    void readClient(QLocalSocket *socket);
    QJsonObject handle(QLocalSocket *socket, const QJsonObject &command);
    QJsonObject state() const;
    void promptPair(Device *device);
    void closePrompt(Device *device);
    void send(QLocalSocket *socket, const QJsonObject &object);

    Lan m_lan;
    Notifier m_notifier;
    Role *m_role = nullptr;
    QMap<QString, Device *> m_devices;
    QLocalServer m_server;
    QHash<QLocalSocket *, Client> m_clients;
    QHash<uint, QString> m_prompts; // notification ID -> device ID
    QTimer m_announce;
    QTimer m_stateSoon;
};
