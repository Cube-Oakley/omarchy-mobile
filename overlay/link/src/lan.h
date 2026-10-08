#pragma once

#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QSslSocket>
#include <QTcpServer>
#include <QUdpSocket>

#include <functional>

// Finding devices and opening links, as KDE Connect's LAN backend does:
// - Each device announces its identity by UDP on port 1716.
// - The one that hears it opens a TCP connection and sends its identity in
//   plain text, then the two start TLS with roles reversed (the TCP client
//   is the TLS server).
// - Inside TLS each sends its identity again; the certificate's common name
//   must be the device ID, and a paired device's certificate must be the one
//   saved at pairing.
class Lan : public QObject
{
    Q_OBJECT
public:
    explicit Lan(QObject *parent = nullptr);
    bool start(QString *error);
    void announce();
    void announceTo(const QHostAddress &address);
    quint16 tcpPort() const { return m_tcpPort; }

    // This device's identity body, with its capabilities.
    std::function<QJsonObject()> identity;

signals:
    void linked(QSslSocket *socket, const QJsonObject &identity, const QHostAddress &address);

private:
    class Server : public QTcpServer
    {
    protected:
        void incomingConnection(qintptr descriptor) override;
    };

    void readDatagrams();
    void discovered(const QHostAddress &address, quint16 port, const QString &id, int version, const QString &type);
    void outgoing(QSslSocket *socket, const QString &id, int version);
    void incoming();
    void readConnection(QSslSocket *socket);
    void secure(QSslSocket *socket, const QString &id, int version);
    void readSecureIdentity(QSslSocket *socket, const QString &id, int version);
    void configure(QSslSocket *socket, const QString &id);
    void sslErrors(QSslSocket *socket, const QList<QSslError> &errors);
    void expire(QSslSocket *socket, int msec);
    QJsonObject identityBody() const;

    QUdpSocket m_udp;
    Server m_server;
    quint16 m_tcpPort = 0;
    quint16 m_udpPort = 0;
    quint16 m_peerUdpPort = 0;
    quint16 m_minTcpPort = 0;
    bool m_test = false;
    QHash<QString, qint64> m_lastAttempt;
};
