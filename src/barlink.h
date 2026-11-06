#pragma once

#include <QList>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QObject>
#include <QVariantList>

class Backend;
class QLocalSocket;

// A small local socket for the Omarchy bar plugin (omarchy-plugin/): it streams
// what is playing and the live spectrum out, and takes a few commands in.
// Nothing is computed or sent while no plugin is connected.
//
// Lines from Hum, UTF-8, one per message:
//   S Playing|Paused|Stopped        playback state, sent on connect and on change
//   N <title> TAB <artist>          the current track, sent on connect and on change
//   L <n> <n> <n> ...               spectrum bars as whole numbers 0 to 99, ~30 times a second
// Lines to Hum:
//   playpause | next | previous | volume <+n|-n>   (volume moves by n percent)
class BarLink : public QObject {
    Q_OBJECT

public:
    // The socket lives in the runtime folder unless a test gives its own path.
    explicit BarLink(Backend *backend, const QString &path = {}, int bars = 24, QObject *parent = nullptr);
    bool listening() const { return m_server.isListening(); }
    QString path() const { return m_server.fullServerName(); }

    static QString encodeLevels(const QVariantList &levels, int bars);
    // Tabs and newlines would break the framing, and a long tag would bloat it.
    static QString encodeTrack(const QString &title, const QString &artist);

private:
    void accept();
    void readCommands(QLocalSocket *client);
    void broadcast(const QByteArray &line);
    QByteArray stateLine() const;
    QByteArray trackLine() const;
    void sendState();
    void sendLevels();

    Backend *m_backend;
    int m_bars;
    QList<QLocalSocket *> m_clients;
    QByteArray m_lastState, m_lastTrack;
    // Declared last so it is destroyed first: its connected sockets announce
    // that they disconnected while it goes, and that must find m_clients alive.
    QElapsedTimer m_lastLevels;
    QLocalServer m_server;
};
