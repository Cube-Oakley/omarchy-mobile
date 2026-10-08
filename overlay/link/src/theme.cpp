#include "theme.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <sys/stat.h>

namespace {

const QRegularExpression ThemeName(QStringLiteral("^[a-z0-9_][a-z0-9._+-]*$"));
const QRegularExpression ImageFile(QStringLiteral("^[^/]+\\.(jpe?g|png|gif|bmp|webp)$"), QRegularExpression::CaseInsensitiveOption);

QString readTrimmed(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
}

// Modification time of the path itself (a background symlink is replaced,
// not its target).
qint64 changedAt(const QString &path)
{
    struct stat info;
    if (lstat(QFile::encodeName(path).constData(), &info) != 0)
        return 0;
    return qint64(info.st_mtim.tv_sec) * 1000 + info.st_mtim.tv_nsec / 1000000;
}

QString env(const char *name, const QString &fallback)
{
    const QString value = qEnvironmentVariable(name);
    return value.isEmpty() ? fallback : value;
}

} // namespace

Theme::Theme(bool phone, QObject *parent)
    : QObject(parent)
    , m_phone(phone)
    , m_home(QDir::homePath())
{
    const QString state = env("XDG_STATE_HOME", m_home + "/.local/state");
    m_state = phone ? state + "/omarchy-mobile" : state + "/omarchy/current";
    // Setting a theme rewrites several files over a second or two.
    m_settle.setSingleShot(true);
    m_settle.setInterval(1500);
    connect(&m_settle, &QTimer::timeout, this, [this]() {
        rewatch();
        const QJsonObject now = current();
        const QString key = now.value("name").toString() + '\n' + now.value("background").toString();
        if (key != m_last) {
            m_last = key;
            emit changed();
        }
    });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_settle, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_settle, qOverload<>(&QTimer::start));
    const QJsonObject now = current();
    m_last = now.value("name").toString() + '\n' + now.value("background").toString();
    rewatch();
}

void Theme::rewatch()
{
    QDir().mkpath(m_state);
    const QStringList wanted = m_phone
        ? QStringList{m_state, m_state + "/selected-theme", m_state + "/backgrounds.json"}
        : QStringList{m_state, m_state + "/theme.name"};
    for (const QString &path : wanted)
        if (QFileInfo::exists(path) && !m_watcher.files().contains(path) && !m_watcher.directories().contains(path))
            m_watcher.addPath(path);
}

QJsonObject Theme::current() const
{
    QString name, background;
    qint64 time = 0;
    if (m_phone) {
        name = readTrimmed(m_state + "/selected-theme");
        const QJsonObject chosen = QJsonDocument::fromJson(readTrimmed(m_state + "/backgrounds.json").toUtf8()).object();
        background = QFileInfo(chosen.value(name).toString()).fileName();
        time = qMax(changedAt(m_state + "/selected-theme"), changedAt(m_state + "/backgrounds.json"));
    } else {
        name = readTrimmed(m_state + "/theme.name");
        background = QFileInfo(QFile::symLinkTarget(m_state + "/background")).fileName();
        time = qMax(changedAt(m_state + "/theme.name"), changedAt(m_state + "/background"));
    }
    if (!validName(name))
        return {};
    return {{"name", name}, {"background", background}, {"time", time}};
}

bool Theme::validName(const QString &name)
{
    return ThemeName.match(name).hasMatch();
}

QString Theme::themeDir(const QString &name) const
{
    const QString config = env("XDG_CONFIG_HOME", m_home + "/.config");
    const QString data = env("XDG_DATA_HOME", m_home + "/.local/share");
    const QString omarchy = env("OMARCHY_PATH", "/usr/share/omarchy");
    const QStringList roots = m_phone
        ? QStringList{config + "/omarchy/themes", data + "/omarchy-mobile/themes", omarchy + "/themes"}
        : QStringList{config + "/omarchy/themes", omarchy + "/themes", data + "/omarchy/themes"};
    // Omarchy also takes a theme with only an alacritty.toml (its palette
    // is converted when it is set); the phone's tools need colors.toml.
    for (const QString &root : roots)
        if (QFileInfo::exists(root + "/" + name + "/colors.toml") ||
            (!m_phone && QFileInfo::exists(root + "/" + name + "/alacritty.toml")))
            return root + "/" + name;
    return {};
}

