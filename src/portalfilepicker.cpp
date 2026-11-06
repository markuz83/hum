#include "portalfilepicker.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QUrl>

namespace {
const char *Service = "org.freedesktop.portal.Desktop";
}

void PortalFilePicker::pickFolder(const QString &title) {
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        emit failed(tr("no D-Bus session"));
        return;
    }

    // The answer arrives as a signal on a request object whose path is known
    // in advance from our bus name and a token, so listen before asking.
    const QString token = "hum" + QString::number(++m_counter);
    QString sender = bus.baseService();
    sender.remove(0, 1).replace('.', '_');
    const QString handle = QString("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
    bus.connect(Service, handle, "org.freedesktop.portal.Request", "Response", this, SLOT(onResponse(uint, QVariantMap)));

    QDBusMessage call = QDBusMessage::createMethodCall(Service, "/org/freedesktop/portal/desktop",
                                                       "org.freedesktop.portal.FileChooser", "OpenFile");
    call << QString() << title << QVariantMap{{"handle_token", token}, {"directory", true}, {"modal", true}};
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, handle] {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        if (reply.isError()) {
            QDBusConnection::sessionBus().disconnect(Service, handle, "org.freedesktop.portal.Request", "Response",
                                                     this, SLOT(onResponse(uint, QVariantMap)));
            emit failed(reply.error().message());
        }
        watcher->deleteLater();
    });
}

void PortalFilePicker::onResponse(uint response, const QVariantMap &results) {
    // 0 means a choice was made; 1 and 2 are cancelled and ended otherwise.
    if (response != 0)
        return;
    QStringList uris = results.value("uris").toStringList();
    if (uris.isEmpty() && results.value("uris").canConvert<QDBusArgument>())
        uris = qdbus_cast<QStringList>(results.value("uris").value<QDBusArgument>());
    if (uris.isEmpty())
        return;
    emit picked(QUrl(uris.first()).toLocalFile());
}
