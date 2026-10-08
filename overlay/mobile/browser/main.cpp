// SPDX-License-Identifier: MIT
// Omarchy Mobile's browser: Chromium's engine through Qt WebEngine, with a
// phone interface in QML (Browser.qml). Quickshell cannot host WebEngine
// (it must be initialized before the application object exists), so this
// small program does that and gives the QML what Quickshell would:
// - the Omarchy theme (palette.json, which the shell keeps current);
// - the shell's back gesture (the stamp it writes in the runtime directory,
//   acted on only while this window has the focus);
// - the on-screen keyboard (omarchy-mobile-keyboard show|hide);
// - one persistent profile: cookies and cache kept, a phone's user agent,
//   downloads to ~/Downloads, and a content blocker fed by a domain list
//   (~/.local/share/omarchy-mobile/browser/blocklist.txt, hosts or plain
//   domains; empty without one);
// - web apps: a site added to the home screen gets a desktop entry that
//   starts `--app URL`, a window of its own without the browser's bar, with
//   its own profile and app ID (omarchy-mobile-webapp-SITE);
// - one process each for the browser and every web app: a second start
//   hands its address to the running one over a local socket and exits,
//   and the running one asks the shell to bring its window forward.
// Usage: omarchy-mobile-browser [URL] | --app URL
#include <QColor>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QQuickWindow>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QReadWriteLock>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTimer>
#include <QPointer>
#include <QUrl>
#include <QWebEngineNotification>
#include <QWebEngineUrlRequestInterceptor>
#include <QtWebEngineQuick/QQuickWebEngineProfile>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>
#include <atomic>
#include <fcntl.h>
#include <unistd.h>

static QString home() { return QDir::homePath(); }

static QString runtimeDir()
{
    const QString dir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    return dir.isEmpty() ? QStringLiteral("/tmp") : dir;
}

static QString stateDir()
{
    const QString dir = qEnvironmentVariable("XDG_STATE_HOME");
    return (dir.isEmpty() ? home() + "/.local/state" : dir) + "/omarchy-mobile";
}

static QString dataDir()
{
    const QString dir = qEnvironmentVariable("XDG_DATA_HOME");
    return (dir.isEmpty() ? home() + "/.local/share" : dir) + "/omarchy-mobile/browser";
}

// A web app's name in IDs and file names: its host, without www, in
// lower-case letters, digits and dashes.
static QString siteSlug(const QUrl &url)
{
    QString host = url.host().toLower();
    if (host.startsWith(QStringLiteral("www.")))
        host = host.mid(4);
    host.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    host.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    return host;
}

static QString applicationsDir()
{
    const QString dir = qEnvironmentVariable("XDG_DATA_HOME");
    return (dir.isEmpty() ? home() + "/.local/share" : dir) + "/applications";
}

static QString webAppIcons()
{
    const QString dir = qEnvironmentVariable("XDG_DATA_HOME");
    return (dir.isEmpty() ? home() + "/.local/share" : dir) + "/omarchy-mobile/webapps";
}

static const QByteArray desktopAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 Safari/537.36";