QString Theme::backgroundsDir(const QString &name) const
{
    return env("XDG_CONFIG_HOME", m_home + "/.config") + "/omarchy/backgrounds/" + name;
}

QString Theme::backgroundPath(const QString &name, const QString &file) const
{
    const QString dir = themeDir(name);
    const QStringList candidates{
        backgroundsDir(name) + "/" + file,
        dir.isEmpty() ? QString() : dir + "/backgrounds/" + file,
        m_phone ? QString() : m_state + "/theme/backgrounds/" + file,
    };
    for (const QString &path : candidates)
        if (!path.isEmpty() && QFileInfo(path).isFile())
            return path;
    return {};
}

bool Theme::has(const QString &name, const QString &background) const
{
    if (!validName(name) || themeDir(name).isEmpty())
        return false;
    return background.isEmpty() || !ImageFile.match(background).hasMatch() || !backgroundPath(name, background).isEmpty();
}

void Theme::apply(const QJsonObject &theme, std::function<void(const QString &)> done)
{
    const QString name = theme.value("name").toString();
    const QString background = theme.value("background").toString();
    if (!validName(name)) {
        done("not a theme name: " + name);
        return;
    }
    if (themeDir(name).isEmpty()) {
        done(name + " is not installed here");
        return;
    }
    const QString file = ImageFile.match(background).hasMatch() ? background : QString();
    const QJsonObject now = current();
    auto finish = [this, name, file, now, done](const QString &error, const QByteArray &) {
        if (!error.isEmpty() || file.isEmpty() || (now.value("name") == name && now.value("background") == file)) {
            done(error);
            return;
        }
        setBackground(name, file, done);
    };
    if (now.value("name").toString() == name)
        finish({}, {});
    else
        run(m_phone ? QStringList{m_home + "/.local/bin/omarchy-mobile-theme", "set", name} : QStringList{"omarchy-theme-set", name},
            60000, finish);
}

void Theme::setBackground(const QString &name, const QString &file, std::function<void(const QString &)> done)
{
    auto finished = [done](const QString &error, const QByteArray &) { done(error); };
    if (m_phone) {
        run({m_home + "/.local/bin/omarchy-mobile-theme", "background", "set", file}, 30000, finished);
        return;
    }
    const QString path = backgroundPath(name, file);
    if (path.isEmpty()) {
        done("no background " + file + " in " + name);
        return;
    }
    run({"omarchy-theme-bg-set", path}, 30000, finished);
}

void Theme::pack(const QString &name, std::function<void(const QByteArray &, const QString &)> done)
{
    const QString dir = themeDir(name);
    if (!validName(name) || dir.isEmpty()) {
        done({}, name + " is not installed here");
        return;
    }
    // A staging folder of two links, archived through them (-h), so the
    // archive holds theme/ and backgrounds/ whatever the theme is called.
    const QString cache = env("XDG_CACHE_HOME", m_home + "/.cache") + "/omarchy-link";
    const QString stage = cache + "/pack-" + name;
    QDir(stage).removeRecursively();
    QDir().mkpath(stage + "/extra");
    QFile::link(dir, stage + "/theme");
    QStringList args{"-C", stage, "-chzf", "-", "--exclude=.git", "--ignore-failed-read", "theme"};
    if (QFileInfo(backgroundsDir(name)).isDir()) {
        QFile::link(backgroundsDir(name), stage + "/backgrounds");
        args << "backgrounds";
    }
    // An older theme with only alacritty.toml gets the colors.toml Omarchy
    // would make from it.
    if (!QFileInfo::exists(dir + "/colors.toml") && QFileInfo::exists(dir + "/alacritty.toml")) {
        QFile::copy(dir + "/alacritty.toml", stage + "/extra/alacritty.toml");
        QProcess convert;
        convert.start("omarchy-theme-colors-from-alacritty", {stage + "/extra"});
        convert.waitForFinished(10000);
        if (QFileInfo::exists(stage + "/extra/colors.toml"))
            args << "-C" << stage + "/extra" << "--transform=s,^colors\\.toml$,theme/colors.toml," << "colors.toml";
    }
    // Broken links (themes carry ones into their author's home) are left
    // out: tar's status 1 is that warning.
    run(QStringList{"sh", "-c", "tar \"$@\"; [ $? -le 1 ]", "tar"} + args, 120000,
        [stage, done](const QString &error, const QByteArray &archive) {
            QDir(stage).removeRecursively();
            done(error.isEmpty() ? archive : QByteArray(), error);
        });
}

