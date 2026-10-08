#include "lan.h"

#include "config.h"
#include "packet.h"

#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QRegularExpression>
#include <QSslConfiguration>
#include <QTimer>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

namespace {

constexpr qint64 MaxIdentitySize = 8192;
constexpr qint64 RetryDelayMs = 500;

bool validId(const QString &id)
{
    static const QRegularExpression valid(QStringLiteral("^[a-zA-Z0-9_-]{32,38}$"));
    return valid.match(id).hasMatch();
}

// A link to a phone whose Wi-Fi went down in sleep should end within a
// minute, so the computer shows it as away.
void keepAlive(QAbstractSocket *socket)
{
    socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    const int fd = int(socket->socketDescriptor());
    if (fd < 0)
        return;
    int idle = 30, interval = 10, count = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof interval);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof count);
}

} // namespace

void Lan::Server::incomingConnection(qintptr descriptor)
{
    auto *socket = new QSslSocket(this);
    if (socket->setSocketDescriptor(descriptor))
        addPendingConnection(socket);
    else
        delete socket;
}

Lan::Lan(QObject *parent)
    : QObject(parent)
{
    // Tests run two daemons on one machine over loopback, each on its own
    // UDP port.
    m_test = qEnvironmentVariableIsSet("OMARCHY_LINK_TEST");
    m_udp.setProxy(QNetworkProxy::NoProxy);
    m_server.setProxy(QNetworkProxy::NoProxy);
    connect(&m_udp, &QUdpSocket::readyRead, this, &Lan::readDatagrams);
    connect(&m_server, &QTcpServer::newConnection, this, &Lan::incoming);
}

bool Lan::start(QString *error)
{
    m_udpPort = m_minTcpPort = Config::get().port();
    bool ok = false;
    const int udp = qEnvironmentVariableIntValue("OMARCHY_LINK_UDP_PORT", &ok);
    if (ok)
        m_udpPort = quint16(udp);
    const int peer = qEnvironmentVariableIntValue("OMARCHY_LINK_PEER_UDP_PORT", &ok);
    m_peerUdpPort = ok ? quint16(peer) : m_udpPort;
    const QHostAddress bind = m_test ? QHostAddress(QHostAddress::LocalHost) : QHostAddress(QHostAddress::AnyIPv4);
    if (!m_udp.bind(bind, m_udpPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        *error = "cannot listen on UDP port " + QString::number(m_udpPort) + ": " + m_udp.errorString();
        return false;
    }
    for (quint16 port = m_minTcpPort; port <= m_minTcpPort + TcpPortSpan; ++port)
        if (m_server.listen(bind, port)) {
            m_tcpPort = port;
            return true;
        }
    *error = QString("no free TCP port from %1 to %2").arg(m_minTcpPort).arg(m_minTcpPort + TcpPortSpan);
    return false;
}

QJsonObject Lan::identityBody() const
{
    QJsonObject body = identity();
    body.insert("tcpPort", m_tcpPort);
    return body;
}

void Lan::announce()
{
    const QByteArray datagram = packet("kdeconnect.identity", identityBody());
    QList<QHostAddress> targets;
    if (m_test) {
        targets << QHostAddress(QHostAddress::LocalHost);
    } else {
        // The limited broadcast leaves by the default route only, so each
        // network (Wi-Fi, the USB link) also gets its own broadcast address.
        targets << QHostAddress(QHostAddress::Broadcast);
        for (const QNetworkInterface &interface : QNetworkInterface::allInterfaces()) {
            const auto flags = interface.flags();
            if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
                (flags & QNetworkInterface::IsLoopBack))
                continue;
            for (const QNetworkAddressEntry &entry : interface.addressEntries())
                if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.broadcast().isNull() &&
                    !targets.contains(entry.broadcast()))
                    targets << entry.broadcast();
        }
    }
    for (const QString &address : Config::get().addresses()) {
        const QHostAddress host(address);
        if (!host.isNull() && !targets.contains(host))
            targets << host;
    }
    for (const QHostAddress &target : targets)
        m_udp.writeDatagram(datagram, target, m_peerUdpPort);
}