// Every request passes here. Requests to listed domains (and their
// subdomains) are not made: a page the user opened is always loaded, only
// what it pulls in is blocked. Pages of sites asked for "desktop" get a
// desktop browser's user agent (the profile's is a phone's).
class Blocker : public QWebEngineUrlRequestInterceptor
{
public:
    explicit Blocker(QObject *parent) : QWebEngineUrlRequestInterceptor(parent)
    {
        QFile file(dataDir() + "/blocklist.txt");
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return;
        while (!file.atEnd()) {
            QString line = QString::fromUtf8(file.readLine()).trimmed();
            if (line.isEmpty() || line.startsWith('#') || line.startsWith('!'))
                continue;
            const QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")));
            QString domain = parts.size() > 1 ? parts.at(1) : parts.at(0);  // hosts: "0.0.0.0 domain"
            if (domain.startsWith(QStringLiteral("||")))                      // Adblock: "||domain^"
                domain = domain.mid(2).section('^', 0, 0);
            if (domain.contains('.') && domain != QStringLiteral("localhost"))
                domains.insert(domain.toLower());
        }
    }
    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        {
            QReadLocker lock(&desktopLock);
            if (!desktop.isEmpty() && desktop.contains(info.firstPartyUrl().host().toLower()))
                info.setHttpHeader("User-Agent", desktopAgent);
        }
        if (domains.isEmpty() || info.resourceType() == QWebEngineUrlRequestInfo::ResourceTypeMainFrame)
            return;
        QString host = info.requestUrl().host().toLower();
        while (!host.isEmpty()) {
            if (domains.contains(host)) {
                info.block(true);
                blocked++;
                return;
            }
            const int dot = host.indexOf('.');
            if (dot < 0)
                break;
            host = host.mid(dot + 1);
        }
    }
    int count() const { return blocked.load(); }
    int size() const { return domains.size(); }
    void setDesktop(const QString &host, bool on)
    {
        QWriteLocker lock(&desktopLock);
        if (on)
            desktop.insert(host.toLower());
        else
            desktop.remove(host.toLower());
    }
    bool isDesktop(const QString &host)
    {
        QReadLocker lock(&desktopLock);
        return desktop.contains(host.toLower());
    }

private:
    QSet<QString> domains;   // read-only after loading: safe on the IO thread
    QSet<QString> desktop;   // sites in desktop mode, under desktopLock
    QReadWriteLock desktopLock;
    std::atomic<int> blocked{0};
};

class Host : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap theme READ theme NOTIFY themeChanged)
    Q_PROPERTY(QString startUrl READ startUrl CONSTANT)
    Q_PROPERTY(QObject *profile READ profile CONSTANT)
    Q_PROPERTY(QString downloads READ downloads CONSTANT)
    Q_PROPERTY(int blockedCount READ blockedCount NOTIFY blockedChanged)
    Q_PROPERTY(int blocklistSize READ blocklistSize CONSTANT)
    Q_PROPERTY(bool appMode READ appMode CONSTANT)
    Q_PROPERTY(QVariantList siteThemes READ siteThemes CONSTANT)
    // Whether the window is on screen. Hyprland suspends a window on a
    // workspace not shown, and Qt then reports it unexposed; with the
    // display off or in the always-on display the window stays exposed, so
    // the screen's state comes from omarchy-mobile-display's runtime file.
    // Browser.qml hides the pages when not shown, so Chromium treats them as
    // background tabs (paused videos, throttled timers, no frames). Hiding
    // after an unexpose waits 1.5 s, so a quick switch away and back costs
    // nothing.
    Q_PROPERTY(bool shown READ shown NOTIFY shownChanged)

