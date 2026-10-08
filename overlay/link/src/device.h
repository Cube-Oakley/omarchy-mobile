#pragma once

#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSslCertificate>
#include <QSslSocket>
#include <QTimer>

// A device this one has seen or paired with, its link while it is
// reachable, and pairing as KDE Connect does it: a pair request carries a
// timestamp, each side shows a code made from both certificates and that
// timestamp, and an accept is the same packet sent back.
class Device : public QObject
{
    Q_OBJECT
public:
    enum class Pair { None, Requested, RequestedByPeer, Paired };

    Device(const QString &id, QObject *parent = nullptr);

    const QString id;
    QString name;
    QString type;
    int protocolVersion = 0;
    QSslCertificate certificate;
    QStringList incoming;
    QStringList outgoing;
    QHostAddress address;
    Pair pair = Pair::None;

    bool reachable() const { return m_socket; }
    bool paired() const { return pair == Pair::Paired; }
    void attach(QSslSocket *socket, const QJsonObject &identity, const QHostAddress &address);
    bool send(const QString &type, const QJsonObject &body = {});
    QString code() const;
    QJsonObject state() const;

    void requestPair();
    void acceptPair();
    void rejectPair();
    void unpair();

signals:
    void received(const QString &type, const QJsonObject &body);
    void reachableChanged();
    void changed();
    void pairRequested();
    void pairingDone();
    void pairingFailed(const QString &reason);
    void unpaired();

private:
    void read();
    void handlePair(const QJsonObject &body);
    void lost(QSslSocket *socket);

    QPointer<QSslSocket> m_socket;
    qint64 m_pairTimestamp = 0;
    bool m_toldUnpaired = false;
    QTimer m_pairTimer;
};
