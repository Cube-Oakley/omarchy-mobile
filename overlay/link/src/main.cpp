#include "daemon.h"
#include "packet.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QSocketNotifier>
#include <QTextStream>

#include <csignal>
#include <unistd.h>

namespace {

int quitPipe[2] = {-1, -1};

void quitSignal(int)
{
    const char byte = 1;
    (void)!write(quitPipe[1], &byte, 1);
}

const char *Usage = R"(Usage: omarchy-link COMMAND
  daemon                 run the link (one per user)
  state                  this device, the devices seen and paired, the switches
  watch                  print each event as one JSON line
  pair|accept|reject|unpair|ping|ring DEVICE
  announce               announce this device on the networks now
  connect ADDRESS        also announce to ADDRESS (a device on another network)
  feature NAME on|off    notifications, sms, battery, ring, theme.send, theme.follow
  call JSON              send one command object, print the reply
)";

int client(const QStringList &args)
{
    QJsonObject command;
    const QString verb = args.value(0);
    if (verb == "state" || verb == "announce") {
        command = {{"cmd", verb}};
    } else if (verb == "watch") {
        command = {{"cmd", "subscribe"}};
    } else if (QStringList{"pair", "accept", "reject", "unpair", "ping", "ring"}.contains(verb) && args.size() == 2) {
        command = {{"cmd", verb}, {"device", args[1]}};
    } else if (verb == "connect" && args.size() == 2) {
        command = {{"cmd", "connect"}, {"address", args[1]}};
    } else if (verb == "feature" && args.size() == 3) {
        command = {{"cmd", "feature"}, {"name", args[1]}, {"on", args[2] == "on"}};
    } else if (verb == "call" && args.size() == 2) {
        command = QJsonDocument::fromJson(args[1].toUtf8()).object();
    }
    if (command.isEmpty()) {
        QTextStream(stderr) << Usage;
        return 2;
    }
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty())
        runtime = QDir::tempPath();
    QLocalSocket socket;
    socket.connectToServer(runtime + "/omarchy-link.sock");
    if (!socket.waitForConnected(2000)) {
        QTextStream(stderr) << "omarchy-link: the daemon is not running\n";
        return 1;
    }
    socket.write(jsonLine(command));
    socket.flush();
    QTextStream out(stdout);
    const bool follow = verb == "watch";
    while (socket.waitForReadyRead(follow ? -1 : 5000)) {
        while (socket.canReadLine()) {
            const QByteArray line = socket.readLine();
            out << line;
            out.flush();
            const QJsonObject object = QJsonDocument::fromJson(line).object();
            if (!follow && (object.contains("reply") || object.value("event") == "state"))
                return object.value("ok").toBool(true) ? 0 : 1;
        }
    }
    return follow ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("omarchy-link");
    const QStringList args = app.arguments().mid(1);
    if (args.value(0) != "daemon")
        return client(args);

    std::signal(SIGPIPE, SIG_IGN);
    // A clean exit on TERM, so links close instead of timing out.
    if (pipe(quitPipe) == 0) {
        auto *notifier = new QSocketNotifier(quitPipe[0], QSocketNotifier::Read, &app);
        QObject::connect(notifier, &QSocketNotifier::activated, &app, &QCoreApplication::quit);
        std::signal(SIGTERM, quitSignal);
        std::signal(SIGINT, quitSignal);
    }
    Daemon daemon;
    QString error;
    if (!daemon.start(&error)) {
        QTextStream(stderr) << "omarchy-link: " << error << '\n';
        return 1;
    }
    return app.exec();
}
