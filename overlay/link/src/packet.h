#pragma once

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>

// KDE Connect's protocol version 8: TLS from the first packet after the
// connection's plain identity line, and the identity sent again inside it.
constexpr int ProtocolVersion = 8;
// KDE Connect listens on UDP 1716 and the first free TCP port of 1716-1764.
// Omarchy devices use the same scheme from 1816 unless set to KDE
// Connect's ports, so the link stays apart from a KDE Connect app (Flux,
// GSConnect) on the same computer.
constexpr quint16 KdeConnectPort = 1716;
constexpr quint16 OmarchyPort = 1816;
constexpr quint16 TcpPortSpan = 48;

// One packet per line: {"id": ms, "type": ..., "body": {...}}.
inline QByteArray packet(const QString &type, const QJsonObject &body = {})
{
    const QJsonObject object{{"id", QDateTime::currentMSecsSinceEpoch()}, {"type", type}, {"body", body}};
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

inline bool parsePacket(const QByteArray &line, QString *type, QJsonObject *body)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    *type = document.object().value("type").toString();
    *body = document.object().value("body").toObject();
    return !type->isEmpty();
}

inline QByteArray jsonLine(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}
