#include "backend.h"

#include <QCollator>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QFileInfo>
#include <QImageReader>
#include <QMediaMetaData>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QUrl>
#include <QtConcurrent>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace {
// How long before a track ends the next one starts when there is no crossfade.
// Starting an already-loaded player takes a variable 10 to 80 ms to reach the
// speakers, in steps of the audio buffer, so no single lead is exact. Joins
// recorded from a real output: 60 ms left a gap of about 10 ms in one of six,
// 75 ms never left a gap in six and overlapped the tracks by 0 to about 75 ms.
// A gap can click; a short overlap at the end of a track is not noticeable.
constexpr qint64 GaplessLeadMs = 75;
constexpr int DefaultVisualizerRate = 60;
// Bumped whenever the cache gains a field, so old entries are read again.
constexpr int CacheVersion = 3;
// The most text read from a lyrics file, or from one ffprobe answer.
constexpr qint64 MaxTextBytes = 8 * 1024 * 1024;
const QStringList Extensions{"mp3", "ogg", "oga", "opus", "flac", "wav", "m4a", "aac", "wma", "mka", "weba"};
}

Backend::Backend(QObject *parent) : QObject(parent) {
    for (int i = 0; i < 2; ++i) {
        m_voices[i] = std::make_unique<Voice>(this);
        m_voices[i]->player.setAudioOutput(&m_voices[i]->out);
        // The visualizer reads the same decoded audio the speakers get.
        m_voices[i]->player.setAudioBufferOutput(&m_voices[i]->tap);
        wire(i);
    }
    applyGains();

    // The next track is chosen a moment after anything that could change it,
    // so skipping quickly doesn't load a file for every press.
    m_planTimer.setSingleShot(true);
    m_planTimer.setInterval(250);
    connect(&m_planTimer, &QTimer::timeout, this, &Backend::planNext);
    connect(this, &Backend::queueChanged, this, &Backend::schedulePlan);
    connect(this, &Backend::modeChanged, this, &Backend::schedulePlan);
    m_transitionTimer.setInterval(10);
    m_transitionTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_transitionTimer, &QTimer::timeout, this, &Backend::tickTransition);
    m_sleepTimer.setInterval(1000);
    connect(&m_sleepTimer, &QTimer::timeout, this, [this] { emit sleepChanged(); });

    // Copying an album in raises many changes; wait until they stop.
    m_rescanTimer.setSingleShot(true);
    m_rescanTimer.setInterval(1500);
    connect(&m_rescanTimer, &QTimer::timeout, this, &Backend::rescan);
    connect(&m_folders, &QFileSystemWatcher::directoryChanged, this, [this] { m_rescanTimer.start(); });
    m_pollTimer.setInterval(60000);
    connect(&m_pollTimer, &QTimer::timeout, this, &Backend::rescan);
    connect(&m_lyricsWatcher, &QFutureWatcher<Lyrics>::finished, this, [this] {
        // Only the track that asked for them; a quick skip leaves stale results.
        if (m_lyricsFor != m_lastPath)
            return;
        m_lyrics = m_lyricsWatcher.result();
        emit lyricsChanged();
    });
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(800);
    connect(&m_saveTimer, &QTimer::timeout, this, &Backend::saveState);
    m_positionTimer.setInterval(10000);
    connect(&m_positionTimer, &QTimer::timeout, this, &Backend::saveState);
    for (auto signal : {&Backend::volumeChanged, &Backend::modeChanged, &Backend::currentChanged})
        connect(this, signal, this, &Backend::scheduleSave);
    connect(qApp, &QCoreApplication::aboutToQuit, this, &Backend::saveState);

    m_frames.setTimerType(Qt::PreciseTimer);
    configureFrames();
    connect(&m_frames, &QTimer::timeout, this, &Backend::tick);
    connect(&m_watcher, &QFutureWatcher<QStringList>::finished, this, [this] { finishScan(m_watcher.result()); });
    connect(&m_tagWatcher, &QFutureWatcher<TagResult>::finished, this, [this] { finishReading(m_tagWatcher.result()); });
    m_cacheFile = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/tags.json";
}

// Everything a voice reports matters only while it is the active one; the
// other is either loading the next track or fading out the last.
void Backend::wire(int index) {
    Voice &voice = *m_voices[index];
    connect(&voice.tap, &QAudioBufferOutput::audioBufferReceived, this, [this, index](const QAudioBuffer &buffer) {
        if (index != m_active)
            return;
        m_buffers.enqueue(buffer);
        // Decoding runs ahead of playback; bound the backlog so a long pause
        // can't grow it without limit.
        while (m_buffers.size() > 600)
            m_buffers.dequeue();
    });
    connect(&voice.player, &QMediaPlayer::positionChanged, this, [this, index](qint64 position) {
        if (index != m_active)
            return;
        m_reportedPosition = position;
        m_reportClock.restart();
        // Never step the clock backwards for a small report lag, only for a real seek.
        const qint64 estimate = m_clockBase + m_clock.elapsed();
        if (position > estimate || estimate - position > 250) {
            m_clockBase = position;
            m_clock.restart();
        }
        emit positionChanged();
    });
    connect(&voice.player, &QMediaPlayer::durationChanged, this, [this, index] {
        if (index == m_active)
            emit durationChanged();
    });
    connect(&voice.player, &QMediaPlayer::metaDataChanged, this, [this, index] {
        if (index == m_active)
            readMetadata();
    });
    connect(&voice.player, &QMediaPlayer::playbackStateChanged, this, [this, index] {
        if (index != m_active)
            return;
        emit playingChanged();
        // Remember the position while playing, and exactly where it stopped.
        if (playing()) {
            m_positionTimer.start();
            m_transitionTimer.start();
        } else {
            m_positionTimer.stop();
        }
        scheduleSave();
        if (playing()) {
            m_clockBase = player().position();
            m_clock.restart();
            m_reportedPosition = m_clockBase;
            m_reportClock.restart();
            startFrames();
        }
    });
    connect(&voice.player, &QMediaPlayer::mediaStatusChanged, this, [this, index](QMediaPlayer::MediaStatus status) {
        Voice &v = *m_voices[index];
        if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia)
            v.ready = true;
        else if (status == QMediaPlayer::NoMedia || status == QMediaPlayer::InvalidMedia)
            v.ready = false;
        if (index != m_active)
            return;
        if (status == QMediaPlayer::EndOfMedia)
            advanceAfterEnd();
        // A resumed track waits, paused, at the spot where Hum was closed.
        if (status == QMediaPlayer::LoadedMedia && m_resumeAt >= 0) {
            m_clockBase = m_resumeAt;
            m_clock.restart();
            player().setPosition(m_resumeAt);
            m_resumeAt = -1;
            emit positionChanged();
        }
    });
    connect(&voice.player, &QMediaPlayer::errorOccurred, this, [this, index](QMediaPlayer::Error, const QString &message) {
        // A bad file queued up behind this one is reported when its turn comes.
        if (index == m_active)
            setError(tr("Can't play %1: %2").arg(m_title, message));
    });
}

bool Backend::playing() const {
    return player().playbackState() == QMediaPlayer::PlayingState;
}

qint64 Backend::position() const {
    return player().position();
}

qint64 Backend::duration() const {
    return player().duration();
}

int Backend::indexOfPath(const QString &path) const {
    for (int i = 0; i < m_tracks.size(); ++i)
        if (m_tracks[i].path == path)
            return i;
    return -1;
}

