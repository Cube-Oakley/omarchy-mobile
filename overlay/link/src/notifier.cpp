#include "notifier.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

namespace {
const QString Service = QStringLiteral("org.freedesktop.Notifications");
const QString Path = QStringLiteral("/org/freedesktop/Notifications");
} // namespace

Notifier::Notifier(QObject *parent)
    : QObject(parent)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(QString(), Path, Service, "ActionInvoked", this, SLOT(onActionInvoked(uint, QString)));
    bus.connect(QString(), Path, Service, "NotificationClosed", this, SLOT(onClosed(uint, uint)));
}

void Notifier::notify(const Note &note, std::function<void(uint)> done)
{
    QDBusMessage call = QDBusMessage::createMethodCall(Service, Path, Service, "Notify");
    call << note.app << note.replaces << note.icon << note.summary << note.body << note.actions << note.hints
         << note.timeout;
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, 5000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [watcher, done]() {
        QDBusPendingReply<uint> reply = *watcher;
        if (done)
            done(reply.isError() ? 0 : reply.value());
        watcher->deleteLater();
    });
}

void Notifier::close(uint id)
{
    if (!id)
        return;
    QDBusMessage call = QDBusMessage::createMethodCall(Service, Path, Service, "CloseNotification");
    call << id;
    QDBusConnection::sessionBus().asyncCall(call, 5000);
}

void Notifier::onActionInvoked(uint id, const QString &key)
{
    emit actionInvoked(id, key);
}

void Notifier::onClosed(uint id, uint reason)
{
    emit closed(id, reason);
}
