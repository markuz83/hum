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
#include <algorithm>

namespace {
const QStringList Extensions{"mp3", "ogg", "oga", "opus", "flac", "wav", "m4a", "aac", "wma", "mka", "weba"};
}

Backend::Backend(QObject *parent) : QObject(parent), m_tap(this) {
    m_audio.setVolume(QtAudio::convertVolume(m_volume, QtAudio::LogarithmicVolumeScale, QtAudio::LinearVolumeScale));
    m_player.setAudioOutput(&m_audio);
    // The visualizer reads the same decoded audio the speakers get.
    m_player.setAudioBufferOutput(&m_tap);
    connect(&m_tap, &QAudioBufferOutput::audioBufferReceived, this, [this](const QAudioBuffer &buffer) {
        m_buffers.enqueue(buffer);
        // Decoding runs ahead of playback; bound the backlog so a long pause
        // can't grow it without limit.
        while (m_buffers.size() > 600)
            m_buffers.dequeue();
    });

    connect(&m_player, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
        // Never step the clock backwards for a small report lag, only for a real seek.
        const qint64 estimate = m_clockBase + m_clock.elapsed();
        if (position > estimate || estimate - position > 250) {
            m_clockBase = position;
            m_clock.restart();
        }
        emit positionChanged();
    });
    connect(&m_player, &QMediaPlayer::durationChanged, this, &Backend::durationChanged);
    connect(&m_player, &QMediaPlayer::metaDataChanged, this, &Backend::readMetadata);
    connect(&m_player, &QMediaPlayer::playbackStateChanged, this, [this] {
        emit playingChanged();
        // Remember the position while playing, and exactly where it stopped.
        if (playing())
            m_positionTimer.start();
        else
            m_positionTimer.stop();
        scheduleSave();
        if (playing()) {
            m_clockBase = m_player.position();
            m_clock.restart();
            startFrames();
        }
    });
    connect(&m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia)
            advanceAfterEnd();
        // A resumed track waits, paused, at the spot where Hum was closed.
        if (status == QMediaPlayer::LoadedMedia && m_resumeAt >= 0) {
            m_clockBase = m_resumeAt;
            m_clock.restart();
            m_player.setPosition(m_resumeAt);
            m_resumeAt = -1;
            emit positionChanged();
        }
    });
    connect(&m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &message) {
        setError(tr("Can't play %1: %2").arg(m_title, message));
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
    connect(&m_frames, &QTimer::timeout, this, [this] {
        // Following the display, the window's frame clock drives the bars and
        // this timer only acts when no frame has come for a while, which is
        // the window being hidden.
        if (m_visualizerRate == 0 && m_lastTick.isValid() && m_lastTick.elapsed() < 50)
            return;
        tick();
    });
    connect(&m_watcher, &QFutureWatcher<QStringList>::finished, this, [this] { finishScan(m_watcher.result()); });
    connect(&m_tagWatcher, &QFutureWatcher<TagResult>::finished, this, [this] { finishReading(m_tagWatcher.result()); });
    m_cacheFile = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/tags.json";
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
                      {"visualizer", m_visualizerStyle}, {"visualizerRate", m_visualizerRate}, {"track", currentPath()},
                      {"position", double(m_resumeAt >= 0 ? m_resumeAt : m_player.position())},
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
    const QJsonObject object = QJsonDocument::fromJson(json).object().value("format").toObject().value("tags").toObject();
    for (auto it = object.begin(); it != object.end(); ++it)
        tags.insert(it.key().toLower(), it.value().toString().trimmed());

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
        return tags;
    };

    // A cached entry is good while the file's size and modified time match.
    QStringList todo;
    for (const auto &path : paths) {
        const QFileInfo info(path);
        const QJsonObject entry = cache.value(path).toObject();
        if (entry.value("s").toDouble(-1) == double(info.size()) &&
            entry.value("m").toDouble(-1) == double(info.lastModified().toMSecsSinceEpoch()))
            result.tags.insert(path, fromJson(entry));
        else
            todo << path;
    }

    if (!todo.isEmpty()) {
        const auto probed = QtConcurrent::blockingMapped<QList<Tags>>(todo, [ffprobe](const QString &path) {
            QProcess process;
            process.start(ffprobe, {"-v", "error", "-show_entries", "format_tags", "-of", "json", path});
            if (!process.waitForFinished(15000)) {
                process.kill();
                return Tags();
            }
            return parseProbe(process.readAllStandardOutput());
        });
        for (int i = 0; i < todo.size(); ++i) {
            const Tags &tags = probed[i];
            const QFileInfo info(todo[i]);
            result.tags.insert(todo[i], tags);
            // Files that failed to probe are cached as empty, so they aren't
            // retried on every launch.
            cache.insert(todo[i], QJsonObject{{"s", double(info.size())},
                                              {"m", double(info.lastModified().toMSecsSinceEpoch())},
                                              {"a", tags.artist}, {"b", tags.album}, {"t", tags.title},
                                              {"n", tags.track}, {"d", tags.disc}});
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
    m_tracks.clear();
    for (const auto &path : paths) {
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

    // Tags arrive a moment later, and the lists re-sort when they do.
    if (!m_tracks.isEmpty()) {
        m_reading = true;
        emit readingChanged();
        const QString cacheFile = m_cacheFile;
        m_tagWatcher.setFuture(QtConcurrent::run([paths, cacheFile] { return readTags(paths, cacheFile); }));
    }
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
    }
    sortTracks();
}

// Library order is artist, album, disc, track, then title.
void Backend::sortTracks() {
    const QString current = m_current >= 0 ? m_tracks[m_current].path : QString();
    const QCollator collator = makeCollator();
    std::sort(m_tracks.begin(), m_tracks.end(), [&](const Track &a, const Track &b) {
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
QStringList Backend::titles() const {
    QStringList names;
    for (int index : m_visible) {
        const Track &t = m_tracks[index];
        names << (t.track > 0 ? QString("%1  %2").arg(t.track, 2, 10, QChar('0')).arg(t.title) : t.title);
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
    m_player.play();
}

bool Backend::playPath(const QString &path) {
    const int index = indexOfPath(path);
    if (index < 0)
        return false;
    m_current = index;
    m_resumeAt = -1;
    setError({});
    loadCurrent();
    m_player.play();
    return true;
}

void Backend::loadCurrent() {
    m_clockBase = 0;
    m_clock.restart();
    m_buffers.clear();
    m_analyzer.clear();
    m_title = m_tracks[m_current].title;
    m_artist = m_tracks[m_current].artist;
    m_album = m_tracks[m_current].album;
    updateCover({});
    const QUrl url = QUrl::fromLocalFile(m_tracks[m_current].path);
    const bool sameSource = m_player.source() == url;
    m_player.setSource(url);
    // Setting the source it already has changes nothing, so no metadata
    // signal follows and the cover cleared above would never come back.
    if (sameSource)
        readMetadata();
    emit currentChanged();
}

void Backend::readMetadata() {
    if (m_current < 0)
        return;
    const auto data = m_player.metaData();
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
        m_player.pause();
    } else {
        m_player.play();
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

void Backend::next() {
    // The queue plays first; entries that are no longer in the library are skipped.
    while (!m_queue.isEmpty()) {
        const QString path = m_queue.first();
        m_queue.removeFirst();
        // Playing through the queue is not an edit to undo.
        m_queueUndo.clear();
        m_queueRedo.clear();
        emit queueChanged();
        scheduleSave();
        if (playPath(path))
            return;
    }
    const int row = nextRow(true);
    if (row >= 0)
        play(row);
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
    const int row = currentRow();
    play(row <= 0 ? int(m_visible.size()) - 1 : row - 1);
}

void Backend::advanceAfterEnd() {
    if (m_repeat == RepeatOne) {
        m_player.setPosition(0);
        m_player.play();
    } else if (!m_queue.isEmpty()) {
        next();
    } else {
        const int row = nextRow(m_repeat == RepeatAll);
        if (row >= 0)
            play(row);
    }
}

void Backend::seek(qint64 ms) {
    m_buffers.clear();
    m_clockBase = std::clamp<qint64>(ms, 0, std::max<qint64>(0, duration()));
    m_clock.restart();
    m_player.setPosition(m_clockBase);
    emit seeked(m_clockBase);
}

void Backend::seekBy(qint64 ms) {
    seek(position() + ms);
}

void Backend::setVolume(qreal volume) {
    m_volume = std::clamp(volume, 0.0, 1.0);
    // The slider is perceptual; the audio device wants a linear gain.
    m_audio.setVolume(QtAudio::convertVolume(m_volume, QtAudio::LogarithmicVolumeScale, QtAudio::LinearVolumeScale));
    emit volumeChanged();
}

void Backend::changeVolume(qreal delta) {
    m_audio.setMuted(false);
    setVolume(m_volume + delta);
}

void Backend::toggleMute() {
    m_audio.setMuted(!m_audio.isMuted());
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
static const QList<int> RatePresets{0, 30, 60, 120, 144, 240};

void Backend::cycleVisualizerRate() {
    // The next preset above the current rate, wrapping back to the display.
    // The display (0) sorts first, so a custom rate lands on the next one up.
    for (int preset : RatePresets) {
        if (preset != 0 && preset > m_visualizerRate && m_visualizerRate != 0) {
            setVisualizerRate(preset);
            return;
        }
    }
    setVisualizerRate(m_visualizerRate == 0 ? RatePresets.at(1) : 0);
}

void Backend::setVisualizerRate(int hz) {
    hz = hz <= 0 ? 0 : std::clamp(hz, 10, 360);
    if (hz == m_visualizerRate)
        return;
    m_visualizerRate = hz;
    configureFrames();
    emit modeChanged();
    scheduleSave();
}

void Backend::setRefreshRate(double hz) {
    if (hz < 10 || qFuzzyCompare(hz, m_refreshRate))
        return;
    m_refreshRate = hz;
    configureFrames();
    emit modeChanged();
}

// A fixed rate is a precise timer. Following the display, the window drives
// frame() from its own frame clock and this timer is only a slow backstop, so
// audio keeps draining while the window is hidden and draws nothing.
void Backend::configureFrames() {
    const bool running = m_frames.isActive();
    if (m_visualizerRate > 0) {
        m_frames.setInterval(std::max(1, qRound(1000.0 / m_visualizerRate)));
        m_analyzer.setFramerate(m_visualizerRate);
    } else {
        m_frames.setInterval(50);
        m_analyzer.setFramerate(qRound(m_refreshRate));
    }
    if (running)
        m_frames.start();
}

void Backend::startFrames() {
    if (!m_frames.isActive())
        m_frames.start();
    if (!m_animating) {
        m_animating = true;
        emit animatingChanged();
    }
}

void Backend::stopFrames() {
    m_frames.stop();
    if (m_animating) {
        m_animating = false;
        emit animatingChanged();
    }
}

void Backend::frame() {
    if (m_visualizerRate == 0)
        tick();
}

void Backend::stop() {
    m_player.stop();
    m_buffers.clear();
}

QString Backend::playbackState() const {
    switch (m_player.playbackState()) {
    case QMediaPlayer::PlayingState: return "Playing";
    case QMediaPlayer::PausedState: return "Paused";
    default: return "Stopped";
    }
}

// Runs once per visualizer frame: feed the analyzer the audio that has just
// reached the speakers, and publish the bar heights.
void Backend::tick() {
    m_lastTick.restart();
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
