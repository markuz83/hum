#pragma once

#include <QAudioBufferOutput>
#include <QAudioOutput>
#include <QFutureWatcher>
#include <QMediaPlayer>
#include <QObject>
#include <QQueue>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include "analyzer.h"

// The music library, the player, and the numbers behind the visualizer.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList tracks READ tracks NOTIFY tracksChanged)
    Q_PROPERTY(int currentRow READ currentRow NOTIFY currentChanged)
    Q_PROPERTY(QString title READ title NOTIFY currentChanged)
    Q_PROPERTY(QString artist READ artist NOTIFY currentChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(qint64 position READ position NOTIFY positionChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(qreal volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY volumeChanged)
    Q_PROPERTY(bool shuffle READ shuffle NOTIFY modeChanged)
    Q_PROPERTY(int repeat READ repeat NOTIFY modeChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY tracksChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    Q_PROPERTY(QString libraryPath READ libraryPath NOTIFY libraryChanged)
    Q_PROPERTY(int libraryCount READ libraryCount NOTIFY tracksChanged)
    Q_PROPERTY(QVariantList levels READ levels NOTIFY levelsChanged)

public:
    enum Repeat { RepeatOff, RepeatAll, RepeatOne };
    Q_ENUM(Repeat)

    explicit Backend(QObject *parent = nullptr);

    QStringList tracks() const;
    int currentRow() const;
    QString title() const { return m_title; }
    QString artist() const { return m_artist; }
    bool playing() const { return m_player.playbackState() == QMediaPlayer::PlayingState; }
    qint64 position() const { return m_player.position(); }
    qint64 duration() const { return m_player.duration(); }
    qreal volume() const { return m_volume; }
    bool muted() const { return m_audio.isMuted(); }
    bool shuffle() const { return m_shuffle; }
    int repeat() const { return m_repeat; }
    QString filter() const { return m_filter; }
    QString error() const { return m_error; }
    bool scanning() const { return m_scanning; }
    QString libraryPath() const { return m_library; }
    int libraryCount() const { return m_tracks.size(); }
    QVariantList levels() const { return m_levelList; }

    // Audio files under a folder, sorted so "2 - x" comes before "10 - x".
    static QStringList scan(const QString &folder);
    static bool isAudioFile(const QString &path);

    // A folder becomes the library; a file plays inside its own folder.
    // Without a path the library is the user's Music folder.
    Q_INVOKABLE void open(const QString &path = {});
    Q_INVOKABLE void play(int row);
    Q_INVOKABLE void toggle();
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void seek(qint64 ms);
    Q_INVOKABLE void seekBy(qint64 ms);
    Q_INVOKABLE void changeVolume(qreal delta);
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void toggleShuffle();
    Q_INVOKABLE void cycleRepeat();
    void setVolume(qreal volume);
    void setFilter(const QString &filter);

signals:
    void tracksChanged();
    void currentChanged();
    void playingChanged();
    void positionChanged();
    void durationChanged();
    void volumeChanged();
    void modeChanged();
    void errorChanged();
    void scanningChanged();
    void libraryChanged();
    void levelsChanged();

private:
    struct Track {
        QString path;
        QString name;
    };

    void finishScan(const QStringList &paths);
    void applyFilter();
    void setError(const QString &message);
    void loadCurrent();
    void readMetadata();
    void advanceAfterEnd();
    void tick();

    QList<Track> m_tracks;
    QList<int> m_visible;  // indexes into m_tracks that match the filter
    int m_current = -1;    // index into m_tracks, -1 when nothing is loaded
    QString m_library, m_filter, m_error, m_title, m_artist, m_pending;
    bool m_scanning = false, m_shuffle = false;
    int m_repeat = RepeatOff;
    qreal m_volume = 0.8;

    QMediaPlayer m_player;
    QAudioOutput m_audio;
    QAudioBufferOutput m_tap;
    QQueue<QAudioBuffer> m_buffers;
    Analyzer m_analyzer;
    QTimer m_frames;
    QVariantList m_levelList;
    QFutureWatcher<QStringList> m_watcher;
};