public:
    Host(const QString &start, const QString &app, const QString &id, QObject *parent)
        : QObject(parent), start(start), app(app), id(id)
    {
        // A web app keeps its own cookies and cache: two processes never
        // share a profile's files.
        webProfile = new QQuickWebEngineProfile(app.isEmpty() ? QStringLiteral("omarchy-mobile")
                                                              : QStringLiteral("webapp-") + app, this);
        webProfile->setPersistentCookiesPolicy(QQuickWebEngineProfile::AllowPersistentCookies);
        webProfile->setHttpCacheType(QQuickWebEngineProfile::DiskHttpCache);
        // A phone's: sites send their mobile pages (Browser.qml can ask for
        // the desktop one per tab).
        webProfile->setHttpUserAgent(QStringLiteral(
            "Mozilla/5.0 (Linux; Android 15; Pixel 7 Pro) AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/134.0.0.0 Mobile Safari/537.36"));
        webProfile->setDownloadPath(downloads());
        blocker = new Blocker(this);
        webProfile->setUrlRequestInterceptor(blocker);
        // Web notifications (a site the user allowed) go to the shell's
        // notification server; its "default" action opens the page's own.
        connect(webProfile, &QQuickWebEngineProfile::presentNotification, this, &Host::presentNotification);
        QDBusConnection::sessionBus().connect(notifyService, notifyPath, notifyService, QStringLiteral("ActionInvoked"),
                                              this, SLOT(notificationAction(uint,QString)));
        QDBusConnection::sessionBus().connect(notifyService, notifyPath, notifyService, QStringLiteral("NotificationClosed"),
                                              this, SLOT(notificationClosed(uint,uint)));

        palette = stateDir() + "/palette.json";
        stampDir = runtimeDir() + "/omarchy-mobile";
        stamp = stampDir + "/back";
        lastStamp = readFile(stamp);
        watcher.addPath(stateDir());
        if (QFile::exists(palette))
            watcher.addPath(palette);
        QDir().mkpath(stampDir);
        watcher.addPath(stampDir);
        if (QFile::exists(stamp))
            watcher.addPath(stamp);
        connect(&watcher, &QFileSystemWatcher::fileChanged, this, &Host::changed);
        connect(&watcher, &QFileSystemWatcher::directoryChanged, this, &Host::changed);
        loadTheme();
    }

    QVariantMap theme() const { return themeMap; }
    QString startUrl() const { return start; }
    QObject *profile() const { return webProfile; }
    QString downloads() const
    {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        return dir.isEmpty() ? home() + "/Downloads" : dir;
    }
    int blockedCount() const { return blocker->count(); }
    int blocklistSize() const { return blocker->size(); }
    bool appMode() const { return !app.isEmpty(); }
    // Themes for further sites, one JSON file each in
    // ~/.config/omarchy-mobile/site-themes (README: Web apps).
    QVariantList siteThemes() const
    {
        QVariantList list;
        const QDir dir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/omarchy-mobile/site-themes");
        for (const QString &name : dir.entryList({"*.json"}, QDir::Files, QDir::Name)) {
            const QJsonDocument document = QJsonDocument::fromJson(readFile(dir.filePath(name)).toUtf8());
            if (document.isObject())
                list << document.object().toVariantMap();
        }
        return list;
    }

    // Web apps. The icon is saved by the QML (an image of the site's icon)
    // at webAppIconPath(); addWebApp() writes the desktop entry.
    Q_INVOKABLE QString webAppIconPath(const QString &url) const
    {
        const QString slug = siteSlug(QUrl(url));
        if (slug.isEmpty())
            return QString();
        QDir().mkpath(webAppIcons());
        return webAppIcons() + "/" + slug + ".png";
    }
    Q_INVOKABLE bool addWebApp(const QString &name, const QString &url) const
    {
        const QUrl address(url);
        const QString slug = siteSlug(address);
        if (slug.isEmpty() || (address.scheme() != QLatin1String("https") && address.scheme() != QLatin1String("http")))
            return false;
        // The address as percent-encoded ASCII; anything the desktop entry's
        // quoting would have to escape is refused rather than escaped.
        QString encoded = QString::fromLatin1(address.toEncoded()).replace(QLatin1Char('$'), QStringLiteral("%24"));
        if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9._~:/?#\\[\\]@!&'()*+,;=%-]+$")).match(encoded).hasMatch())
            return false;
        QString title = name.simplified();
        title.remove(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]")));
        title = title.left(60);
        if (title.isEmpty())
            title = address.host();
        const QString icon = webAppIcons() + "/" + slug + ".png";
        QString entry = QStringLiteral("[Desktop Entry]\nType=Application\nName=") + title +
            QStringLiteral("\nExec=") + home() + QStringLiteral("/.local/bin/omarchy-mobile-browser --app \"") +
            encoded.replace(QLatin1Char('%'), QStringLiteral("%%")) + QStringLiteral("\"\n");
        if (QFile::exists(icon))
            entry += QStringLiteral("Icon=") + icon + QStringLiteral("\n");
        entry += QStringLiteral("Categories=Network;WebBrowser;\nX-Omarchy-Mobile-WebApp=") +
                 QString::fromLatin1(address.toEncoded()) + QStringLiteral("\n");
        QDir().mkpath(applicationsDir());
        const QString path = applicationsDir() + "/omarchy-mobile-webapp-" + slug + ".desktop";
        QFile file(path + ".tmp");
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        file.write(entry.toUtf8());
        file.close();
        QFile::remove(path);
        return file.rename(path);
    }
    // A web app's link to another site, in the browser.
    Q_INVOKABLE void openInBrowser(const QString &url) const
    {
        QProcess::startDetached(home() + "/.local/bin/omarchy-mobile-browser", {url});
    }
    // The shell brings this window forward (its request FIFO; nothing
    // happens without a shell reading it).
    Q_INVOKABLE void raise() const
    {
        if (!askShell("focus " + id.toUtf8()))
            qWarning("browser: the shell did not take the focus request");
    }
    // A line on the shell's request FIFO; false when no shell reads it.
    static bool askShell(const QByteArray &request)
    {
        const QByteArray line = request + "\n";
        const int fd = ::open(QFile::encodeName(runtimeDir() + "/omarchy-mobile-shell.fifo").constData(),
                              O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            return false;
        const bool written = ::write(fd, line.constData(), line.size()) == line.size();
        ::close(fd);
        return written;
    }
    // A second start's address (empty when it had none).
    void handOver(const QString &url) { emit openRequested(url); }

    // The shell's share sheet for this page (omarchy-mobile-share).
    Q_INVOKABLE void share(const QString &url, const QString &title) const
    {
        QProcess::startDetached(home() + "/.local/bin/omarchy-mobile-share",
                                {QStringLiteral("--title"), title, QStringLiteral("--url"), url});
    }
    // A photo for a page's file input: the camera, in its capture mode,
    // writes the shot the user keeps to the returned path (empty when the
    // camera could not be started); the page polls fileReady().
    Q_INVOKABLE QString startCapture() const
    {
        const QString dir = runtimeDir() + "/omarchy-mobile-capture";
        QDir().mkpath(dir);
        const QString path = dir + "/photo-" + QString::number(QDateTime::currentMSecsSinceEpoch()) + ".jpg";
        // Over this window through the shell, which brings it back when the
        // camera closes; started directly without a shell.
        if (askShell("over camera capture:" + path.toUtf8()))
            return path;
        if (!QProcess::startDetached(home() + "/.local/bin/omarchy-mobile-app",
                                     {QStringLiteral("launch"), QStringLiteral("camera"), "capture:" + path}))
            return QString();
        return path;
    }
    Q_INVOKABLE bool fileReady(const QString &path) const
    {
        const QFileInfo info(path);
        return info.isFile() && info.size() > 0;
    }
    // The camera roll's newest pictures, for the file picker.
    Q_INVOKABLE QStringList recentPhotos(int count) const
    {
        const QDir dir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + "/Camera");
        const QFileInfoList files = dir.entryInfoList({QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
                                                       QStringLiteral("*.png")}, QDir::Files, QDir::Time);
        QStringList paths;
        for (int i = 0; i < files.size() && i < count; ++i)
            paths << files.at(i).absoluteFilePath();
        return paths;
    }

    Q_INVOKABLE void keyboard(const QString &action)
    {
        if (action == QLatin1String("show") || action == QLatin1String("hide"))
            QProcess::startDetached(home() + "/.local/bin/omarchy-mobile-keyboard", {action});
    }
    Q_INVOKABLE void copy(const QString &text)
    {
        QProcess::startDetached(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("printf %s \"$1\" | wl-copy"),
                                                       QStringLiteral("copy"), text});
    }
    // Small files of the browser's own in the state directory.
    static bool stateName(const QString &name)
    {
        return QRegularExpression(QStringLiteral("^browser-[a-z0-9-]+\\.json$")).match(name).hasMatch();
    }
    Q_INVOKABLE QString readState(const QString &name) const
    {
        return stateName(name) ? readFile(stateDir() + "/" + name) : QString();
    }
    Q_INVOKABLE void writeState(const QString &name, const QString &text) const
    {
        if (!stateName(name))
            return;
        QDir().mkpath(stateDir());
        QFile file(stateDir() + "/" + name + ".tmp");
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(text.toUtf8());
            file.close();
            QFile::remove(stateDir() + "/" + name);
            file.rename(stateDir() + "/" + name);
        }
    }
    // Page events kept for diagnosis (renderer exits, reloads, navigations):
    // browser-events.log in the state directory, its newest 128-256 KB.
    Q_INVOKABLE void logEvent(const QString &event) const
    {
        QDir().mkpath(stateDir());
        QFile file(stateDir() + "/browser-events.log");
        if (file.size() > 256 * 1024 && file.open(QIODevice::ReadWrite)) {
            QByteArray tail = file.readAll().right(128 * 1024);
            tail = tail.mid(tail.indexOf('\n') + 1);
            file.resize(0);
            file.seek(0);
            file.write(tail);
            file.close();
        }
        if (file.open(QIODevice::Append)) {
            file.write(QStringLiteral("%1 %2 %3\n")
                           .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
                                QGuiApplication::desktopFileName(), event)
                           .toUtf8());
        }
    }
    Q_INVOKABLE void refreshBlocked() { emit blockedChanged(); }
    void useDesktopAgent() { webProfile->setHttpUserAgent(QString::fromLatin1(desktopAgent)); }

