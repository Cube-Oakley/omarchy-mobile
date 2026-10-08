#pragma once

#include <QJsonObject>
#include <QMap>
#include <QSslCertificate>
#include <QSslKey>
#include <QString>

// This device's identity and certificate, the paired devices and the
// feature switches, under $XDG_CONFIG_HOME/omarchy-link.
class Config
{
public:
    struct Trusted {
        QString name;
        QString type;
        QSslCertificate certificate;
        int protocolVersion = 0;
        QString address;
    };

    static Config &get();
    bool load(QString *error);

    QString dir;
    QString deviceId;
    QString deviceName;
    QString deviceType;
    QString role; // "phone" or "desktop"
    QSslCertificate certificate;
    QSslKey privateKey;
    QMap<QString, Trusted> trusted;

    void trust(const QString &id, const Trusted &device);
    void untrust(const QString &id);
    void rememberAddress(const QString &id, const QString &address);

    bool feature(const QString &name) const;
    void setFeature(const QString &name, bool on);
    QJsonObject features() const;
    quint16 port() const;
    QStringList addresses() const;
    void addAddress(const QString &address);

    static QString filterName(QString name);

private:
    QJsonObject m_settings;
    void saveTrusted() const;
    void saveSettings() const;
    bool makeCertificate(QString *error) const;
};
