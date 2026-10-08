#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantMap>

#include <functional>

// The session's notification server (org.freedesktop.Notifications): the
// Omarchy shell on either end.
class Notifier : public QObject
{
    Q_OBJECT
public:
    explicit Notifier(QObject *parent = nullptr);

    struct Note {
        QString app;
        QString icon;
        QString summary;
        QString body;
        QStringList actions; // key, label, key, label...
        QVariantMap hints;
        int timeout = -1;
        uint replaces = 0;
    };
    // done receives the server's ID, or 0 when there is no server.
    void notify(const Note &note, std::function<void(uint)> done = {});
    void close(uint id);

signals:
    void actionInvoked(uint id, const QString &key);
    void closed(uint id, uint reason);

private slots:
    void onActionInvoked(uint id, const QString &key);
    void onClosed(uint id, uint reason);
};