QVariantList Backend::queue() const {
    QVariantList items;
    for (const QString &path : m_queue) {
        const int index = indexOfPath(path);
        if (index < 0)
            continue;
        const Track &t = m_tracks[index];
        items << QVariantMap{{"title", t.title}, {"artist", t.artist}, {"album", t.album}};
    }
    return items;
}

void Backend::setQueueRatio(qreal ratio) {
    ratio = std::clamp(ratio, 0.15, 0.6);
    if (qFuzzyCompare(ratio, m_queueRatio))
        return;
    m_queueRatio = ratio;
    emit layoutChanged();
    scheduleSave();
}

// Every edit keeps the queue it replaced, so Ctrl+Z can bring it back.
void Backend::setQueue(const QStringList &queue, bool recordUndo) {
    if (queue == m_queue)
        return;
    if (recordUndo) {
        m_queueUndo.append(m_queue);
        if (m_queueUndo.size() > 50)
            m_queueUndo.removeFirst();
        m_queueRedo.clear();
    }
    m_queue = queue;
    emit queueChanged();
    scheduleSave();
}

void Backend::insertIntoQueue(const QStringList &paths, bool playNext) {
    if (paths.isEmpty())
        return;
    QStringList queue = m_queue;
    if (playNext) {
        for (int i = 0; i < paths.size(); ++i)
            queue.insert(i, paths[i]);
    } else {
        queue += paths;
    }
    setQueue(queue);
}

void Backend::enqueue(int row, bool playNext) {
    if (row >= 0 && row < m_visible.size())
        insertIntoQueue({m_tracks[m_visible[row]].path}, playNext);
}

void Backend::enqueueVisible(bool playNext) {
    QStringList paths;
    for (int index : std::as_const(m_visible))
        paths << m_tracks[index].path;
    insertIntoQueue(paths, playNext);
}

void Backend::enqueueGroup(int column, int row, bool playNext) {
    QStringList paths;
    for (int index : std::as_const(m_matching)) {
        const Track &t = m_tracks[index];
        bool match = true;
        if (column == 0 && row > 0 && row <= m_artistValues.size())
            match = t.artist == m_artistValues[row - 1];
        else if (column == 1) {
            // An album means that album within the artist chosen, if one is.
            match = m_allArtists || t.artist == m_artistSel;
            if (match && row > 0 && row <= m_albumValues.size())
                match = t.album == m_albumValues[row - 1];
        }
        if (match)
            paths << t.path;
    }
    insertIntoQueue(paths, playNext);
}

void Backend::removeFromQueue(int row) {
    if (row < 0 || row >= m_queue.size())
        return;
    QStringList queue = m_queue;
    queue.removeAt(row);
    setQueue(queue);
}

void Backend::moveInQueue(int from, int to) {
    if (from < 0 || from >= m_queue.size() || to < 0 || to >= m_queue.size() || from == to)
        return;
    QStringList queue = m_queue;
    queue.move(from, to);
    setQueue(queue);
}

void Backend::clearQueue() {
    setQueue({});
}

void Backend::playFromQueue(int row) {
    if (row < 0 || row >= m_queue.size())
        return;
    const QString path = m_queue.takeAt(row);
    m_queueUndo.clear();
    m_queueRedo.clear();
    emit queueChanged();
    scheduleSave();
    playPath(path);
}

void Backend::undoQueue() {
    if (m_queueUndo.isEmpty())
        return;
    m_queueRedo.append(m_queue);
    m_queue = m_queueUndo.takeLast();
    emit queueChanged();
    scheduleSave();
}

void Backend::redoQueue() {
    if (m_queueRedo.isEmpty())
        return;
    m_queueUndo.append(m_queue);
    m_queue = m_queueRedo.takeLast();
    emit queueChanged();
    scheduleSave();
}

void Backend::setFilePicker(FilePicker *picker) {
    m_picker = picker;
    connect(picker, &FilePicker::picked, this, [this](const QString &path) {
        // A folder chosen on purpose is remembered, unlike one passed on the
        // command line for a single run.
        m_savedLibrary = path;
        scheduleSave();
        open(path);
    });
    connect(picker, &FilePicker::failed, this, [this](const QString &message) {
        setError(tr("Couldn't open the folder chooser: %1").arg(message));
    });
}

void Backend::chooseLibrary() {
    if (m_picker)
        m_picker->pickFolder(tr("Choose a music folder"));
}

void Backend::setStateFile(const QString &path) {
    m_stateFile = path;
    loadState();
}

void Backend::loadState() {
    QFile file(m_stateFile);
    if (m_stateFile.isEmpty() || !file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject state = QJsonDocument::fromJson(file.readAll()).object();
    // Applying saved values must not trigger a save of half-restored state.
    m_loadingState = true;
    if (state.contains("volume"))
        setVolume(state.value("volume").toDouble(m_volume));
    setShuffle(state.value("shuffle").toBool());
    setRepeat(state.value("repeat").toInt());
    setVisualizerStyle(state.value("visualizer").toInt());
    setVisualizerRate(state.value("visualizerRate").toInt());
    setReplayGain(state.value("replayGain").toInt());
    setSortMode(state.value("sort").toInt());
    setShowLyrics(state.value("lyrics").toBool());
    setCrossfade(state.value("crossfade").toInt());
    m_resumePath = state.value("track").toString();
    m_resumePosition = qint64(state.value("position").toDouble());
    m_savedLibrary = state.value("library").toString();
    if (state.contains("queueRatio"))
        setQueueRatio(state.value("queueRatio").toDouble(m_queueRatio));
    m_queue.clear();
    for (const QJsonValue &path : state.value("queue").toArray())
        m_queue << path.toString();
    m_loadingState = false;
}

void Backend::scheduleSave() {
    if (!m_stateFile.isEmpty() && !m_loadingState)
        m_saveTimer.start();
}

// Saved beside the target and renamed, so a crash never leaves half a file.
void Backend::saveState() {
    if (m_stateFile.isEmpty())
        return;
    QJsonObject state{{"volume", m_volume}, {"shuffle", m_shuffle}, {"repeat", m_repeat},
                      {"visualizer", m_visualizerStyle}, {"visualizerRate", m_visualizerRate}, {"replayGain", m_replayGain},
                      {"crossfade", m_crossfade}, {"sort", m_sortMode}, {"lyrics", m_showLyrics}, {"track", currentPath()},
                      {"position", double(m_resumeAt >= 0 ? m_resumeAt : player().position())},
                      {"library", m_savedLibrary}, {"queueRatio", m_queueRatio},
                      {"queue", QJsonArray::fromStringList(m_queue)}};
    // Before the track the user left has been found again, keep remembering it.
    if (currentPath().isEmpty() && !m_resumePath.isEmpty()) {
        state["track"] = m_resumePath;
        state["position"] = double(m_resumePosition);
    }
    QDir().mkpath(QFileInfo(m_stateFile).absolutePath());
    QSaveFile out(m_stateFile);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(QJsonDocument(state).toJson());
        out.commit();
    }
}

bool Backend::isAudioFile(const QString &path) {
    return Extensions.contains(QFileInfo(path).suffix().toLower());
}