void Lan::announceTo(const QHostAddress &address)
{
    m_udp.writeDatagram(packet("kdeconnect.identity", identityBody()), address, m_peerUdpPort);
}

void Lan::readDatagrams()
{
    while (m_udp.hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_udp.receiveDatagram(65536);
        QHostAddress sender = datagram.senderAddress();
        if (sender.isLoopback() && !m_test)
            continue;
        bool mapped = false;
        const quint32 v4 = sender.toIPv4Address(&mapped);
        if (mapped)
            sender = QHostAddress(v4);
        QString type;
        QJsonObject body;
        if (!parsePacket(datagram.data(), &type, &body) || type != "kdeconnect.identity")
            continue;
        const QString id = body.value("deviceId").toString();
        if (!validId(id) || id == Config::get().deviceId)
            continue;
        const int version = body.value("protocolVersion").toInt();
        const int port = body.value("tcpPort").toInt();
        if (version < ProtocolVersion || port < m_minTcpPort || port > m_minTcpPort + TcpPortSpan)
            continue;
        discovered(sender, quint16(port), id, version, body.value("deviceType").toString());
    }
}

void Lan::discovered(const QHostAddress &address, quint16 port, const QString &id, int version, const QString &type)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastAttempt.value(id, 0) < RetryDelayMs)
        return;
    m_lastAttempt.insert(id, now);
    if (m_lastAttempt.size() > 1024)
        m_lastAttempt.clear();

    // Two of these daemons hearing each other would both connect, and each
    // might keep a different one of the two links, so one connects and the
    // other answers with its identity so that it does. A computer connects
    // to a phone: computers run firewalls, and the phone's answer to the
    // computer's own datagram passes one as a reply. Between two of a kind
    // the smaller ID connects. KDE Connect peers always connect.
    const bool mobile = type == "phone" || type == "tablet";
    const bool selfMobile = Config::get().deviceType == "phone" || Config::get().deviceType == "tablet";
    const bool connectOut = mobile != selfMobile ? mobile : id < Config::get().deviceId;
    if (!connectOut) {
        announceTo(address);
        return;
    }

    auto *socket = new QSslSocket(this);
    socket->setProxy(QNetworkProxy::NoProxy);
    connect(socket, &QAbstractSocket::errorOccurred, this, [this, socket, address]() {
        // The peer may not be able to reach us either way round: KDE
        // Connect asks it to try the reverse direction.
        if (socket->state() != QAbstractSocket::ConnectedState)
            announceTo(address);
        socket->deleteLater();
    });
    connect(socket, &QAbstractSocket::connected, this, [this, socket, id, version]() { outgoing(socket, id, version); });
    expire(socket, 10000);
    socket->connectToHost(address, port);
}

void Lan::outgoing(QSslSocket *socket, const QString &id, int version)
{
    disconnect(socket, &QAbstractSocket::errorOccurred, this, nullptr);
    connect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);
    keepAlive(socket);
    QJsonObject hello = identityBody();
    hello.insert("targetDeviceId", id);
    hello.insert("targetProtocolVersion", version);
    socket->write(packet("kdeconnect.identity", hello));
    configure(socket, id);
    connect(socket, &QSslSocket::encrypted, this, [this, socket, id, version]() { secure(socket, id, version); });
    socket->startServerEncryption();
}

void Lan::incoming()
{
    while (m_server.hasPendingConnections()) {
        auto *socket = qobject_cast<QSslSocket *>(m_server.nextPendingConnection());
        if (!socket)
            continue;
        socket->setParent(this);
        keepAlive(socket);
        connect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QIODevice::readyRead, this, [this, socket]() { readConnection(socket); });
        expire(socket, 10000);
    }
}

