#pragma once

#include <array>
#include <memory>

#include <QAudioBufferOutput>
#include <QAudioOutput>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QMediaPlayer>
#include <QObject>
#include <QQueue>
#include <QStringList>
#include <QtMath>
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
    // Length in seconds of each title in the list, 0 when unknown.
    Q_PROPERTY(QVariantList titleSeconds READ titleSeconds NOTIFY browserChanged)
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
    // Volume leveling from ReplayGain tags: 0 off, 1 per track, 2 per album.
    Q_PROPERTY(int replayGain READ replayGain WRITE setReplayGain NOTIFY modeChanged)
    // Seconds of overlap between tracks; 0 plays them back to back, gaplessly.
    Q_PROPERTY(int crossfade READ crossfade WRITE setCrossfade NOTIFY modeChanged)
    // Sleep timer: 0 off, minutes left to run, or -1 to stop after this track.
    Q_PROPERTY(int sleepSetting READ sleepSetting NOTIFY sleepChanged)
    Q_PROPERTY(int sleepRemaining READ sleepRemaining NOTIFY sleepChanged)
    Q_PROPERTY(bool shuffle READ shuffle WRITE setShuffle NOTIFY modeChanged)
    Q_PROPERTY(int repeat READ repeat WRITE setRepeat NOTIFY modeChanged)
    // Visualizer updates per second; 0 follows the display's refresh rate.
    Q_PROPERTY(int visualizerRate READ visualizerRate WRITE setVisualizerRate NOTIFY modeChanged)
    // The display's refresh rate in Hz, reported by the window.
    Q_PROPERTY(double refreshRate READ refreshRate WRITE setRefreshRate NOTIFY modeChanged)
    // True while the bars are moving, so a frame-synced driver knows to run.
    Q_PROPERTY(bool animating READ animating NOTIFY animatingChanged)
    // How the title list is ordered: 0 album order, 1 title, 2 year, 3 recently added, 4 length.
    Q_PROPERTY(int sortMode READ sortMode WRITE setSortMode NOTIFY modeChanged)
    // Lyrics of the playing track: lines of text, and when each one starts if they are synced.
    Q_PROPERTY(bool showLyrics READ showLyrics WRITE setShowLyrics NOTIFY modeChanged)
    Q_PROPERTY(QStringList lyricLines READ lyricLines NOTIFY lyricsChanged)
    Q_PROPERTY(QVariantList lyricTimes READ lyricTimes NOTIFY lyricsChanged)
    Q_PROPERTY(bool lyricsSynced READ lyricsSynced NOTIFY lyricsChanged)
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
    QVariantList titleSeconds() const;
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
    bool playing() const;
    qint64 position() const;
    qint64 duration() const;
    qreal volume() const { return m_volume; }
    bool muted() const { return m_muted; }
    int replayGain() const { return m_replayGain; }
    int crossfade() const { return m_crossfade; }
    int sleepSetting() const { return m_sleepSetting; }
    int sleepRemaining() const;
    bool shuffle() const { return m_shuffle; }
    int repeat() const { return m_repeat; }
    QString filter() const { return m_filter; }
    QString error() const { return m_error; }
    bool scanning() const { return m_scanning; }
    QString libraryPath() const { return m_library; }
    QString currentPath() const { return m_current >= 0 ? m_tracks[m_current].path : QString(); }
    // "Playing", "Paused" or "Stopped", as MPRIS names them.
    QString playbackState() const;
    int sortMode() const { return m_sortMode; }
    bool showLyrics() const { return m_showLyrics; }
    QStringList lyricLines() const { return m_lyrics.lines; }
    QVariantList lyricTimes() const;
    bool lyricsSynced() const { return !m_lyrics.times.isEmpty(); }
    // The line being sung at this moment, or -1 before the first one.
    Q_INVOKABLE int lyricIndexAt(qint64 ms) const;
    int visualizerRate() const { return m_visualizerRate; }
    double refreshRate() const { return m_refreshRate; }
    bool animating() const { return m_animating; }
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
        int seconds = 0;  // length, rounded; 0 when unknown
        // ReplayGain in dB, NaN when the file has none.
        double trackGain = qQNaN(), albumGain = qQNaN();
        int year = 0;
        qint64 modified = 0;  // file modified time, ms since the epoch
    };
    // Lyrics from a .lrc file beside the track, or from its tags. A line is
    // synced when the text carries [mm:ss.xx] stamps, and times then holds
    // the start of every line.
    struct Lyrics {
        QStringList lines;
        QList<qint64> times;
    };
    static Lyrics parseLyrics(const QString &text);
    static Lyrics readLyrics(const QString &path, const QString &ffprobe);
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

    // The cover image sitting beside music files: a conventional name first
    // (cover, folder, front, album, AlbumArt…), otherwise the only image there.
    // Empty when there isn't one clear choice.
    static QString findFolderCover(const QString &folder);
    // Reads an image no larger than `limit` pixels a side, decoding big ones at
    // the smaller size instead of in full.
    static QImage loadScaledImage(const QString &path, int limit = 800);

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
    void setSortMode(int mode);
    Q_INVOKABLE void cycleSort();
    void setShowLyrics(bool show);
    void setVisualizerRate(int hz);
    void setRefreshRate(double hz);
    Q_INVOKABLE void cycleVisualizerRate();
    // The gain ReplayGain gives a file under the current mode, as a factor.
    double leveling(const QString &path) const;
    void setReplayGain(int mode);
    void setCrossfade(int seconds);
    Q_INVOKABLE void cycleReplayGain();
    Q_INVOKABLE void cycleCrossfade();
    // Off, 15, 30, 45, 60 and 90 minutes, then the end of the current track.
    Q_INVOKABLE void cycleSleep();
    void setSleep(int setting);
    // Called once per display frame while the rate follows the display.
    Q_INVOKABLE void frame();
    void setVolume(qreal volume);
    void setFilter(const QString &filter);