QStringList Backend::scan(const QString &folder) {
    QStringList found;
    QDirIterator it(folder, QDir::Files | QDir::Readable, QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
    while (it.hasNext()) {
        const QString path = it.next();
        if (isAudioFile(path))
            found << path;
    }
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(found.begin(), found.end(), [&](const QString &a, const QString &b) { return collator.compare(a, b) < 0; });
    return found;
}

void Backend::open(const QString &path) {
    QString folder = path;
    m_pending.clear();
    if (path.isEmpty()) {
        folder = QFileInfo(m_savedLibrary).isDir() ? m_savedLibrary : QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    } else if (QFileInfo(path).isFile()) {
        m_pending = QFileInfo(path).absoluteFilePath();
        folder = QFileInfo(path).absolutePath();
    } else if (!QFileInfo(path).isDir()) {
        setError(tr("%1 doesn't exist").arg(path));
        return;
    }
    if (!QFileInfo(folder).isDir()) {
        setError(tr("The music folder %1 doesn't exist").arg(folder));
        return;
    }

    m_library = QFileInfo(folder).absoluteFilePath();
    emit libraryChanged();
    setError({});
    m_scanning = true;
    emit scanningChanged();
    m_watcher.setFuture(QtConcurrent::run([folder] { return scan(folder); }));
}

namespace {
// Empty tags sort last, so "Unknown" doesn't crowd the top of a column.
QCollator makeCollator() {
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    return collator;
}

bool lessEmptyLast(const QCollator &collator, const QString &a, const QString &b) {
    if (a.isEmpty() != b.isEmpty())
        return b.isEmpty();
    return collator.compare(a, b) < 0;
}
}

Backend::Tags Backend::parseProbe(const QByteArray &json) {
    // Tag names differ in case between formats (TITLE in Vorbis, title in ID3).
    QHash<QString, QString> tags;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    const QJsonObject object = root.value("format").toObject().value("tags").toObject();
    for (auto it = object.begin(); it != object.end(); ++it)
        tags.insert(it.key().toLower(), it.value().toString().trimmed());
    // Ogg, Opus and a few other containers keep their tags on the audio
    // stream, not the file. The file's own tags win; a cover picture's don't count.
    for (const QJsonValue &value : root.value("streams").toArray()) {
        const QJsonObject stream = value.toObject();
        if (stream.value("codec_type").toString() != "audio")
            continue;
        const QJsonObject streamTags = stream.value("tags").toObject();
        for (auto it = streamTags.begin(); it != streamTags.end(); ++it)
            if (!tags.contains(it.key().toLower()))
                tags.insert(it.key().toLower(), it.value().toString().trimmed());
    }

    Tags result;
    result.title = tags.value("title");
    // Compilations list the album's artist separately; browse by that.
    for (const char *key : {"album_artist", "albumartist", "artist"}) {
        result.artist = tags.value(key);
        if (!result.artist.isEmpty())
            break;
    }
    result.album = tags.value("album");
    result.track = tags.value("track", tags.value("tracknumber")).section('/', 0, 0).toInt();
    result.disc = tags.value("disc", tags.value("discnumber")).section('/', 0, 0).toInt();
    // The year is the first four digits of whichever date tag is there.
    for (const char *key : {"date", "year", "originaldate", "originalyear"}) {
        result.year = tags.value(key).left(4).toInt();
        if (result.year > 0)
            break;
    }
    // ReplayGain: "-7.50 dB" in ID3 and Vorbis tags, or Opus's R128 gain, a
    // whole number in 1/256 dB against -23 LUFS (ReplayGain aims at -18).
    auto gain = [&](const char *rg, const char *r128) {
        const QString text = tags.value(rg);
        if (!text.isEmpty()) {
            bool ok = false;
            const double value = text.section(' ', 0, 0).toDouble(&ok);
            if (ok)
                return value;
        }
        bool ok = false;
        const double q = tags.value(r128).toDouble(&ok);
        return ok ? q / 256.0 + 5.0 : qQNaN();
    };
    result.trackGain = gain("replaygain_track_gain", "r128_track_gain");
    result.albumGain = gain("replaygain_album_gain", "r128_album_gain");
    // ffprobe reports the length as a string of seconds with decimals.
    result.seconds = qRound(QJsonDocument::fromJson(json).object().value("format").toObject().value("duration").toString().toDouble());
    return result;
}

Backend::Tags Backend::fallbackTags(const QString &relativeName) {
    const QStringList parts = relativeName.split('/');
    Tags tags;
    tags.title = parts.last();
    if (parts.size() >= 3) {
        tags.artist = parts[0];
        tags.album = parts[1];
    } else if (parts.size() == 2) {
        tags.artist = parts[0];
    }
    return tags;
}

Backend::TagResult Backend::readTags(const QStringList &paths, const QString &cacheFile) {
    TagResult result;
    const QString ffprobe = QStandardPaths::findExecutable("ffprobe");
    if (ffprobe.isEmpty()) {
        result.ffprobeMissing = true;
        return result;
    }

    QJsonObject cache;
    QFile cached(cacheFile);
    if (cached.open(QIODevice::ReadOnly))
        cache = QJsonDocument::fromJson(cached.readAll()).object();

    auto fromJson = [](const QJsonObject &o) {
        Tags tags;
        tags.artist = o.value("a").toString();
        tags.album = o.value("b").toString();
        tags.title = o.value("t").toString();
        tags.track = o.value("n").toInt();
        tags.disc = o.value("d").toInt();
        tags.seconds = o.value("u").toInt();
        tags.year = o.value("y").toInt();
        if (o.contains("g"))
            tags.trackGain = o.value("g").toDouble();
        if (o.contains("h"))
            tags.albumGain = o.value("h").toDouble();
        return tags;
    };

    // A cached entry is good while the file's size and modified time match.
    QStringList todo;
    for (const auto &path : paths) {
        const QFileInfo info(path);
        const QJsonObject entry = cache.value(path).toObject();
        if (entry.value("v").toInt() == CacheVersion && entry.value("s").toDouble(-1) == double(info.size()) &&
            entry.value("m").toDouble(-1) == double(info.lastModified().toMSecsSinceEpoch()))
        {
            result.tags.insert(path, fromJson(entry));
            result.tags[path].modified = info.lastModified().toMSecsSinceEpoch();
        } else
            todo << path;
    }

    if (!todo.isEmpty()) {
        const auto probed = QtConcurrent::blockingMapped<QList<Tags>>(todo, [ffprobe](const QString &path) {
            QProcess process;
            process.start(ffprobe, {"-v", "error", "-show_entries", "format=duration:format_tags:stream=codec_type:stream_tags", "-of", "json", path});
            if (!process.waitForFinished(15000)) {
                process.kill();
                return Tags();
            }
            // Tags are read from files that may not be ours; ignore a probe
            // that answers with an absurd amount of text.
            if (process.bytesAvailable() > MaxTextBytes)
                return Tags();
            return parseProbe(process.readAllStandardOutput());
        });
        for (int i = 0; i < todo.size(); ++i) {
            const Tags &tags = probed[i];
            const QFileInfo info(todo[i]);
            result.tags.insert(todo[i], tags);
            result.tags[todo[i]].modified = info.lastModified().toMSecsSinceEpoch();
            // Files that failed to probe are cached as empty, so they aren't
            // retried on every launch.
            QJsonObject entry{{"v", CacheVersion}, {"s", double(info.size())},
                              {"m", double(info.lastModified().toMSecsSinceEpoch())},
                              {"a", tags.artist}, {"b", tags.album}, {"t", tags.title},
                              {"n", tags.track}, {"d", tags.disc}, {"u", tags.seconds}, {"y", tags.year}};
            if (!qIsNaN(tags.trackGain))
                entry.insert("g", tags.trackGain);
            if (!qIsNaN(tags.albumGain))
                entry.insert("h", tags.albumGain);
            cache.insert(todo[i], entry);
        }
        // Forget files that are gone, then save beside the target and rename.
        const QSet<QString> present(paths.begin(), paths.end());
        for (const auto &key : cache.keys())
            if (!present.contains(key))
                cache.remove(key);
        QDir().mkpath(QFileInfo(cacheFile).absolutePath());
        QSaveFile out(cacheFile);
        if (out.open(QIODevice::WriteOnly)) {
            out.write(QJsonDocument(cache).toJson(QJsonDocument::Compact));
            out.commit();
        }
    }
    return result;
}

void Backend::finishScan(const QStringList &paths) {
    m_scanning = false;
    emit scanningChanged();

    const QString current = m_current >= 0 ? m_tracks[m_current].path : QString();
    const QDir root(m_library);
    // A rescan keeps what is already known about the files that are still
    // there, so the lists don't flash back to folder names while tags reload.
    QHash<QString, Track> known;
    if (m_rescanning)
        for (const Track &track : std::as_const(m_tracks))
            known.insert(track.path, track);
    m_tracks.clear();
    for (const auto &path : paths) {
        if (known.contains(path)) {
            m_tracks.append(known.value(path));
            continue;
        }
        Track track;
        track.path = path;
        track.name = root.relativeFilePath(path);
        track.name.chop(QFileInfo(track.name).suffix().size() + 1);
        const Tags guess = fallbackTags(track.name);
        track.artist = guess.artist;
        track.album = guess.album;
        track.title = guess.title;
        m_tracks.append(track);
    }
    m_current = -1;
    for (int i = 0; i < m_tracks.size(); ++i)
        if (m_tracks[i].path == current)
            m_current = i;
    if (!m_rescanning)
        m_allArtists = m_allAlbums = true;
    sortTracks();

    // Queued files that aren't in this library (it changed, or they were
    // deleted) can't be played, so they leave the queue.
    QStringList kept;
    for (const QString &path : std::as_const(m_queue))
        if (indexOfPath(path) >= 0)
            kept << path;
    m_queue = kept;
    emit queueChanged();

    if (m_tracks.isEmpty())
        setError(tr("No music found in %1").arg(m_library));
    if (!m_pending.isEmpty()) {
        for (int row = 0; row < m_visible.size(); ++row)
            if (m_tracks[m_visible[row]].path == m_pending)
                play(row);
        m_pending.clear();
    } else if (!m_resumePath.isEmpty()) {
        // Reopening without a file picks up where the last session stopped,
        // paused, if that track is still in the library.
        for (int row = 0; row < m_visible.size(); ++row) {
            if (m_tracks[m_visible[row]].path == m_resumePath && m_current < 0) {
                m_current = m_visible[row];
                m_resumeAt = m_resumePosition;
                loadCurrent();
            }
        }
        m_resumePath.clear();
    }

    m_rescanning = false;
    watchLibrary(paths);

    // Tags arrive a moment later, and the lists re-sort when they do.
    if (!m_tracks.isEmpty()) {
        m_reading = true;
        emit readingChanged();
        const QString cacheFile = m_cacheFile;
        m_tagWatcher.setFuture(QtConcurrent::run([paths, cacheFile] { return readTags(paths, cacheFile); }));
    }
}

// Every folder that holds music, and the library folder itself, is watched
// for changes. A new folder shows up as a change in its parent.
void Backend::watchLibrary(const QStringList &paths) {
    QSet<QString> folders{m_library};
    for (const QString &path : paths) {
        QString folder = QFileInfo(path).absolutePath();
        while (folder.startsWith(m_library) && !folders.contains(folder)) {
            folders.insert(folder);
            folder = QFileInfo(folder).absolutePath();
        }
    }
    if (!m_folders.directories().isEmpty())
        m_folders.removePaths(m_folders.directories());
    const QStringList failed = m_folders.addPaths(QStringList(folders.begin(), folders.end()));
    // Out of watches (the inotify limit): look for changes now and then instead.
    if (failed.isEmpty())
        m_pollTimer.stop();
    else
        m_pollTimer.start();
}

void Backend::rescan() {
    if (m_scanning || m_rescanning || m_library.isEmpty())
        return;
    if (!QFileInfo(m_library).isDir())
        return;
    m_rescanning = true;
    const QString folder = m_library;
    m_watcher.setFuture(QtConcurrent::run([folder] { return scan(folder); }));
}

void Backend::finishReading(const TagResult &result) {
    m_reading = false;
    emit readingChanged();
    if (result.ffprobeMissing) {
        setError(tr("ffprobe wasn't found, so artists and albums can't be read. Install ffmpeg."));
        return;
    }
    const QDir root(m_library);
    for (Track &track : m_tracks) {
        const Tags guess = fallbackTags(track.name);
        const Tags tags = result.tags.value(track.path);
        // Each missing tag falls back to what the folders suggest.
        track.artist = tags.artist.isEmpty() ? guess.artist : tags.artist;
        track.album = tags.album.isEmpty() ? guess.album : tags.album;
        track.title = tags.title.isEmpty() ? guess.title : tags.title;
        track.track = tags.track;
        track.disc = tags.disc;
        track.seconds = tags.seconds;
        track.year = tags.year;
        track.added = tags.modified;
        track.trackGain = tags.trackGain;
        track.albumGain = tags.albumGain;
    }
    applyGains();
    sortTracks();
}

// Library order is artist, album, disc, track, then title.
void Backend::sortTracks() {
    const QString current = m_current >= 0 ? m_tracks[m_current].path : QString();
    const QCollator collator = makeCollator();
    std::sort(m_tracks.begin(), m_tracks.end(), [&](const Track &a, const Track &b) {
        // The other orders break their ties the way the library is normally laid out.
        switch (m_sortMode) {
        case 1:
            if (a.title != b.title)
                return collator.compare(a.title, b.title) < 0;
            break;
        case 2:
            if (a.year != b.year)
                return a.year == 0 ? false : b.year == 0 ? true : a.year < b.year;
            break;
        case 3:
            if (a.added != b.added)
                return a.added > b.added;
            break;
        case 4:
            if (a.seconds != b.seconds)
                return a.seconds < b.seconds;
            break;
        default:
            break;
        }
        if (a.artist != b.artist)
            return lessEmptyLast(collator, a.artist, b.artist);
        if (a.album != b.album)
            return lessEmptyLast(collator, a.album, b.album);
        if (a.disc != b.disc)
            return a.disc < b.disc;
        if (a.track != b.track)
            return a.track < b.track;
        if (a.title != b.title)
            return collator.compare(a.title, b.title) < 0;
        return collator.compare(a.path, b.path) < 0;
    });
    m_current = -1;
    for (int i = 0; i < m_tracks.size(); ++i)
        if (m_tracks[i].path == current)
            m_current = i;
    refresh();
    // Queue rows are looked up by path, so they appear once the library has
    // loaded and change names when the tags arrive; the pane must be told.
    emit queueChanged();
}

// Rebuilds the three columns from the filter and the artist and album chosen.
void Backend::refresh() {
    const QCollator collator = makeCollator();
    QList<int> matching;
    for (int i = 0; i < m_tracks.size(); ++i) {
        const Track &t = m_tracks[i];
        if (m_filter.isEmpty() || t.artist.contains(m_filter, Qt::CaseInsensitive) ||
            t.album.contains(m_filter, Qt::CaseInsensitive) || t.title.contains(m_filter, Qt::CaseInsensitive) ||
            t.name.contains(m_filter, Qt::CaseInsensitive))
            matching.append(i);
    }

    m_matching = matching;

    auto unique = [&](const QList<int> &indexes, auto field) {
        QStringList values;
        QSet<QString> seen;
        for (int i : indexes) {
            const QString value = field(m_tracks[i]);
            if (!seen.contains(value)) {
                seen.insert(value);
                values << value;
            }
        }
        std::sort(values.begin(), values.end(), [&](const QString &a, const QString &b) { return lessEmptyLast(collator, a, b); });
        return values;
    };

    m_artistValues = unique(matching, [](const Track &t) { return t.artist; });
    if (!m_allArtists && !m_artistValues.contains(m_artistSel))
        m_allArtists = true;

    QList<int> ofArtist;
    for (int i : matching)
        if (m_allArtists || m_tracks[i].artist == m_artistSel)
            ofArtist.append(i);
    m_albumValues = unique(ofArtist, [](const Track &t) { return t.album; });
    if (!m_allAlbums && !m_albumValues.contains(m_albumSel))
        m_allAlbums = true;

    m_visible.clear();
    for (int i : ofArtist)
        if (m_allAlbums || m_tracks[i].album == m_albumSel)
            m_visible.append(i);

    emit browserChanged();
    emit currentChanged();
}

void Backend::setSortMode(int mode) {
    mode = std::clamp(mode, 0, 4);
    if (mode == m_sortMode)
        return;
    m_sortMode = mode;
    sortTracks();
    emit modeChanged();
    scheduleSave();
}

void Backend::cycleSort() {
    setSortMode((m_sortMode + 1) % 5);
}

void Backend::selectArtist(int row) {
    if (row <= 0 || row > m_artistValues.size()) {
        m_allArtists = true;
    } else {
        m_allArtists = false;
        m_artistSel = m_artistValues[row - 1];
    }
    m_allAlbums = true;
    refresh();
}

void Backend::selectAlbum(int row) {
    if (row <= 0 || row > m_albumValues.size()) {
        m_allAlbums = true;
    } else {
        m_allAlbums = false;
        m_albumSel = m_albumValues[row - 1];
    }
    refresh();
}

void Backend::setFilter(const QString &filter) {
    if (filter == m_filter)
        return;
    m_filter = filter;
    refresh();
}

// A track number in front, when the tags have one, like a record's sleeve.
QVariantList Backend::titleSeconds() const {
    QVariantList lengths;
    for (int index : m_visible)
        lengths << m_tracks[index].seconds;
    return lengths;
}

QStringList Backend::titles() const {
    QStringList names;
    for (int index : m_visible) {
        const Track &t = m_tracks[index];
        // A track number only means something in album order.
        names << (t.track > 0 && m_sortMode == 0 ? QString("%1  %2").arg(t.track, 2, 10, QChar('0')).arg(t.title) : t.title);
    }
    return names;
}

int Backend::currentRow() const {
    return int(m_visible.indexOf(m_current));
}

void Backend::setError(const QString &message) {
    if (message == m_error)
        return;
    m_error = message;
    emit errorChanged();
}

void Backend::play(int row) {
    if (row < 0 || row >= m_visible.size())
        return;
    m_current = m_visible[row];
    m_resumeAt = -1;
    setError({});
    loadCurrent();
    player().play();
}

bool Backend::playPath(const QString &path) {
    const int index = indexOfPath(path);
    if (index < 0)
        return false;
    m_current = index;
    m_resumeAt = -1;
    setError({});
    loadCurrent();
    player().play();
    return true;
}

void Backend::pushHistory(const QString &path) {
    m_history.append(path);
    while (m_history.size() > 200)
        m_history.removeFirst();
}

// Makes m_current the track in the active voice. When the next track was
// already loaded in the other voice, that voice takes over; a natural
// handoff leaves the old one playing out or fading, a manual one stops it.
void Backend::loadCurrent(Handoff handoff) {
    const QString path = m_tracks[m_current].path;
    if (!m_goingBack && !m_lastPath.isEmpty() && m_lastPath != path)
        pushHistory(m_lastPath);
    m_goingBack = false;
    m_lastPath = path;
    m_planned.clear();

    m_clockBase = 0;
    m_clock.restart();
    m_reportedPosition = 0;
    m_reportClock.restart();
    m_buffers.clear();
    m_analyzer.clear();
    m_title = m_tracks[m_current].title;
    m_artist = m_tracks[m_current].artist;
    m_album = m_tracks[m_current].album;
    updateCover({});

    const int other = 1 - m_active;
    if (idle().ready && idle().path == path && m_fadeVoice != other) {
        const int old = m_active;
        m_active = other;
        if (handoff == Handoff::Natural) {
            m_fadeVoice = old;
            m_fadeClock.start();
            active().fade = m_fadeMs > 0 ? 0.0 : 1.0;
            m_voices[old]->fade = 1.0;
        } else {
            finishFade();
            Voice &previous = *m_voices[old];
            previous.player.stop();
            previous.player.setSource({});
            previous.path.clear();
            previous.ready = false;
        }
        readMetadata();
    } else {
        finishFade();
        Voice &voice = active();
        const QUrl url = QUrl::fromLocalFile(path);
        const bool sameSource = voice.player.source() == url;
        voice.path = path;
        voice.player.setSource(url);
        // Setting the source it already has changes nothing, so no metadata
        // signal follows and the cover cleared above would never come back.
        if (sameSource)
            readMetadata();
    }
    applyGains();
    loadLyrics();
    emit currentChanged();
    schedulePlan();
}

// Per-voice loudness: the volume slider, ReplayGain, a crossfade in progress
// and the sleep timer's fade-out all multiply into one linear gain.
void Backend::applyGains() {
    // The slider is perceptual; the audio device wants a linear gain.
    const double master = QtAudio::convertVolume(m_volume, QtAudio::LogarithmicVolumeScale, QtAudio::LinearVolumeScale);
    for (auto &voice : m_voices) {
        voice->out.setVolume(std::clamp(master * leveling(voice->path) * voice->fade * m_sleepFactor, 0.0, 1.0));
        voice->out.setMuted(m_muted);
    }
}

// ReplayGain as a factor. A file missing the chosen kind of gain uses the
// other kind, and one with neither plays as it is. The output can't go above
// full scale, so a boost only has room below full volume.
double Backend::leveling(const QString &path) const {
    if (m_replayGain == 0 || path.isEmpty())
        return 1.0;
    const int index = indexOfPath(path);
    if (index < 0)
        return 1.0;
    const Track &track = m_tracks[index];
    double gain = m_replayGain == 2 ? track.albumGain : track.trackGain;
    if (qIsNaN(gain))
        gain = m_replayGain == 2 ? track.trackGain : track.albumGain;
    return qIsNaN(gain) ? 1.0 : std::pow(10.0, std::clamp(gain, -30.0, 15.0) / 20.0);
}

void Backend::readMetadata() {
    if (m_current < 0)
        return;
    const auto data = player().metaData();
    const Track &track = m_tracks[m_current];
    const QString title = data.stringValue(QMediaMetaData::Title);
    QString artist = data.value(QMediaMetaData::ContributingArtist).toStringList().join(", ");
    if (artist.isEmpty())
        artist = data.stringValue(QMediaMetaData::AlbumArtist);
    if (artist.isEmpty())
        artist = data.value(QMediaMetaData::Author).toStringList().join(", ");
    const QString album = data.stringValue(QMediaMetaData::AlbumTitle);
    m_title = title.isEmpty() ? track.title : title;
    m_artist = artist.isEmpty() ? track.artist : artist;
    m_album = album.isEmpty() ? track.album : album;
    QImage art = data.value(QMediaMetaData::CoverArtImage).value<QImage>();
    if (art.isNull())
        art = data.value(QMediaMetaData::ThumbnailImage).value<QImage>();
    if (!art.isNull())
        updateCover(art);
    emit currentChanged();
}

QString Backend::findFolderCover(const QString &folder) {
    static const QStringList extensions{"jpg", "jpeg", "jfif", "png", "webp", "bmp", "gif", "tif", "tiff"};
    QFileInfoList images;
    for (const QFileInfo &info : QDir(folder).entryInfoList(QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase))
        if (extensions.contains(info.suffix().toLower()))
            images << info;
    if (images.isEmpty())
        return {};

    // Names players and rippers use for the front cover, best first. Matching
    // ignores case, so Cover.JPG and folder.jpg both count; Windows Media
    // Player's AlbumArt_{GUID}_Large.jpg starts with "albumart".
    for (const char *name : {"cover", "folder", "front", "album", "albumart", "thumb"}) {
        for (const QFileInfo &info : std::as_const(images)) {
            const QString stem = info.completeBaseName().toLower();
            if (stem == name || (QString(name) == "albumart" && stem.startsWith("albumart") && !stem.contains("small")))
                return info.filePath();
        }
    }
    // A folder with exactly one image is almost certainly the cover, whatever
    // it is called. With several and no clear name, guessing would often pick
    // a booklet scan or a back cover, so none is better.
    return images.size() == 1 ? images.first().filePath() : QString();
}

QImage Backend::loadScaledImage(const QString &path, int limit) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QSize size = reader.size();
    if (size.isValid() && (size.width() > limit || size.height() > limit)) {
        size.scale(limit, limit, Qt::KeepAspectRatio);
        reader.setScaledSize(size);
    }
    return reader.read();
}

