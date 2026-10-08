#include "config.h"

#include "packet.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUuid>

namespace {

QJsonObject readJson(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool writeJson(const QString &path, const QJsonObject &object)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(object).toJson());
    return file.commit();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool hasBattery()
{
    const QDir supplies("/sys/class/power_supply");
    for (const QString &name : supplies.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (readFile(supplies.filePath(name + "/type")).trimmed() == "Battery")
            return true;
    return false;
}

// Switches a new install starts with. Clipboard sharing stays opt-in.
const QMap<QString, bool> &defaultFeatures(const QString &role)
{
    static const QMap<QString, bool> phone{
        {"notifications", true}, {"sms", true}, {"battery", true}, {"ring", true},
        {"theme.send", true}, {"theme.follow", true}, {"clipboard", false},
    };
    static const QMap<QString, bool> desktop{
        {"notifications", true}, {"sms", true}, {"battery", true}, {"ring", true},
        {"theme.send", true}, {"theme.follow", false}, {"clipboard", false},
    };
    return role == "phone" ? phone : desktop;
}

} // namespace

Config &Config::get()
{
    static Config config;
    return config;
}

QString Config::filterName(QString name)
{
    // KDE Connect drops these from names and keeps 32 characters.
    static const QRegularExpression invalid(QStringLiteral("[\"',;:.!?()\\[\\]<>]"));
    return name.remove(invalid).trimmed().left(32);
}

bool Config::load(QString *error)
{
    dir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/omarchy-link";
    if (!QDir().mkpath(dir)) {
        *error = "cannot create " + dir;
        return false;
    }
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    m_settings = readJson(dir + "/settings.json");
    const QString home = QDir::homePath();
    role = qEnvironmentVariable("OMARCHY_LINK_ROLE", m_settings.value("role").toString());
    if (role != "phone" && role != "desktop")
        role = QFile::exists(home + "/.local/bin/omarchy-mobile-messages") ? "phone" : "desktop";
    deviceType = role == "phone" ? "phone" : (hasBattery() ? "laptop" : "desktop");

    QString name = m_settings.value("name").toString();
    if (name.isEmpty() && role == "phone") {
        // The device adapter's name, else the board's: bring-up device trees
        // carry a codename ("GS201 CHEETAH"), not the phone's name.
        const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        name = QJsonDocument::fromJson(readFile(config + "/omarchy-mobile/device.json")).object().value("name").toString();
        const QString board = QString::fromUtf8(readFile("/proc/device-tree/compatible")).toLower();
        static const QList<QPair<QString, QString>> boards{
            {"cheetah", "Pixel 7 Pro"}, {"panther", "Pixel 7"}, {"guacamole", "OnePlus 7 Pro"}};
        for (const auto &[codename, model] : boards)
            if (name.isEmpty() && board.contains(codename))
                name = model;
        if (name.isEmpty())
            name = QString::fromUtf8(readFile("/proc/device-tree/model")).remove(QChar(0)).remove(QRegularExpression("^Google "));
    }
    if (name.isEmpty())
        name = QSysInfo::machineHostName();
    deviceName = filterName(name);
    if (deviceName.isEmpty())
        deviceName = role == "phone" ? "Omarchy phone" : "Omarchy";

    QJsonObject identity = readJson(dir + "/identity.json");
    deviceId = identity.value("deviceId").toString();
    static const QRegularExpression valid(QStringLiteral("^[a-zA-Z0-9_-]{32,38}$"));
    if (!valid.match(deviceId).hasMatch()) {
        deviceId = QUuid::createUuid().toString(QUuid::WithoutBraces).replace('-', '_');
        identity["deviceId"] = deviceId;
        if (!writeJson(dir + "/identity.json", identity)) {
            *error = "cannot write identity.json";
            return false;
        }
        QFile::remove(dir + "/certificate.pem");
        QFile::remove(dir + "/privateKey.pem");
    }

    if (!QFile::exists(dir + "/certificate.pem") || !QFile::exists(dir + "/privateKey.pem"))
        if (!makeCertificate(error))
            return false;
    const QList<QSslCertificate> certificates = QSslCertificate::fromData(readFile(dir + "/certificate.pem"));
    privateKey = QSslKey(readFile(dir + "/privateKey.pem"), QSsl::Ec);
    if (certificates.isEmpty() || privateKey.isNull()) {
        *error = "unreadable certificate or key in " + dir;
        return false;
    }
    certificate = certificates.first();
    if (certificate.subjectInfo(QSslCertificate::CommonName).value(0) != deviceId) {
        *error = "the certificate is not this device's (" + deviceId + ")";
        return false;
    }

    const QJsonObject devices = readJson(dir + "/devices.json");
    for (auto it = devices.begin(); it != devices.end(); ++it) {
        const QJsonObject entry = it.value().toObject();
        const QList<QSslCertificate> peer = QSslCertificate::fromData(entry.value("certificate").toString().toUtf8());
        if (peer.isEmpty())
            continue;
        trusted.insert(it.key(), {entry.value("name").toString(), entry.value("type").toString(), peer.first(),
                                  entry.value("protocolVersion").toInt(), entry.value("address").toString()});
    }
    return true;
}

bool Config::makeCertificate(QString *error) const
{
    // An EC P-256 certificate whose common name is the device ID, as KDE
    // Connect makes them. Dated from 2025 so a peer whose clock runs behind
    // still accepts it.
    QProcess openssl;
    openssl.start("openssl", {"req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes",
                              "-not_before", "20250101000000Z", "-not_after", "20450101000000Z",
                              "-subj", "/O=KDE/OU=Kde connect/CN=" + deviceId,
                              "-keyout", dir + "/privateKey.pem", "-out", dir + "/certificate.pem"});
    if (!openssl.waitForFinished(30000) || openssl.exitStatus() != QProcess::NormalExit || openssl.exitCode() != 0) {
        *error = "openssl could not make a certificate: " + QString::fromUtf8(openssl.readAllStandardError());
        return false;
    }
    QFile::setPermissions(dir + "/privateKey.pem", QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

void Config::trust(const QString &id, const Trusted &device)
{
    trusted.insert(id, device);
    saveTrusted();
}

void Config::untrust(const QString &id)
{
    if (trusted.remove(id))
        saveTrusted();
}

void Config::rememberAddress(const QString &id, const QString &address)
{
    auto it = trusted.find(id);
    if (it == trusted.end() || it->address == address)
        return;
    it->address = address;
    saveTrusted();
}

void Config::saveTrusted() const
{
    QJsonObject devices;
    for (auto it = trusted.begin(); it != trusted.end(); ++it)
        devices.insert(it.key(), QJsonObject{
                                     {"name", it->name},
                                     {"type", it->type},
                                     {"certificate", QString::fromUtf8(it->certificate.toPem())},
                                     {"protocolVersion", it->protocolVersion},
                                     {"address", it->address},
                                 });
    writeJson(dir + "/devices.json", devices);
}

bool Config::feature(const QString &name) const
{
    const QJsonValue value = m_settings.value("features").toObject().value(name);
    return value.isBool() ? value.toBool() : defaultFeatures(role).value(name, false);
}

void Config::setFeature(const QString &name, bool on)
{
    QJsonObject features = m_settings.value("features").toObject();
    features.insert(name, on);
    m_settings.insert("features", features);
    saveSettings();
}

QJsonObject Config::features() const
{
    QJsonObject result;
    for (auto it = defaultFeatures(role).begin(); it != defaultFeatures(role).end(); ++it)
        result.insert(it.key(), feature(it.key()));
    return result;
}

quint16 Config::port() const
{
    return m_settings.value("kdeconnectPorts").toBool() ? KdeConnectPort : OmarchyPort;
}

QStringList Config::addresses() const
{
    QStringList result;
    for (const QJsonValue &value : m_settings.value("addresses").toArray())
        result << value.toString();
    for (const Trusted &device : trusted)
        if (!device.address.isEmpty() && !result.contains(device.address))
            result << device.address;
    return result;
}

void Config::addAddress(const QString &address)
{
    QJsonArray list = m_settings.value("addresses").toArray();
    if (list.contains(address))
        return;
    list.append(address);
    m_settings.insert("addresses", list);
    saveSettings();
}

void Config::saveSettings() const
{
    writeJson(dir + "/settings.json", m_settings);
}
