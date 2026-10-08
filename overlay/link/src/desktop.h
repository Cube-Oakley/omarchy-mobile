#pragma once

#include "daemon.h"

#include <QHash>
#include <QJsonArray>
#include <QMap>

// The computer's half of the link:
// - the phone's battery for the bar;
// - the phone's notifications as desktop notifications (dismissing one here
//   dismisses it on the phone; its buttons work from here);
// - texts: the conversation list, threads on request, sending (held until
//   the phone is back when it is asleep);
// - ringing the phone, call alerts, and the theme, sent and followed.
class Desktop : public Role
{
    Q_OBJECT
public:
    explicit Desktop(Daemon *daemon);
    QStringList incoming() const override;
    QStringList outgoing() const override;
    void connected(Device *device) override;
    void received(Device *device, const QString &type, const QJsonObject &body) override;
    bool command(const QJsonObject &command, QJsonObject *reply) override;
    QJsonObject deviceState(Device *device) const override;

private:
    struct Phone {
        QJsonObject battery;
        QMap<QString, QJsonObject> notes;     // phone's notification ID -> notification
        QMap<qint64, QJsonObject> threads;    // thread ID -> newest message
        uint call = 0;                        // desktop notification for a ringing call
    };

    void notification(Device *device, const QJsonObject &body);
    void texts(Device *device, const QJsonObject &body);
    void telephony(Device *device, const QJsonObject &body);
    QJsonArray conversations(const QString &device) const;
    QJsonArray notes(const QString &device) const;
    void sendQueued(Device *device);
    void loadOutbox();
    void saveOutbox() const;

    QMap<QString, Phone> m_phones;
    QHash<uint, QPair<QString, QString>> m_posted; // desktop ID -> device, phone's notification ID
    QJsonArray m_outbox;                            // texts waiting for the phone
};