// Embedded art wins; otherwise the folder's cover image. Art is kept at a size
// that stays sharp in the window without holding a huge scan in memory.
void Backend::updateCover(const QImage &embedded) {
    QImage image = embedded;
    if (image.isNull() && m_current >= 0) {
        const QString folder = QFileInfo(m_tracks[m_current].path).absolutePath();
        if (folder != m_coverFolder) {
            const QString file = findFolderCover(folder);
            m_coverFolder = folder;
            m_coverFolderImage = file.isEmpty() ? QImage() : loadScaledImage(file);
        }
        image = m_coverFolderImage;
    }
    if (image.width() > 800 || image.height() > 800)
        image = image.scaled(800, 800, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_cover = image;
    ++m_coverId;
    emit coverChanged();
}

void Backend::toggle() {
    if (m_current < 0) {
        play(0);
    } else if (playing()) {
        // A fade in progress would leave the old track playing on.
        finishFade();
        player().pause();
    } else {
        player().play();
    }
}

int Backend::nextRow(bool wrap) const {
    if (m_visible.isEmpty())
        return -1;
    const int row = currentRow();
    if (m_shuffle) {
        if (m_visible.size() == 1)
            return 0;
        int target;
        do {
            target = int(QRandomGenerator::global()->bounded(m_visible.size()));
        } while (target == row);
        return target;
    }
    if (row >= 0)
        return row + 1 < m_visible.size() ? row + 1 : wrap ? 0 : -1;
    // The playing track isn't in this list: carry on after its place in the
    // library, which the visible indexes follow in order.
    for (int r = 0; r < m_visible.size(); ++r)
        if (m_visible[r] > m_current)
            return r;
    return wrap ? 0 : -1;
}

// The track that follows the current one: the queue first, then the library.
// It does not change anything, so it can be asked ahead of time.
QString Backend::computeNext(bool wrap) const {
    for (const QString &path : m_queue)
        if (indexOfPath(path) >= 0)
            return path;
    const int row = nextRow(wrap);
    return row >= 0 ? m_tracks[m_visible[row]].path : QString();
}

// A track taken from the queue leaves it, along with any entries ahead of it
// that were no longer in the library. Playing through the queue is not an
// edit to undo.
static void consumeQueued(QStringList &queue, const QString &path, bool &changed) {
    changed = false;
    if (!queue.contains(path))
        return;
    while (!queue.isEmpty() && queue.takeFirst() != path) {}
    changed = true;
}

void Backend::next() {
    // The planned track is the one the user was told comes next, which
    // matters when shuffling; otherwise ask afresh, wrapping at the end.
    QString path = m_planned.isEmpty() || indexOfPath(m_planned) < 0 ? computeNext(true) : m_planned;
    if (path.isEmpty())
        return;
    bool changed;
    consumeQueued(m_queue, path, changed);
    if (changed) {
        m_queueUndo.clear();
        m_queueRedo.clear();
        emit queueChanged();
        scheduleSave();
    }
    playPath(path);
}

void Backend::previous() {
    if (m_visible.isEmpty())
        return;
    // Like most players: a second press within the first seconds goes back a
    // track, otherwise it restarts this one.
    if (position() > 3000) {
        seek(0);
        return;
    }
    // Shuffled, the track before this one is whichever was really playing.
    if (m_shuffle) {
        while (!m_history.isEmpty()) {
            const QString path = m_history.takeLast();
            if (indexOfPath(path) >= 0) {
                m_goingBack = true;
                playPath(path);
                return;
            }
        }
    }
    const int row = currentRow();
    play(row <= 0 ? int(m_visible.size()) - 1 : row - 1);
}

void Backend::advanceAfterEnd() {
    if (m_sleepAtEnd) {
        endSleep();
        return;
    }
    if (m_repeat == RepeatOne) {
        player().setPosition(0);
        player().play();
        return;
    }
    const QString path = m_planned.isEmpty() || indexOfPath(m_planned) < 0 ? computeNext(m_repeat == RepeatAll) : m_planned;
    if (path.isEmpty())
        return;
    bool changed;
    consumeQueued(m_queue, path, changed);
    if (changed) {
        m_queueUndo.clear();
        m_queueRedo.clear();
        emit queueChanged();
        scheduleSave();
    }
    playPath(path);
}

// Chooses the track after this one and loads it into the idle voice, so the
// handoff can happen the moment it is due.
void Backend::schedulePlan() {
    m_planTimer.start();
}

void Backend::planNext() {
    if (m_fadeVoice >= 0)
        return;  // the fading voice isn't free yet; finishFade plans again
    QString want;
    if (m_current >= 0 && !m_sleepAtEnd) {
        if (m_repeat == RepeatOne) {
            want = m_tracks[m_current].path;
        } else if (m_shuffle && m_queue.isEmpty() && !m_planned.isEmpty() && indexOfPath(m_planned) >= 0
                   && m_planned != m_tracks[m_current].path) {
            want = m_planned;  // keep the random pick instead of rolling again
        } else {
            want = computeNext(m_repeat == RepeatAll);
        }
    }
    m_planned = want;
    if (want.isEmpty())
        return;
    Voice &voice = idle();
    if (voice.path != want) {
        voice.path = want;
        voice.ready = false;
        voice.player.setSource(QUrl::fromLocalFile(want));
        applyGains();
    }
}

// Starts the planned track in the idle voice, overlapping the end of this one.
void Backend::beginTransition() {
    const int index = indexOfPath(m_planned);
    if (index < 0)
        return;
    bool changed;
    consumeQueued(m_queue, m_planned, changed);
    if (changed) {
        m_queueUndo.clear();
        m_queueRedo.clear();
        emit queueChanged();
        scheduleSave();
    }
    m_current = index;
    m_resumeAt = -1;
    setError({});
    loadCurrent(Handoff::Natural);
    player().play();
}

void Backend::finishFade() {
    if (m_fadeVoice >= 0) {
        Voice &old = *m_voices[m_fadeVoice];
        m_fadeVoice = -1;
        old.player.stop();
        old.player.setSource({});
        old.path.clear();
        old.ready = false;
        old.fade = 1.0;
        schedulePlan();
    }
    active().fade = 1.0;
    applyGains();
}

// Runs every 10 ms while anything is moving: the crossfade, the sleep timer's
// fade-out, and watching for the moment the next track has to start.
void Backend::tickTransition() {
    if (m_fadeVoice >= 0) {
        const qint64 elapsed = m_fadeClock.elapsed();
        if (m_fadeMs > 0) {
            const double t = std::clamp(double(elapsed) / double(m_fadeMs), 0.0, 1.0);
            // Equal power keeps the loudness steady through the overlap.
            active().fade = std::sin(t * M_PI / 2);
            m_voices[m_fadeVoice]->fade = std::cos(t * M_PI / 2);
            applyGains();
            if (t >= 1.0)
                finishFade();
        } else if (elapsed >= 600) {
            finishFade();  // a gapless handoff: the old track has played out
        }
    }

    if (m_sleepSetting > 0) {
        const qint64 left = m_sleepMs - m_sleepClock.elapsed();
        const double factor = left >= 10000 ? 1.0 : std::max(0.0, double(left) / 10000.0);
        if (factor != m_sleepFactor) {
            m_sleepFactor = factor;
            applyGains();
        }
        if (left <= 0) {
            finishFade();
            player().pause();
            endSleep();
        }
    }

    if (playing() && m_fadeVoice < 0 && !m_planned.isEmpty() && idle().ready && idle().path == m_planned) {
        const qint64 length = duration();
        if (length > 0) {
            const qint64 left = length - (m_reportedPosition + (m_reportClock.isValid() ? m_reportClock.elapsed() : 0));
            // A crossfade needs a track long enough to hold it.
            const qint64 fade = m_crossfade > 0 && length > 4000LL * m_crossfade ? 1000LL * m_crossfade : 0;
            if (left <= (fade > 0 ? fade : GaplessLeadMs)) {
                m_fadeMs = fade;
                beginTransition();
            }
        }
    }

    if (!playing() && m_fadeVoice < 0 && m_sleepSetting <= 0)
        m_transitionTimer.stop();
}

int Backend::sleepRemaining() const {
    if (m_sleepSetting <= 0)
        return 0;
    return int(std::max<qint64>(0, (m_sleepMs - m_sleepClock.elapsed() + 999) / 1000));
}

void Backend::setSleep(int setting) {
    if (setting == 0) {
        endSleep();
        return;
    }
    m_sleepSetting = setting;
    m_sleepAtEnd = setting < 0;
    m_sleepFactor = 1.0;
    if (setting > 0) {
        m_sleepMs = qint64(setting) * 60000;
        m_sleepClock.start();
        m_sleepTimer.start();
        m_transitionTimer.start();
    } else {
        m_sleepTimer.stop();
    }
    m_planned.clear();
    schedulePlan();
    emit sleepChanged();
}

void Backend::endSleep() {
    const bool wasSet = m_sleepSetting != 0;
    m_sleepSetting = 0;
    m_sleepAtEnd = false;
    m_sleepFactor = 1.0;
    m_sleepTimer.stop();
    applyGains();
    schedulePlan();
    if (wasSet)
        emit sleepChanged();
}

void Backend::cycleSleep() {
    static const QList<int> steps{0, 15, 30, 45, 60, 90, -1};
    const int at = int(steps.indexOf(m_sleepSetting));
    // A running countdown counts as its nearest step, so the key moves on from it.
    int index = at;
    if (at < 0) {
        index = 0;
        for (int i = 1; i < steps.size() - 1; ++i)
            if (steps[i] <= m_sleepSetting)
                index = i;
    }
    setSleep(steps[(index + 1) % steps.size()]);
}

void Backend::setReplayGain(int mode) {
    mode = std::clamp(mode, 0, 2);
    if (mode == m_replayGain)
        return;
    m_replayGain = mode;
    applyGains();
    emit modeChanged();
}

void Backend::cycleReplayGain() {
    setReplayGain((m_replayGain + 1) % 3);
}

void Backend::setCrossfade(int seconds) {
    seconds = std::clamp(seconds, 0, 12);
    if (seconds == m_crossfade)
        return;
    m_crossfade = seconds;
    emit modeChanged();
}

void Backend::cycleCrossfade() {
    static const QList<int> steps{0, 2, 4, 6, 8, 10, 12};
    for (int step : steps)
        if (step > m_crossfade) {
            setCrossfade(step);
            return;
        }
    setCrossfade(0);
}

QVariantList Backend::lyricTimes() const {
    QVariantList times;
    for (qint64 time : m_lyrics.times)
        times << double(time);
    return times;
}

int Backend::lyricIndexAt(qint64 ms) const {
    // The last line that has started.
    const auto it = std::upper_bound(m_lyrics.times.begin(), m_lyrics.times.end(), ms);
    return int(it - m_lyrics.times.begin()) - 1;
}

Backend::Lyrics Backend::parseLyrics(const QString &text) {
    static const QRegularExpression stamp(R"(\[(\d{1,3}):(\d{2})(?:[.:](\d{1,3}))?\])");
    static const QRegularExpression meta(R"(^\[[A-Za-z]+:[^\]]*\]$)");  // [ar:Artist] and the like
    QList<QPair<qint64, QString>> timed;
    QStringList plain;
    for (QString line : text.split('\n')) {
        line.remove('\r');
        QList<qint64> stamps;
        int end = 0;
        for (auto it = stamp.globalMatch(line); it.hasNext();) {
            const auto match = it.next();
            if (match.capturedStart() != end)
                break;
            const QString fraction = match.captured(3).leftJustified(3, '0').left(3);
            stamps << (match.captured(1).toLongLong() * 60 + match.captured(2).toLongLong()) * 1000 + fraction.toInt();
            end = match.capturedEnd();
        }
        if (!stamps.isEmpty()) {
            const QString rest = line.mid(end).trimmed();
            for (qint64 time : std::as_const(stamps))
                timed.append({time, rest});
        } else if (!meta.match(line.trimmed()).hasMatch()) {
            plain << line.trimmed();
        }
    }
    Lyrics lyrics;
    if (!timed.isEmpty()) {
        std::stable_sort(timed.begin(), timed.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        for (const auto &line : std::as_const(timed)) {
            lyrics.times << line.first;
            lyrics.lines << line.second;
        }
    } else {
        while (!plain.isEmpty() && plain.last().isEmpty())
            plain.removeLast();
        while (!plain.isEmpty() && plain.first().isEmpty())
            plain.removeFirst();
        lyrics.lines = plain;
    }
    return lyrics;
}

Backend::Lyrics Backend::readLyrics(const QString &path, const QString &ffprobe) {
    // A .lrc file beside the track wins: it is what someone put there on purpose.
    const QFileInfo info(path);
    for (const QFileInfo &file : QDir(info.path()).entryInfoList({info.completeBaseName() + ".*"}, QDir::Files)) {
        if (file.suffix().compare("lrc", Qt::CaseInsensitive) == 0) {
            QFile lrc(file.filePath());
            if (lrc.open(QIODevice::ReadOnly)) {
                // A lyrics file is a few kilobytes; cap what is read so a huge file can't fill memory.
                const Lyrics lyrics = parseLyrics(QString::fromUtf8(lrc.read(MaxTextBytes)));
                if (!lyrics.lines.isEmpty())
                    return lyrics;
            }
        }
    }
    if (ffprobe.isEmpty())
        return {};
    QProcess process;
    process.start(ffprobe, {"-v", "error", "-show_entries", "format_tags:stream_tags", "-of", "json", path});
    if (!process.waitForFinished(15000)) {
        process.kill();
        return {};
    }
    // Tag names vary: LYRICS, UNSYNCEDLYRICS, USLT, lyrics-eng, SYNCEDLYRICS.
    if (process.bytesAvailable() > MaxTextBytes)
        return {};
    const QJsonObject root = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
    QList<QJsonObject> sources{root.value("format").toObject().value("tags").toObject()};
    for (const QJsonValue &stream : root.value("streams").toArray())
        sources << stream.toObject().value("tags").toObject();
    QString best;
    for (const QJsonObject &tags : std::as_const(sources))
        for (auto it = tags.begin(); it != tags.end(); ++it) {
            const QString key = it.key().toLower();
            if ((key.contains("lyrics") || key == "uslt") && it.value().toString().size() > best.size())
                best = it.value().toString();
        }
    return parseLyrics(best);
}

// Lyrics are only fetched while the view is open, and only for the track
// that is playing now.
void Backend::loadLyrics() {
    m_lyrics = {};
    emit lyricsChanged();
    if (!m_showLyrics || m_lastPath.isEmpty())
        return;
    m_lyricsFor = m_lastPath;
    const QString path = m_lastPath, ffprobe = QStandardPaths::findExecutable("ffprobe");
    m_lyricsWatcher.setFuture(QtConcurrent::run([path, ffprobe] { return readLyrics(path, ffprobe); }));
}

void Backend::setShowLyrics(bool show) {
    if (show == m_showLyrics)
        return;
    m_showLyrics = show;
    loadLyrics();
    emit modeChanged();
    scheduleSave();
}

void Backend::seek(qint64 ms) {
    finishFade();
    m_buffers.clear();
    m_clockBase = std::clamp<qint64>(ms, 0, std::max<qint64>(0, duration()));
    m_clock.restart();
    player().setPosition(m_clockBase);
    m_reportedPosition = m_clockBase;
    m_reportClock.restart();
    emit seeked(m_clockBase);
}

void Backend::seekBy(qint64 ms) {
    seek(position() + ms);
}

void Backend::setVolume(qreal volume) {
    m_volume = std::clamp(volume, 0.0, 1.0);
    applyGains();
    emit volumeChanged();
}

void Backend::changeVolume(qreal delta) {
    m_muted = false;
    setVolume(m_volume + delta);
}

void Backend::toggleMute() {
    m_muted = !m_muted;
    applyGains();
    emit volumeChanged();
}

void Backend::toggleShuffle() {
    setShuffle(!m_shuffle);
}

void Backend::cycleRepeat() {
    setRepeat((m_repeat + 1) % 3);
}

void Backend::setShuffle(bool shuffle) {
    if (shuffle == m_shuffle)
        return;
    m_shuffle = shuffle;
    emit modeChanged();
}

void Backend::setRepeat(int repeat) {
    repeat = std::clamp(repeat, 0, 2);
    if (repeat == m_repeat)
        return;
    m_repeat = repeat;
    emit modeChanged();
}

void Backend::cycleVisualizer() {
    setVisualizerStyle((m_visualizerStyle + 1) % 3);
}

void Backend::setVisualizerStyle(int style) {
    style = std::clamp(style, 0, 2);
    if (style == m_visualizerStyle)
        return;
    m_visualizerStyle = style;
    emit modeChanged();
}

// Rates people can step through with one key; the state file takes any value.
static const QList<int> RatePresets{30, 60, 120, 144, 240};

void Backend::cycleVisualizerRate() {
    // The next preset above the current rate, wrapping around to the lowest.
    // A custom rate lands on the next one up.
    for (int preset : RatePresets) {
        if (preset > m_visualizerRate) {
            setVisualizerRate(preset);
            return;
        }
    }
    setVisualizerRate(RatePresets.first());
}

void Backend::setVisualizerRate(int hz) {
    // Zero or less means the default, so an old state file that asked to
    // follow the display gets 60.
    hz = hz <= 0 ? DefaultVisualizerRate : std::clamp(hz, 10, 360);
    if (hz == m_visualizerRate)
        return;
    m_visualizerRate = hz;
    configureFrames();
    emit modeChanged();
    scheduleSave();
}

// The bars update on a precise timer at the chosen rate, and the cava-style
// smoothing is told the rate so the bars fall at the same speed at any of them.
void Backend::configureFrames() {
    const bool running = m_frames.isActive();
    m_frames.setInterval(std::max(1, qRound(1000.0 / m_visualizerRate)));
    m_analyzer.setFramerate(m_visualizerRate);
    if (running)
        m_frames.start();
}

void Backend::startFrames() {
    if (!m_frames.isActive())
        m_frames.start();
}

void Backend::stopFrames() {
    m_frames.stop();
}

void Backend::stop() {
    finishFade();
    player().stop();
    m_buffers.clear();
}

QString Backend::playbackState() const {
    switch (player().playbackState()) {
    case QMediaPlayer::PlayingState: return "Playing";
    case QMediaPlayer::PausedState: return "Paused";
    default: return "Stopped";
    }
}

// Runs once per visualizer frame: feed the analyzer the audio that has just
// reached the speakers, and publish the bar heights.
void Backend::tick() {
    QVector<float> levels;
    if (playing()) {
        // The player reports its position in coarse steps, which would feed the
        // analyzer in bursts and make the bars stutter, so interpolate between
        // reports with a steady clock. Buffer times are in microseconds.
        const qint64 now = (m_clockBase + m_clock.elapsed()) * 1000;
        while (!m_buffers.isEmpty() && m_buffers.head().startTime() <= now)
            m_analyzer.push(m_buffers.dequeue());
        levels = m_analyzer.compute();
    } else {
        levels = m_analyzer.decay();
        if (std::all_of(levels.begin(), levels.end(), [](float v) { return v == 0.0f; }))
            stopFrames();
    }

    QVariantList list;
    list.reserve(levels.size());
    for (float level : levels)
        list << double(level);
    m_levelList = list;
    emit levelsChanged();
}