void Lan::readConnection(QSslSocket *socket)
{
    if (socket->bytesAvailable() > MaxIdentitySize) {
        socket->abort();
        return;
    }
    if (!socket->canReadLine())
        return;
    disconnect(socket, &QIODevice::readyRead, this, nullptr);
    QString type;
    QJsonObject body;
    if (!parsePacket(socket->readLine(), &type, &body) || type != "kdeconnect.identity") {
        socket->abort();
        return;
    }
    const QString id = body.value("deviceId").toString();
    const QString target = body.value("targetDeviceId").toString();
    const int version = body.value("protocolVersion").toInt();
    if (!validId(id) || id == Config::get().deviceId || (!target.isEmpty() && target != Config::get().deviceId) ||
        version < ProtocolVersion) {
        socket->abort();
        return;
    }
    configure(socket, id);
    connect(socket, &QSslSocket::encrypted, this, [this, socket, id, version]() { secure(socket, id, version); });
    socket->startClientEncryption();
}

void Lan::configure(QSslSocket *socket, const QString &id)
{
    const Config &config = Config::get();
    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    ssl.setLocalCertificate(config.certificate);
    ssl.setPrivateKey(config.privateKey);
    ssl.setProtocol(QSsl::TlsV1_2OrLater);
    const auto trusted = config.trusted.find(id);
    if (trusted != config.trusted.end()) {
        ssl.setCaCertificates({trusted->certificate});
        ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
    } else {
        // Any certificate until pairing saves this one.
        ssl.setPeerVerifyMode(QSslSocket::QueryPeer);
    }
    socket->setSslConfiguration(ssl);
    socket->setPeerVerifyName(id);
    connect(socket, &QSslSocket::sslErrors, this, [this, socket](const QList<QSslError> &errors) { sslErrors(socket, errors); });
}

void Lan::sslErrors(QSslSocket *socket, const QList<QSslError> &errors)
{
    // Device certificates sign themselves; anything else ends the link.
    for (const QSslError &error : errors)
        if (error.error() != QSslError::SelfSignedCertificate && error.error() != QSslError::SelfSignedCertificateInChain) {
            qWarning("omarchy-link: TLS error from %s: %s", qPrintable(socket->peerAddress().toString()),
                     qPrintable(error.errorString()));
            socket->abort();
            return;
        }
    socket->ignoreSslErrors(errors);
}

void Lan::secure(QSslSocket *socket, const QString &id, int version)
{
    connect(socket, &QIODevice::readyRead, this, [this, socket, id, version]() { readSecureIdentity(socket, id, version); });
    socket->write(packet("kdeconnect.identity", identityBody()));
    socket->flush();
    readSecureIdentity(socket, id, version);
}

void Lan::readSecureIdentity(QSslSocket *socket, const QString &id, int version)
{
    if (socket->bytesAvailable() > MaxIdentitySize) {
        socket->abort();
        return;
    }
    if (!socket->canReadLine())
        return;
    disconnect(socket, &QIODevice::readyRead, this, nullptr);
    QString type;
    QJsonObject body;
    const bool parsed = parsePacket(socket->readLine(), &type, &body);
    const QSslCertificate certificate = socket->peerCertificate();
    const auto trusted = Config::get().trusted.find(id);
    if (!parsed || type != "kdeconnect.identity" || body.value("deviceId").toString() != id ||
        body.value("protocolVersion").toInt() != version || Config::filterName(body.value("deviceName").toString()).isEmpty() ||
        certificate.subjectInfo(QSslCertificate::CommonName).value(0) != id ||
        (trusted != Config::get().trusted.end() && trusted->certificate != certificate)) {
        qWarning("omarchy-link: refused %s: identity or certificate does not match", qPrintable(id));
        socket->abort();
        return;
    }
    disconnect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);
    disconnect(socket, &QSslSocket::sslErrors, this, nullptr);
    disconnect(socket, &QSslSocket::encrypted, this, nullptr);
    socket->setProperty("linked", true);
    QHostAddress address = socket->peerAddress();
    bool mapped = false;
    const quint32 v4 = address.toIPv4Address(&mapped);
    if (mapped)
        address = QHostAddress(v4);
    emit linked(socket, body, address);
}

void Lan::expire(QSslSocket *socket, int msec)
{
    // A connection that has not finished its handshake in time is dropped.
    QTimer::singleShot(msec, socket, [socket]() {
        if (!socket->property("linked").toBool()) {
            socket->abort();
            socket->deleteLater();
        }
    });
}