public slots:
    void notificationAction(uint shellId, const QString &action)
    {
        QWebEngineNotification *notification = notifications.value(shellId);
        if (!notification || action != QLatin1String("default"))
            return;
        raise();
        notification->click();
    }
    void notificationClosed(uint shellId, uint)
    {
        if (QWebEngineNotification *notification = notifications.take(shellId))
            notification->close();
    }

public:
    bool shown() const { return isShown; }
    void watch(QWindow *window)
    {
        watched = window;
        window->installEventFilter(this);
        hideTimer.setSingleShot(true);
        hideTimer.setInterval(1500);
        connect(&hideTimer, &QTimer::timeout, this, [this] { exposed = false; updateShown(); });
        updateShown();
    }
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (object == watched && event->type() == QEvent::Expose) {
            if (watched->isExposed()) {
                hideTimer.stop();
                exposed = true;
                updateShown();
            } else if (exposed && !hideTimer.isActive()) {
                hideTimer.start();
            }
        }
        return QObject::eventFilter(object, event);
    }
    // Pages shown by the compositor directly (see main()), or drawn by Qt:
    // a picture of a page (grabToImage) needs Qt to draw it.
    bool directPagesAllowed = false;
    Q_INVOKABLE void directPages(bool on)
    {
        if (!watched)
            return;
        watched->setProperty("_q_webengine_direct", on && directPagesAllowed);
        watched->requestUpdate();
    }
    // A site's pages with a desktop browser's user agent (reload to apply).
    Q_INVOKABLE void setDesktop(const QString &site, bool on) { blocker->setDesktop(site, on); }
    Q_INVOKABLE bool isDesktop(const QString &site) const { return blocker->isDesktop(site); }

