#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#include <QObject>
#include <QTimer>
#include <QVariantMap>

class Backend;

// The MPRIS interfaces that media keys, `playerctl`, and desktop media widgets
// speak, so Hum can be controlled and shown without focusing its window.
// https://specifications.freedesktop.org/mpris-spec/latest/

// org.mpris.MediaPlayer2: the application itself.
class MprisRoot : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ canQuit)
    Q_PROPERTY(bool CanRaise READ canRaise)
    Q_PROPERTY(bool HasTrackList READ hasTrackList)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ supportedUriSchemes)
    Q_PROPERTY(QStringList SupportedMimeTypes READ supportedMimeTypes)

public:
    explicit MprisRoot(QObject *parent) : QDBusAbstractAdaptor(parent) {}
    bool canQuit() const { return true; }
    bool canRaise() const { return false; }
    bool hasTrackList() const { return false; }
    QString identity() const { return "Hum"; }
    QString desktopEntry() const { return "hum"; }
    QStringList supportedUriSchemes() const { return {"file"}; }
    QStringList supportedMimeTypes() const;

public slots:
    void Raise() {}
    void Quit();
};

// org.mpris.MediaPlayer2.Player: what is playing, and the controls.
class MprisPlayer : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(QString LoopStatus READ loopStatus WRITE setLoopStatus)
    Q_PROPERTY(double Rate READ rate WRITE setRate)
    Q_PROPERTY(bool Shuffle READ shuffle WRITE setShuffle)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double MinimumRate READ rate)
    Q_PROPERTY(double MaximumRate READ rate)
    Q_PROPERTY(bool CanGoNext READ canGoNext)
    Q_PROPERTY(bool CanGoPrevious READ canGoNext)
    Q_PROPERTY(bool CanPlay READ canPlay)
    Q_PROPERTY(bool CanPause READ canPlay)
    Q_PROPERTY(bool CanSeek READ canSeek)
    Q_PROPERTY(bool CanControl READ canControl)

public:
    MprisPlayer(Backend *backend, QObject *parent);

    QString playbackStatus() const;
    QString loopStatus() const;
    void setLoopStatus(const QString &status);
    double rate() const { return 1.0; }
    void setRate(double) {}
    bool shuffle() const;
    void setShuffle(bool shuffle);
    QVariantMap metadata() const;
    double volume() const;
    void setVolume(double volume);
    qlonglong position() const;
    bool canGoNext() const;
    bool canPlay() const;
    bool canSeek() const;
    bool canControl() const { return true; }

    // The cover as a file other programs can load, or empty without one.
    void setArtUrl(const QString &url) { m_artUrl = url; }

public slots:
    void Next();
    void Previous();
    void Pause();
    void PlayPause();
    void Stop();
    void Play();
    void Seek(qlonglong offset);
    void SetPosition(const QDBusObjectPath &trackId, qlonglong position);
    void OpenUri(const QString &uri);

signals:
    void Seeked(qlonglong position);

private:
    QDBusObjectPath trackId() const;

    Backend *m_backend;
    QString m_artUrl;
};

// Owns the bus name and object, and tells listeners what changed.
class Mpris : public QObject {
    Q_OBJECT

public:
    // The service is org.mpris.MediaPlayer2.<name>; tests pass their own name.
    explicit Mpris(Backend *backend, const QString &name = "hum", const QString &artDirectory = {},
                   QObject *parent = nullptr);
    bool registered() const { return m_registered; }
    QString serviceName() const { return m_service; }

private:
    void schedule();
    void publish();
    QVariantMap snapshot() const;
    QString metadataSignature() const;
    void writeArt();

    Backend *m_backend;
    MprisPlayer *m_player;
    QString m_service, m_artDirectory;
    bool m_registered = false;
    QVariantMap m_last;
    QString m_lastMetadata;
    QString m_artFile;
    QTimer m_debounce;
};