void Theme::unpack(const QString &name, const QByteArray &archive, std::function<void(const QString &)> done)
{
    if (!validName(name)) {
        done("not a theme name: " + name);
        return;
    }
    const QString cache = env("XDG_CACHE_HOME", m_home + "/.cache") + "/omarchy-link";
    const QString file = cache + "/theme-" + name + ".tar.gz";
    const QString stage = cache + "/unpack-" + name;
    QDir().mkpath(cache);
    QFile out(file);
    if (!out.open(QIODevice::WriteOnly) || out.write(archive) != archive.size()) {
        done("cannot write " + file);
        return;
    }
    out.close();
    // Only plain files and folders under theme/ and backgrounds/: no links,
    // no absolute paths, no "..".
    run({"tar", "-tvzf", file}, 60000, [this, name, file, stage, done](const QString &error, const QByteArray &listing) {
        static const QRegularExpression path(QStringLiteral("^(theme|backgrounds)(/|$)"));
        QString refused = error;
        for (const QByteArray &raw : listing.split('\n')) {
            const QString line = QString::fromUtf8(raw);
            if (line.isEmpty() || !refused.isEmpty())
                continue;
            const int at = line.indexOf(QRegularExpression(QStringLiteral(" \\d\\d:\\d\\d ")));
            const QString entry = at < 0 ? QString() : line.mid(at + 7);
            if ((line[0] != '-' && line[0] != 'd') || !path.match(entry).hasMatch() || entry.split('/').contains(".."))
                refused = "refused an entry in the archive: " + line;
        }
        if (!refused.isEmpty()) {
            QFile::remove(file);
            done(refused);
            return;
        }
        QDir(stage).removeRecursively();
        QDir().mkpath(stage);
        run({"tar", "-xzf", file, "-C", stage, "--no-same-owner", "--no-same-permissions"}, 120000,
            [this, name, file, stage, done](const QString &error, const QByteArray &) {
                QFile::remove(file);
                if (!error.isEmpty() || !QFileInfo::exists(stage + "/theme/colors.toml")) {
                    QDir(stage).removeRecursively();
                    done(error.isEmpty() ? name + " has no colors.toml" : error);
                    return;
                }
                const QString themes = env("XDG_CONFIG_HOME", m_home + "/.config") + "/omarchy/themes";
                QDir().mkpath(themes);
                QDir(themes + "/" + name).removeRecursively();
                bool moved = QDir().rename(stage + "/theme", themes + "/" + name);
                if (moved && QFileInfo(stage + "/backgrounds").isDir()) {
                    // The user's backgrounds join any already here.
                    const QString target = backgroundsDir(name);
                    QDir().mkpath(target);
                    const QDir source(stage + "/backgrounds");
                    for (const QString &image : source.entryList(QDir::Files)) {
                        QFile::remove(target + "/" + image);
                        QFile::rename(source.filePath(image), target + "/" + image);
                    }
                }
                QDir(stage).removeRecursively();
                done(moved ? QString() : "cannot install " + name + " in " + themes);
            });
    });
}

void Theme::run(const QStringList &command, int timeoutMs, std::function<void(const QString &, const QByteArray &)> done)
{
    auto *process = new QProcess(this);
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, process, &QProcess::kill);
    connect(process, &QProcess::finished, this, [process, command, done](int code, QProcess::ExitStatus status) {
        const QByteArray output = process->readAllStandardOutput();
        const QString errors = QString::fromUtf8(process->readAllStandardError()).trimmed();
        process->deleteLater();
        if (status != QProcess::NormalExit || code != 0)
            done(QFileInfo(command.first()).fileName() + " failed: " + (errors.isEmpty() ? QString::fromUtf8(output) : errors).right(300),
                 {});
        else
            done({}, output);
    });
    connect(process, &QProcess::errorOccurred, this, [process, command, done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            process->deleteLater();
            done("cannot run " + command.first(), {});
        }
    });
    process->start(command.first(), command.mid(1));
    timer->start(timeoutMs);
}