signals:
    void themeChanged();
    void shownChanged();
    void backRequested();
    void blockedChanged();
    void openRequested(const QString &url);

private:
    static inline const QString notifyService = QStringLiteral("org.freedesktop.Notifications");
    static inline const QString notifyPath = QStringLiteral("/org/freedesktop/Notifications");
    void presentNotification(QWebEngineNotification *notification)
    {
        // A notification with the tag of one still shown replaces it.
        uint replaces = 0;
        for (auto it = notifications.cbegin(); it != notifications.cend(); ++it) {
            if (it.value() && notification->matches(it.value())) {
                replaces = it.key();
                break;
            }
        }
        QVariantMap hints;
        hints.insert(QStringLiteral("desktop-entry"), QGuiApplication::desktopFileName());
        QDBusInterface server(notifyService, notifyPath, notifyService, QDBusConnection::sessionBus());
        const QDBusReply<uint> shellId = server.call(QStringLiteral("Notify"), appTitle(), replaces, QString(),
                                                     notification->title(), notification->message(),
                                                     QStringList{QStringLiteral("default"), QStringLiteral("Open")},
                                                     hints, -1);
        if (!shellId.isValid())
            return;
        notifications.insert(shellId.value(), notification);
        notification->show();
        // Closed by the page: take it off the shade too.
        connect(notification, &QWebEngineNotification::closed, this, [this, id = shellId.value()] {
            if (notifications.take(id))
                QDBusInterface(notifyService, notifyPath, notifyService, QDBusConnection::sessionBus())
                    .call(QStringLiteral("CloseNotification"), id);
        });
    }
    // The web app's name from its desktop entry (the browser: its own).
    QString appTitle() const
    {
        const QString entry = readFile(applicationsDir() + "/" + QGuiApplication::desktopFileName() + ".desktop");
        const QRegularExpressionMatch name = QRegularExpression(QStringLiteral("^Name=(.+)$"),
                                                                QRegularExpression::MultilineOption).match(entry);
        return name.hasMatch() ? name.captured(1).trimmed() : QStringLiteral("Browser");
    }
    QHash<uint, QPointer<QWebEngineNotification>> notifications;
    void updateShown()
    {
        // No file yet (the display script has not run since boot): on.
        const QString screen = readFile(stampDir + "/screen");
        const bool on = exposed && screen != QLatin1String("off") && screen != QLatin1String("ambient");
        if (on == isShown)
            return;
        isShown = on;
        qInfo("browser: window %s", on ? "shown" : "hidden");
        emit shownChanged();
    }
    bool isShown = true;
    bool exposed = true;
    QWindow *watched = nullptr;
    QTimer hideTimer;
    static QString readFile(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
    }
    void loadTheme()
    {
        const QJsonDocument doc = QJsonDocument::fromJson(readFile(palette).toUtf8());
        if (doc.isObject() && doc.object().toVariantMap() != themeMap) {
            themeMap = doc.object().toVariantMap();
            // Pages see the theme as their preferred colour scheme
            // (prefers-color-scheme), so sites pick their dark or light look.
            const QColor background(themeMap.value(QStringLiteral("colors")).toMap()
                                        .value(QStringLiteral("background")).toString());
            if (background.isValid())
                QGuiApplication::styleHints()->setColorScheme(
                    background.lightnessF() < 0.5 ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
            emit themeChanged();
        }
    }
    // Files replaced by rename drop out of the watch: they are added again.
    void changed(const QString &)
    {
        if (QFile::exists(palette) && !watcher.files().contains(palette))
            watcher.addPath(palette);
        if (QFile::exists(stamp) && !watcher.files().contains(stamp))
            watcher.addPath(stamp);
        loadTheme();
        updateShown();
        const QString now = readFile(stamp);
        if (!now.isEmpty() && now != lastStamp) {
            lastStamp = now;
            if (focused())
                emit backRequested();
        }
    }
    // The back gesture is for the window on screen: this one, if Hyprland's
    // focused window is this process.
    bool focused() const
    {
        QProcess hyprctl;
        hyprctl.start(QStringLiteral("hyprctl"), {QStringLiteral("activewindow"), QStringLiteral("-j")});
        if (!hyprctl.waitForFinished(1000))
            return false;
        const QJsonObject window = QJsonDocument::fromJson(hyprctl.readAllStandardOutput()).object();
        return window.value(QStringLiteral("pid")).toInteger() == QCoreApplication::applicationPid();
    }

    QString start, app, id, palette, stampDir, stamp, lastStamp;
    QVariantMap themeMap;
    QFileSystemWatcher watcher;
    QQuickWebEngineProfile *webProfile;
    Blocker *blocker;
};

