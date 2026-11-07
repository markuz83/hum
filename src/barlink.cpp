#include "barlink.h"

#include <QLocalSocket>
#include <QStandardPaths>
#include <QRegularExpression>
#include <algorithm>

#include "backend.h"

BarLink::BarLink(Backend *backend, const QString &path, int bars, QObject *parent)
    : QObject(parent), m_backend(backend), m_bars(bars) {
    const QString socket = !path.isEmpty() ? path : QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + "/hum-bar.sock";
    // Only the user may connect, and a socket left by a crashed run is replaced.
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    QLocalServer::removeServer(socket);
    if (!m_server.listen(socket))
        return;
    connect(&m_server, &QLocalServer::newConnection, this, &BarLink::accept);
    connect(backend, &Backend::playingChanged, this, &BarLink::sendState);
    connect(backend, &Backend::currentChanged, this, &BarLink::sendState);
    connect(backend, &Backend::levelsChanged, this, &BarLink::sendLevels);
    connect(backend, &Backend::seeked, this, &BarLink::sendProgress);
    connect(backend, &Backend::durationChanged, this, &BarLink::sendProgress);
    m_progressTimer.setInterval(500);
    connect(&m_progressTimer, &QTimer::timeout, this, &BarLink::sendProgress);
}

QString BarLink::encodeLevels(const QVariantList &levels, int bars) {
    // Pool neighbouring bands down to the number of bars the plugin draws,
    // keeping the loudest of each so peaks survive.
    QStringList values;
    const int count = int(levels.size());
    if (count == 0 || bars <= 0)
        return "L";
    for (int bar = 0; bar < bars; ++bar) {
        const int from = bar * count / bars;
        const int to = std::max(from + 1, (bar + 1) * count / bars);
        double peak = 0;
        for (int i = from; i < to && i < count; ++i)
            peak = std::max(peak, levels[i].toDouble());
        values << QString::number(std::clamp(int(peak * 99 + 0.5), 0, 99));
    }
    return "L " + values.join(' ');
}

QString BarLink::encodeTrack(const QString &title, const QString &artist) {
    auto clean = [](QString text) {
        text.replace(QRegularExpression("[\\t\\r\\n]+"), " ");
        return text.simplified().left(200);
    };
    return "N " + clean(title) + "\t" + clean(artist);
}

QByteArray BarLink::stateLine() const {
    return "S " + m_backend->playbackState().toUtf8();
}

QByteArray BarLink::progressLine() const {
    return "P " + QByteArray::number(m_backend->position()) + " " + QByteArray::number(std::max<qint64>(0, m_backend->duration()));
}

void BarLink::sendProgress() {
    if (!m_clients.isEmpty())
        broadcast(progressLine());
}

QByteArray BarLink::trackLine() const {
    return encodeTrack(m_backend->title(), m_backend->artist()).toUtf8();
}

void BarLink::accept() {
    while (QLocalSocket *client = m_server.nextPendingConnection()) {
        // A plugin is one small local client: a handful of connections is
        // plenty, and a line longer than any command is not a command.
        if (m_clients.size() >= 16) {
            client->abort();
            client->deleteLater();
            continue;
        }
        client->setReadBufferSize(4096);
        m_clients << client;
        connect(client, &QLocalSocket::disconnected, this, [this, client] {
            m_clients.removeOne(client);
            client->deleteLater();
            if (m_clients.isEmpty())
                m_progressTimer.stop();
        });
        connect(client, &QLocalSocket::readyRead, this, [this, client] { readCommands(client); });
        // A new plugin instance learns the current state straight away.
        client->write(stateLine() + "\n" + trackLine() + "\n" + progressLine() + "\n");
        m_progressTimer.start();
    }
}

void BarLink::readCommands(QLocalSocket *client) {
    while (client->canReadLine()) {
        const QString line = QString::fromUtf8(client->readLine(256)).trimmed();
        if (line == "playpause")
            m_backend->toggle();
        else if (line == "next")
            m_backend->next();
        else if (line == "previous")
            m_backend->previous();
        else if (line.startsWith("seek "))
            m_backend->seek(std::clamp<qint64>(line.mid(5).toLongLong(), 0, std::max<qint64>(0, m_backend->duration())));
        else if (line.startsWith("volume "))
            m_backend->changeVolume(std::clamp(line.mid(7).toInt(), -100, 100) / 100.0);
    }
}

void BarLink::broadcast(const QByteArray &line) {
    for (QLocalSocket *client : std::as_const(m_clients)) {
        // A plugin that stopped reading must not make Hum buffer without end.
        if (client->bytesToWrite() > 64 * 1024) {
            client->abort();
            continue;
        }
        client->write(line + "\n");
    }
}

void BarLink::sendState() {
    if (m_clients.isEmpty())
        return;
    const QByteArray state = stateLine(), track = trackLine();
    if (state != m_lastState) {
        m_lastState = state;
        broadcast(state);
    }
    if (track != m_lastTrack) {
        m_lastTrack = track;
        broadcast(track);
    }
    broadcast(progressLine());
}

void BarLink::sendLevels() {
    // A bar draws a handful of pixels, so about thirty updates a second is
    // plenty, however fast the window's own visualizer runs.
    if (m_clients.isEmpty() || (m_lastLevels.isValid() && m_lastLevels.elapsed() < 30))
        return;
    m_lastLevels.restart();
    broadcast(encodeLevels(m_backend->levels(), m_bars).toUtf8());
}