signals:
    void browserChanged();
    void coverChanged();
    void queueChanged();
    void animatingChanged();
    void sleepChanged();
    void lyricsChanged();
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
        int seconds = 0;
        double trackGain = qQNaN(), albumGain = qQNaN();
        int year = 0;
        qint64 added = 0;  // file modified time, for "recently added"
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
    void readMetadata();
    void advanceAfterEnd();
    void tick();
    void startFrames();
    void stopFrames();
    void configureFrames();

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
    // Albums share one cover.jpg, so the last folder's image is kept for the next track.
    QString m_coverFolder;
    QImage m_coverFolderImage;
    int m_coverId = 0;
    bool m_reading = false;
    bool m_scanning = false, m_shuffle = false;
    int m_visualizerStyle = 0;
    int m_visualizerRate = 0;
    double m_refreshRate = 60;
    bool m_animating = false;
    QElapsedTimer m_lastTick;
    int m_repeat = RepeatOff;
    qreal m_volume = 0.8;

    // Two players, so the next track can be loaded while this one plays and
    // take over without a gap, or fade in under the one fading out.
    struct Voice {
        explicit Voice(QObject *parent) : player(parent), out(parent), tap(parent) {}
        QMediaPlayer player;
        QAudioOutput out;
        QAudioBufferOutput tap;
        QString path;       // the file this voice has loaded, or is loading
        bool ready = false;  // loaded and able to start at once
        double fade = 1.0;   // 0 to 1, for a crossfade
    };
    enum class Handoff { Manual, Natural };
    void loadCurrent(Handoff handoff = Handoff::Manual);
    void loadLyrics();
    void watchLibrary(const QStringList &paths);
    void rescan();
    Voice &active() const { return *m_voices[m_active]; }
    Voice &idle() const { return *m_voices[1 - m_active]; }
    QMediaPlayer &player() const { return active().player; }
    void wire(int index);
    void applyGains();
    QString computeNext(bool wrap) const;
    void schedulePlan();
    void planNext();
    void beginTransition();
    void finishFade();
    void tickTransition();
    void pushHistory(const QString &path);
    void endSleep();
    bool advance(bool wrap);

    std::array<std::unique_ptr<Voice>, 2> m_voices;
    int m_active = 0;
    int m_fadeVoice = -1;  // the voice fading out, or -1
    qint64 m_fadeMs = 0;
    QElapsedTimer m_fadeClock;
    // The player's own last position report, for timing the handoff. The
    // smoothed clock the visualizer uses may run a little ahead of the sound.
    qint64 m_reportedPosition = 0;
    QElapsedTimer m_reportClock;
    QString m_planned;  // the track queued up behind this one
    QTimer m_planTimer, m_transitionTimer, m_sleepTimer;
    QStringList m_history;
    QString m_lastPath;
    bool m_goingBack = false;
    bool m_muted = false;
    int m_replayGain = 0;
    int m_crossfade = 0;
    int m_sleepSetting = 0;
    bool m_sleepAtEnd = false;
    QElapsedTimer m_sleepClock;
    qint64 m_sleepMs = 0;
    double m_sleepFactor = 1.0;
    QQueue<QAudioBuffer> m_buffers;
    Analyzer m_analyzer;
    QTimer m_frames;
    QElapsedTimer m_clock;
    qint64 m_clockBase = 0;
    QVariantList m_levelList;
    int m_sortMode = 0;
    bool m_showLyrics = false;
    Lyrics m_lyrics;
    QFutureWatcher<Lyrics> m_lyricsWatcher;
    QString m_lyricsFor;
    // The library follows its folder: changes there trigger a quiet rescan.
    QFileSystemWatcher m_folders;
    QTimer m_rescanTimer, m_pollTimer;
    bool m_rescanning = false;
    QFutureWatcher<QStringList> m_watcher;
    QFutureWatcher<TagResult> m_tagWatcher;
};