int main(int argc, char *argv[])
{
    // Scrolling as on a phone: Chromium resamples a touch scroll to each
    // frame, as Chrome on Android does (scrollbars are off, in Browser.qml).
    // Qt Quick's merging of a frame's touch moves stays on: without it the
    // page drew at 60 instead of 120 frames a second while dragged.
    QByteArray flags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    flags += " --enable-features=ResamplingScrollEvents";
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags.trimmed());
    // With the phone's WebEngine library (qtwebengine/0003), Chromium's frame
    // clock follows Qt's frames. Starting each frame 3.5 ms before Qt's puts
    // X's frames mid-interval instead of just before Qt's next frame: a
    // third fewer refreshes without a new frame in flings (October 5).
    if (!qEnvironmentVariableIsSet("QTWEBENGINE_FRAME_PHASE_US"))
        qputenv("QTWEBENGINE_FRAME_PHASE_US", "-3500");
    QtWebEngineQuick::initialize();
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("omarchy-mobile-browser"));
    app.setDesktopFileName(QStringLiteral("omarchy-mobile-browser"));

    QStringList args = app.arguments().mid(1);
    // --desktop: the whole profile presents itself as a desktop browser, to
    // pages' scripts too (Snapchat's web client sends phones to its app).
    const bool desktopSite = args.removeAll(QStringLiteral("--desktop")) > 0;
    QString webApp;
    if (args.size() == 2 && args.at(0) == QLatin1String("--app")) {
        webApp = siteSlug(QUrl(args.at(1)));
        if (webApp.isEmpty())
            return 2;
        args.removeFirst();
    }
    const QString start = !args.isEmpty() ? args.at(0) : qEnvironmentVariable("OMARCHY_MOBILE_APP_ARG");
    // The Wayland app ID: the shell's drawer and switcher know the window by it.
    const QString id = webApp.isEmpty() ? QStringLiteral("omarchy-mobile-browser")
                                        : QStringLiteral("omarchy-mobile-webapp-") + webApp;
    app.setDesktopFileName(id);

    // One process: a running one takes the address and comes forward.
    const QString instance = runtimeDir() + "/" + id + ".sock";
    {
        QLocalSocket running;
        running.connectToServer(instance);
        if (running.waitForConnected(500)) {
            running.write((start + "\n").toUtf8());
            running.waitForBytesWritten(1000);
            running.disconnectFromServer();
            return 0;
        }
    }
    QLocalServer::removeServer(instance);
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(instance))
        qWarning("browser: no single-instance socket at %s", qPrintable(instance));

    Host host(start, webApp, id, &app);
    if (desktopSite)
        host.useDesktopAgent();
    QObject::connect(&server, &QLocalServer::newConnection, &host, [&server, &host] {
        while (QLocalSocket *other = server.nextPendingConnection()) {
            QObject::connect(other, &QLocalSocket::readyRead, other, [other, &host] {
                if (!other->canReadLine())
                    return;
                host.handOver(QString::fromUtf8(other->readLine()).trimmed());
                other->disconnectFromServer();
            });
            QObject::connect(other, &QLocalSocket::disconnected, other, &QObject::deleteLater);
        }
    });
    // Pages shown by the compositor directly (the phone's WebEngine library,
    // qtwebengine/0007): Chromium's frames go to Hyprland as a sub-surface
    // below the window, which leaves a transparent hole for them, so the
    // window needs an alpha channel. Only what changed is redrawn, in
    // Chromium and Hyprland, and Qt draws nothing for a page's frames.
    // OMARCHY_BROWSER_DIRECT=0 has Qt draw them, as before.
    const bool direct = qEnvironmentVariable("OMARCHY_BROWSER_DIRECT") != QLatin1String("0");
    if (direct)
        QQuickWindow::setDefaultAlphaBuffer(true);
    host.directPagesAllowed = direct;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("host"), &host);
    // Browser.qml beside the program in the source tree, else installed.
    QString qml = QCoreApplication::applicationDirPath() + "/Browser.qml";
    if (!QFile::exists(qml))
        qml = dataDir() + "/Browser.qml";
    engine.load(QUrl::fromLocalFile(qml));
    if (engine.rootObjects().isEmpty())
        return 1;
    if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst())) {
        host.watch(window);
        host.directPages(true);
    }
    return app.exec();
}

#include "main.moc"
