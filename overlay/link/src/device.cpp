#include "device.h"

#include "config.h"
#include "packet.h"

#include <QCryptographicHash>
#include <QJsonArray>

namespace {

// KDE Connect: a pair request is answered within 30 s, and its timestamp
// may be at most 30 minutes off this device's clock.
constexpr int PairTimeoutMs = 30000;
constexpr qint64 PairClockSkew = 1800;

QStringList strings(const QJsonValue &value)
{
    QStringList list;
    for (const QJsonValue &item : value.toArray())
        list << item.toString();
    return list;
}

} // namespace

Device::Device(const QString &id, QObject *parent)
    : QObject(parent)
    , id(id)
{
    m_pairTimer.setSingleShot(true);
    m_pairTimer.setInterval(PairTimeoutMs);
    connect(&m_pairTimer, &QTimer::timeout, this, [this]() {
        send("kdeconnect.pair", {{"pair", false}});
        pair = Pair::None;
        emit pairingFailed("Timed out");
        emit changed();
    });
}

void Device::attach(QSslSocket *socket, const QJsonObject &identity, const QHostAddress &peerAddress)
{
    if (m_socket && m_socket != socket) {
        // A newer link replaces one that may have gone stale (a phone
        // whose Wi-Fi slept). Its end must not count as losing the device.
        QSslSocket *old = m_socket;
        disconnect(old, nullptr, this, nullptr);
        old->abort();
        old->deleteLater();
    }
    m_socket = socket;
    socket->setParent(this);
    name = Config::filterName(identity.value("deviceName").toString());
    type = identity.value("deviceType").toString();
    protocolVersion = identity.value("protocolVersion").toInt();
    incoming = strings(identity.value("incomingCapabilities"));
    outgoing = strings(identity.value("outgoingCapabilities"));
    certificate = socket->peerCertificate();
    address = peerAddress;
    m_toldUnpaired = false;
    if (paired()) {
        Config::Trusted saved = Config::get().trusted.value(id);
        if (saved.name != name || saved.type != type || saved.protocolVersion != protocolVersion ||
            saved.address != address.toString())
            Config::get().trust(id, {name, type, certificate, protocolVersion, address.toString()});
    }
    connect(socket, &QIODevice::readyRead, this, &Device::read);
    connect(socket, &QAbstractSocket::disconnected, this, [this, socket]() { lost(socket); });
    emit reachableChanged();
    emit changed();
    read();
}

void Device::lost(QSslSocket *socket)
{
    if (socket != m_socket)
        return;
    m_socket = nullptr;
    socket->deleteLater();
    if (pair == Pair::Requested || pair == Pair::RequestedByPeer) {
        m_pairTimer.stop();
        pair = Pair::None;
        emit pairingFailed("Device not reachable");
    }
    emit reachableChanged();
    emit changed();
}

bool Device::send(const QString &type, const QJsonObject &body)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        return false;
    return m_socket->write(packet(type, body)) > 0;
}

void Device::read()
{
    while (m_socket && m_socket->canReadLine()) {
        QString type;
        QJsonObject body;
        if (!parsePacket(m_socket->readLine(), &type, &body))
            continue;
        if (type == "kdeconnect.pair") {
            handlePair(body);
        } else if (paired()) {
            emit received(type, body);
        } else if (!m_toldUnpaired && type != "kdeconnect.identity") {
            // A device that thinks it is paired with this one learns it is not.
            m_toldUnpaired = true;
            send("kdeconnect.pair", {{"pair", false}});
        }
    }
}

void Device::handlePair(const QJsonObject &body)
{
    if (body.value("pair").toBool()) {
        switch (pair) {
        case Pair::Requested:
            m_pairTimer.stop();
            pair = Pair::Paired;
            Config::get().trust(id, {name, type, certificate, protocolVersion, address.toString()});
            emit pairingDone();
            break;
        case Pair::RequestedByPeer:
            break;
        case Pair::Paired:
        case Pair::None: {
            if (pair == Pair::Paired) {
                // A request from a paired device: it has forgotten this one.
                pair = Pair::None;
                Config::get().untrust(id);
                emit unpaired();
            }
            const QJsonValue stamp = body.value("timestamp");
            if (!stamp.isDouble()) {
                emit changed();
                return;
            }
            m_pairTimestamp = qint64(stamp.toDouble());
            if (qAbs(m_pairTimestamp - QDateTime::currentSecsSinceEpoch()) > PairClockSkew) {
                emit pairingFailed("Device clocks are out of sync");
                emit changed();
                return;
            }
            pair = Pair::RequestedByPeer;
            m_pairTimer.start();
            emit pairRequested();
            break;
        }
        }
    } else {
        m_pairTimer.stop();
        switch (pair) {
        case Pair::None:
            break;
        case Pair::Requested:
        case Pair::RequestedByPeer:
            pair = Pair::None;
            emit pairingFailed("Cancelled by the other device");
            break;
        case Pair::Paired:
            pair = Pair::None;
            Config::get().untrust(id);
            emit unpaired();
            break;
        }
    }
    emit changed();
}

void Device::requestPair()
{
    if (pair == Pair::Paired)
        return;
    if (pair == Pair::RequestedByPeer) {
        acceptPair();
        return;
    }
    m_pairTimestamp = QDateTime::currentSecsSinceEpoch();
    if (!send("kdeconnect.pair", {{"pair", true}, {"timestamp", m_pairTimestamp}})) {
        emit pairingFailed("Device not reachable");
        return;
    }
    pair = Pair::Requested;
    m_pairTimer.start();
    emit changed();
}

void Device::acceptPair()
{
    if (pair != Pair::RequestedByPeer)
        return;
    m_pairTimer.stop();
    if (!send("kdeconnect.pair", {{"pair", true}})) {
        pair = Pair::None;
        emit pairingFailed("Device not reachable");
        emit changed();
        return;
    }
    pair = Pair::Paired;
    Config::get().trust(id, {name, type, certificate, protocolVersion, address.toString()});
    emit pairingDone();
    emit changed();
}

void Device::rejectPair()
{
    if (pair != Pair::Requested && pair != Pair::RequestedByPeer)
        return;
    m_pairTimer.stop();
    pair = Pair::None;
    send("kdeconnect.pair", {{"pair", false}});
    emit pairingFailed("Cancelled");
    emit changed();
}

void Device::unpair()
{
    m_pairTimer.stop();
    pair = Pair::None;
    send("kdeconnect.pair", {{"pair", false}});
    Config::get().untrust(id);
    emit unpaired();
    emit changed();
}

QString Device::code() const
{
    // The same eight characters KDE Connect shows: SHA-256 over both public
    // keys (larger first) and the request's timestamp.
    if (pair != Pair::Requested && pair != Pair::RequestedByPeer)
        return {};
    QByteArray a = Config::get().certificate.publicKey().toDer();
    QByteArray b = certificate.publicKey().toDer();
    if (a < b)
        std::swap(a, b);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(a);
    hash.addData(b);
    hash.addData(QByteArray::number(m_pairTimestamp));
    return QString::fromLatin1(hash.result().toHex().left(8).toUpper());
}

QJsonObject Device::state() const
{
    static const char *pairing[] = {"", "requested", "incoming", ""};
    return {
        {"id", id},
        {"name", name},
        {"type", type},
        {"reachable", reachable()},
        {"paired", paired()},
        {"pairing", pairing[int(pair)]},
        {"code", code()},
        {"address", address.isNull() ? QString() : address.toString()},
    };
}
