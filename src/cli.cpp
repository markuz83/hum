#include "cli.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QJsonDocument>
#include <QUrl>
#include <QVariantMap>

namespace Cli {

namespace {
const char *DefaultService = "org.mpris.MediaPlayer2.hum";
const char *ObjectPath = "/org/mpris/MediaPlayer2";
const char *PlayerInterface = "org.mpris.MediaPlayer2.Player";

const QStringList Commands{"status", "play", "pause", "toggle", "next", "previous", "stop", "volume", "seek"};

QDBusInterface player(const QString &service) {
    return QDBusInterface(service.isEmpty() ? DefaultService : service, ObjectPath, PlayerInterface, QDBusConnection::sessionBus());
}

QString mmss(qint64 ms) {
    const qint64 total = ms / 1000;
    return QString("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QChar('0'));
}
}  // namespace

// Titles come from a file's tags. Printed raw, a tag holding a terminal escape
// sequence could rewrite the screen, so control characters become spaces.
QString printable(const QString &text) {
    QString out = text;
    for (QChar &c : out)
        if (c.unicode() < 0x20 || (c.unicode() >= 0x7f && c.unicode() < 0xa0))
            c = ' ';
    return out;
}

bool isCommand(const QString &word) {
    return Commands.contains(word);
}

QString usage() {
    return QObject::tr(
        "Control a running Hum:\n"
        "  hum status [--json]   what is playing\n"
        "  hum play | pause | toggle | stop\n"
        "  hum next | previous\n"
        "  hum volume [N | +N | -N]   show or set the volume, in percent\n"
        "  hum seek N | +N | -N       jump to N seconds, or move by N");
}

QJsonObject status(const QString &service, bool *running) {
    QDBusInterface iface = player(service);
    if (running)
        *running = iface.isValid();
    if (!iface.isValid())
        return {{"running", false}};
    const QString state = iface.property("PlaybackStatus").toString();
    // Metadata arrives as a D-Bus map; read it through the properties interface.
    QDBusInterface props(service.isEmpty() ? DefaultService : service, ObjectPath, "org.freedesktop.DBus.Properties",
                         QDBusConnection::sessionBus());
    QVariantMap metadata;
    const QDBusReply<QVariant> reply = props.call("Get", PlayerInterface, "Metadata");
    if (reply.isValid()) {
        const QDBusArgument arg = reply.value().value<QDBusArgument>();
        arg >> metadata;
    }
    auto text = [&](const char *key) {
        const QVariant value = metadata.value(key);
        return value.canConvert<QStringList>() && value.typeId() == QMetaType::QStringList ? value.toStringList().join(", ")
                                                                                         : value.toString();
    };
    const QString loop = iface.property("LoopStatus").toString();
    return {
        {"running", true},
        {"state", state},
        {"title", text("xesam:title")},
        {"artist", text("xesam:artist")},
        {"album", text("xesam:album")},
        {"file", QUrl(text("xesam:url")).toLocalFile()},
        {"position", double(iface.property("Position").toLongLong() / 1000)},
        {"duration", double(metadata.value("mpris:length").toLongLong() / 1000)},
        {"volume", iface.property("Volume").toDouble()},
        {"shuffle", iface.property("Shuffle").toBool()},
        {"repeat", loop == "Track" ? "one" : loop == "Playlist" ? "all" : "off"},
    };
}

int run(const QStringList &args, QTextStream &out, QTextStream &err, const QString &service) {
    const QString command = args.value(0);
    if (!isCommand(command)) {
        err << QObject::tr("Unknown command: %1").arg(command) << "\n" << usage() << "\n";
        return 1;
    }
    const bool json = args.contains("--json");
    QStringList rest = args.mid(1);
    rest.removeAll("--json");

    bool running = false;
    const QJsonObject now = status(service, &running);
    if (!running) {
        // A script reading --json still gets an answer it can parse.
        if (json)
            out << QJsonDocument(now).toJson(QJsonDocument::Compact) << "\n";
        err << QObject::tr("Hum isn't running.") << "\n";
        return 1;
    }
    QDBusInterface iface = player(service);

    if (command == "status") {
        if (json) {
            out << QJsonDocument(now).toJson(QJsonDocument::Compact) << "\n";
        } else if (now.value("title").toString().isEmpty()) {
            out << now.value("state").toString() << "\n";
        } else {
            out << now.value("state").toString() << ": " << printable(now.value("title").toString());
            if (!now.value("artist").toString().isEmpty())
                out << " — " << printable(now.value("artist").toString());
            out << " (" << mmss(qint64(now.value("position").toDouble())) << " / " << mmss(qint64(now.value("duration").toDouble()))
                << ")\n";
        }
        return 0;
    }

    if (command == "volume") {
        if (!rest.isEmpty()) {
            bool ok = false;
            const double value = rest.first().toDouble(&ok);
            if (!ok) {
                err << QObject::tr("Volume must be a number, such as 60, +5 or -5.") << "\n";
                return 1;
            }
            const bool relative = rest.first().startsWith('+') || rest.first().startsWith('-');
            const double target = std::clamp((relative ? now.value("volume").toDouble() * 100 + value : value) / 100.0, 0.0, 1.0);
            iface.setProperty("Volume", target);
            out << qRound(target * 100) << "\n";
        } else {
            out << qRound(now.value("volume").toDouble() * 100) << "\n";
        }
        return 0;
    }

    if (command == "seek") {
        bool ok = false;
        const double value = rest.value(0).toDouble(&ok);
        if (!ok) {
            err << QObject::tr("Seek needs a number of seconds, such as 90, +10 or -10.") << "\n";
            return 1;
        }
        const bool relative = rest.first().startsWith('+') || rest.first().startsWith('-');
        const qint64 target = relative ? qint64(now.value("position").toDouble()) + qint64(value * 1000) : qint64(value * 1000);
        iface.call("Seek", qlonglong((target - qint64(now.value("position").toDouble())) * 1000));
        return 0;
    }

    static const QHash<QString, QString> methods{{"play", "Play"}, {"pause", "Pause"}, {"toggle", "PlayPause"},
                                                  {"stop", "Stop"}, {"next", "Next"}, {"previous", "Previous"}};
    const QDBusMessage reply = iface.call(methods.value(command));
    if (reply.type() == QDBusMessage::ErrorMessage) {
        err << reply.errorMessage() << "\n";
        return 1;
    }
    return 0;
}

}  // namespace Cli
