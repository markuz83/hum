#include "backend.h"

#include <QCollator>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QFileInfo>
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
            if (!m_frames.isActive())
                m_frames.start();
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

    m_frames.setInterval(33);
    connect(&m_frames, &QTimer::timeout, this, &Backend::tick);
    connect(&m_watcher, &QFutureWatcher<QStringList>::finished, this, [this] { finishScan(m_watcher.result()); });
    connect(&m_tagWatcher, &QFutureWatcher<TagResult>::finished, this, [this] { finishReading(m_tagWatcher.result()); });
    m_cacheFile = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/tags.json";
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
    m_resumePath = state.value("track").toString();
    m_resumePosition = qint64(state.value("position").toDouble());
    m_savedLibrary = state.value("library").toString();
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
                      {"visualizer", m_visualizerStyle}, {"track", currentPath()},
                      {"position", double(m_resumeAt >= 0 ? m_resumeAt : m_player.position())},
                      {"library", m_savedLibrary}};
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

void Backend::loadCurrent() {
    m_clockBase = 0;
    m_clock.restart();
    m_buffers.clear();
    m_analyzer.clear();
    m_title = m_tracks[m_current].title;
    m_artist = m_tracks[m_current].artist;
    m_album = m_tracks[m_current].album;
    updateCover({});
    m_player.setSource(QUrl::fromLocalFile(m_tracks[m_current].path));
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

// Embedded art wins; otherwise a cover.jpg, folder.png and the like beside the
// file. Art is kept at a size that stays sharp in the window without holding a
// huge scan in memory.
void Backend::updateCover(const QImage &embedded) {
    QImage image = embedded;
    if (image.isNull() && m_current >= 0) {
        const QDir folder(QFileInfo(m_tracks[m_current].path).absolutePath());
        const QStringList names = folder.entryList({"cover.*", "folder.*", "front.*", "album.*", "albumart*.*"}, QDir::Files);
        for (const auto &name : names) {
            const QString suffix = QFileInfo(name).suffix().toLower();
            if (suffix == "jpg" || suffix == "jpeg" || suffix == "png" || suffix == "webp") {
                image.load(folder.filePath(name));
                if (!image.isNull())
                    break;
            }
        }
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

void Backend::next() {
    if (m_visible.isEmpty())
        return;
    const int row = currentRow();
    int target = (row + 1) % int(m_visible.size());
    if (m_shuffle && m_visible.size() > 1) {
        do {
            target = int(QRandomGenerator::global()->bounded(m_visible.size()));
        } while (target == row);
    }
    play(target);
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
    } else if (m_repeat == RepeatAll || m_shuffle || currentRow() < m_visible.size() - 1) {
        next();
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

// Runs about thirty times a second: feed the analyzer the audio that has just
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
            m_frames.stop();
    }

    QVariantList list;
    list.reserve(levels.size());
    for (float level : levels)
        list << double(level);
    m_levelList = list;
    emit levelsChanged();
}
