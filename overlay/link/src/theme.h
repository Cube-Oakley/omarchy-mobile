#pragma once

#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

#include <functional>

// The Omarchy theme and background on this end, read and set through each
// side's own tools: omarchy-theme-set and friends on a computer,
// omarchy-mobile-theme on the phone. A theme the other end picked that is
// missing here comes over the link as an archive of its folder and its
// user backgrounds (no git or network needed), installed under
// ~/.config/omarchy/themes and ~/.config/omarchy/backgrounds as Omarchy
// keeps them.
class Theme : public QObject
{
    Q_OBJECT
public:
    explicit Theme(bool phone, QObject *parent = nullptr);

    // {name, background, time}: time is when it last changed here (ms).
    QJsonObject current() const;
    // The theme and, when named, its background are here.
    bool has(const QString &name, const QString &background) const;
    void apply(const QJsonObject &theme, std::function<void(const QString &error)> done);
    // theme/ (the theme's folder, without .git) and backgrounds/ (the
    // user's backgrounds for it) as a gzip tar.
    void pack(const QString &name, std::function<void(const QByteArray &archive, const QString &error)> done);
    void unpack(const QString &name, const QByteArray &archive, std::function<void(const QString &error)> done);

    static bool validName(const QString &name);

signals:
    void changed();

private:
    QString themeDir(const QString &name) const;
    QString backgroundsDir(const QString &name) const;
    QString backgroundPath(const QString &name, const QString &file) const;
    void setBackground(const QString &name, const QString &file, std::function<void(const QString &)> done);
    void run(const QStringList &command, int timeoutMs, std::function<void(const QString &error, const QByteArray &output)> done);
    void rewatch();

    bool m_phone;
    QString m_home;
    QString m_state;
    QFileSystemWatcher m_watcher;
    QTimer m_settle;
    QString m_last;
};
