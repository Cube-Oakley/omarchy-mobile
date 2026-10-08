#pragma once

#include "daemon.h"

#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonArray>
#include <QMap>
#include <QTimer>

#include <functional>

// The phone's half of the link:
// - battery level;
// - the shell's notifications, mirrored, dismissed and acted on from the
//   computer;
// - texts: conversations and threads from omarchy-mobile-messages, sending,
//   and new or changed messages pushed as they land in its database;
// - ringing the phone, through the shell;
// - the theme, sent and followed.
class Phone : public Role
{
    Q_OBJECT
public:
    explicit Phone(Daemon *daemon);
    QStringList incoming() const override;
    QStringList outgoing() const override;
    void connected(Device *device) override;
    void received(Device *device, const QString &type, const QJsonObject &body) override;
    bool command(const QJsonObject &command, QJsonObject *reply) override;

private:
    void sendAll(const QString &type, const QJsonObject &body, const QString &feature);
    QJsonObject battery() const;
    QJsonObject notificationPacket(const QJsonObject &note, bool silent) const;

    void messages(const QStringList &args, std::function<void(const QJsonObject &)> done);
    QJsonObject smsMessage(const QJsonObject &row, const QJsonObject &person = {});
    qint64 threadId(const QString &key);
    void withThreads(std::function<void(const QJsonArray &threads)> done);
    void sendConversations(Device *device);
    void sendConversation(Device *device, const QJsonObject &request);
    void sendText(Device *device, const QJsonObject &request);
    void textsChanged();
    void stopRinging();

    QTimer m_batteryTimer;
    QJsonObject m_lastBattery;
    QMap<QString, QJsonObject> m_notes; // shell notification ID -> notification
    QHash<qint64, QString> m_threadKeys;
    QHash<QString, QString> m_threadSeen; // key -> latest message's id and state
    QFileSystemWatcher m_texts;
    QTimer m_textsSettle;
    bool m_textsPrimed = false;
    uint m_ringNote = 0;
};
