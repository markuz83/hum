#pragma once

#include <QAudioBufferOutput>
#include <QAudioOutput>
#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QMediaPlayer>
#include <QObject>
#include <QQueue>
#include <QStringList>
#include <QElapsedTimer>
#include <QTimer>
#include <QVariantList>

#include "analyzer.h"
#include "filepicker.h"

// The music library, the player, and the numbers behind the visualizer.
class Backend : public QObject {
    Q_OBJECT
    // The three browser columns. Each list holds the raw values (an empty
    // string means the tag is missing); row 0 of the QML list is "All", so
    // artistRow and albumRow count from 1 for a real choice and 0 for all.
    Q_PROPERTY(QStringList artists READ artists NOTIFY browserChanged)
    Q_PROPERTY(QStringList albums READ albums NOTIFY browserChanged)
    Q_PROPERTY(QStringList titles READ titles NOTIFY browserChanged)
    Q_PROPERTY(int artistRow READ artistRow NOTIFY browserChanged)
    Q_PROPERTY(int albumRow READ albumRow NOTIFY browserChanged)
    Q_PROPERTY(int currentRow READ currentRow NOTIFY currentChanged)
    Q_PROPERTY(QString title READ title NOTIFY currentChanged)
    Q_PROPERTY(QString artist READ artist NOTIFY currentChanged)
    Q_PROPERTY(QString album READ album NOTIFY currentChanged)
    // The play queue: tracks that play next, before the library carries on.
    // Each entry is {title, artist, album}. queueRatio is the share of the
    // window width the queue pane takes, which the user can change.
    Q_PROPERTY(QVariantList queue READ queue NOTIFY queueChanged)
    Q_PROPERTY(qreal queueRatio READ queueRatio WRITE setQueueRatio NOTIFY layoutChanged)
    Q_PROPERTY(bool hasCover READ hasCover NOTIFY coverChanged)
    Q_PROPERTY(int coverId READ coverId NOTIFY coverChanged)
    Q_PROPERTY(bool reading READ reading NOTIFY readingChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(QString playbackState READ playbackState NOTIFY playingChanged)
    Q_PROPERTY(qint64 position READ position NOTIFY positionChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(qreal volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY volumeChanged)
    Q_PROPERTY(bool shuffle READ shuffle WRITE setShuffle NOTIFY modeChanged)
    Q_PROPERTY(int repeat READ repeat WRITE setRepeat NOTIFY modeChanged)
    Q_PROPERTY(int visualizerStyle READ visualizerStyle WRITE setVisualizerStyle NOTIFY modeChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY browserChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    Q_PROPERTY(QString libraryPath READ libraryPath NOTIFY libraryChanged)
    Q_PROPERTY(int libraryCount READ libraryCount NOTIFY browserChanged)
    Q_PROPERTY(QVariantList levels READ levels NOTIFY levelsChanged)

public:
    enum Repeat { RepeatOff, RepeatAll, RepeatOne };
    Q_ENUM(Repeat)

    explicit Backend(QObject *parent = nullptr);

    QStringList artists() const { return m_artistValues; }
    QStringList albums() const { return m_albumValues; }
    QStringList titles() const;
    int artistRow() const { return m_allArtists ? 0 : int(m_artistValues.indexOf(m_artistSel)) + 1; }
    int albumRow() const { return m_allAlbums ? 0 : int(m_albumValues.indexOf(m_albumSel)) + 1; }
    int currentRow() const;
    QString title() const { return m_title; }
    QString artist() const { return m_artist; }
    QString album() const { return m_album; }
    bool hasCover() const { return !m_cover.isNull(); }
    int coverId() const { return m_coverId; }
    QImage cover() const { return m_cover; }
    bool reading() const { return m_reading; }
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
    QString currentPath() const { return m_current >= 0 ? m_tracks[m_current].path : QString(); }
    // "Playing", "Paused" or "Stopped", as MPRIS names them.
    QString playbackState() const;
    int visualizerStyle() const { return m_visualizerStyle; }
    bool canGoNext() const { return !m_queue.isEmpty() || !m_visible.isEmpty(); }
    QVariantList queue() const;
    qreal queueRatio() const { return m_queueRatio; }
    QStringList queuePaths() const { return m_queue; }
    void setQueueRatio(qreal ratio);
    int libraryCount() const { return m_tracks.size(); }
    QVariantList levels() const { return m_levelList; }

    // What the tags say about a file. Empty fields mean the tag is missing.
    struct Tags {
        QString artist, album, title;
        int track = 0, disc = 0;
    };
    struct TagResult {
        QHash<QString, Tags> tags;
        bool ffprobeMissing = false;
    };
    // ffprobe's JSON for one file, and the best guess from a Artist/Album/Title
    // folder layout when the tags say nothing.
    static Tags parseProbe(const QByteArray &json);
    static Tags fallbackTags(const QString &relativeName);
    // Reads every file's tags with ffprobe in parallel, reusing results from
    // the cache file for files that haven't changed.
    static TagResult readTags(const QStringList &paths, const QString &cacheFile);
    void setCacheFile(const QString &path) { m_cacheFile = path; }
    // Where volume, modes, and the last track are remembered. Without a state
    // file nothing is saved, which keeps tests away from the user's settings.
    // Setting it also loads whatever was saved.
    void setStateFile(const QString &path);
    void saveState();

    // Audio files under a folder, sorted so "2 - x" comes before "10 - x".
    static QStringList scan(const QString &folder);
    static bool isAudioFile(const QString &path);

    // A folder becomes the library; a file plays inside its own folder.
    // Without a path the library is the user's Music folder.
    Q_INVOKABLE void open(const QString &path = {});
    // Opens the desktop's folder chooser and makes the choice the library.
    Q_INVOKABLE void chooseLibrary();
    void setFilePicker(FilePicker *picker);
    // Queue editing. Rows count from 0 in the title column or the queue.
    Q_INVOKABLE void enqueue(int row, bool playNext = false);
    Q_INVOKABLE void enqueueVisible(bool playNext = false);
    // column 0 is artists and 1 is albums; row 0 of either means "all".
    Q_INVOKABLE void enqueueGroup(int column, int row, bool playNext = false);
    Q_INVOKABLE void removeFromQueue(int row);
    Q_INVOKABLE void moveInQueue(int from, int to);
    Q_INVOKABLE void clearQueue();
    Q_INVOKABLE void playFromQueue(int row);
    Q_INVOKABLE void undoQueue();
    Q_INVOKABLE void redoQueue();
    Q_INVOKABLE void selectArtist(int row);
    Q_INVOKABLE void selectAlbum(int row);
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
    Q_INVOKABLE void stop();
    Q_INVOKABLE void cycleVisualizer();
    void setShuffle(bool shuffle);
    void setRepeat(int repeat);
    void setVisualizerStyle(int style);
    void setVolume(qreal volume);
    void setFilter(const QString &filter);

signals:
    void browserChanged();
    void coverChanged();
    void queueChanged();
    void layoutChanged();
    void readingChanged();
    void currentChanged();
    void playingChanged();
    void positionChanged();
    // The user or another program jumped to a new position, in milliseconds.
    void seeked(qint64 ms);
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
        QString name;  // path under the library, without the extension
        QString artist, album, title;
        int track = 0, disc = 0;
    };

    void loadState();
    // Queue plumbing: paths survive the library re-sorting, so the queue
    // holds paths and looks tracks up when it needs them.
    int indexOfPath(const QString &path) const;
    void setQueue(const QStringList &queue, bool recordUndo = true);
    void insertIntoQueue(const QStringList &paths, bool playNext);
    bool playPath(const QString &path);
    // The row that plays after the current one, or -1 at the end of the list.
    // When the current track isn't in the visible list (it came from the
    // queue), the library carries on from where that track sits in it.
    int nextRow(bool wrap) const;
    void scheduleSave();
    void finishScan(const QStringList &paths);
    void finishReading(const TagResult &result);
    void sortTracks();
    void refresh();
    void updateCover(const QImage &embedded);
    void setError(const QString &message);
    void loadCurrent();
    void readMetadata();
    void advanceAfterEnd();
    void tick();

    QList<Track> m_tracks;
    QList<int> m_visible;
    QList<int> m_matching;  // tracks passing the filter, whatever the artist and album chosen
    QStringList m_queue;
    QList<QStringList> m_queueUndo, m_queueRedo;
    qreal m_queueRatio = 1.0 / 3.0;  // indexes into m_tracks shown in the title column
    QStringList m_artistValues, m_albumValues;
    bool m_allArtists = true, m_allAlbums = true;
    QString m_artistSel, m_albumSel;
    int m_current = -1;    // index into m_tracks, -1 when nothing is loaded
    QString m_library, m_filter, m_error, m_title, m_artist, m_album, m_pending, m_cacheFile;
    FilePicker *m_picker = nullptr;
    QString m_stateFile, m_resumePath, m_savedLibrary;
    qint64 m_resumePosition = 0;
    qint64 m_resumeAt = -1;  // where to seek once the resumed track has loaded
    bool m_loadingState = false;
    QTimer m_saveTimer, m_positionTimer;
    QImage m_cover;
    int m_coverId = 0;
    bool m_reading = false;
    bool m_scanning = false, m_shuffle = false;
    int m_visualizerStyle = 0;
    int m_repeat = RepeatOff;
    qreal m_volume = 0.8;

    QMediaPlayer m_player;
    QAudioOutput m_audio;
    QAudioBufferOutput m_tap;
    QQueue<QAudioBuffer> m_buffers;
    Analyzer m_analyzer;
    QTimer m_frames;
    QElapsedTimer m_clock;
    qint64 m_clockBase = 0;
    QVariantList m_levelList;
    QFutureWatcher<QStringList> m_watcher;
    QFutureWatcher<TagResult> m_tagWatcher;
};
