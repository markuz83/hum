#include "backend.h"

#include <QCollator>
#include <QDir>
#include <QDirIterator>
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

    connect(&m_player, &QMediaPlayer::positionChanged, this, &Backend::positionChanged);
    connect(&m_player, &QMediaPlayer::durationChanged, this, &Backend::durationChanged);
    connect(&m_player, &QMediaPlayer::metaDataChanged, this, &Backend::readMetadata);
    connect(&m_player, &QMediaPlayer::playbackStateChanged, this, [this] {
        emit playingChanged();
        if (playing() && !m_frames.isActive())
            m_frames.start();
    });
    connect(&m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia)
            advanceAfterEnd();
    });
    connect(&m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &message) {
        setError(tr("Can't play %1: %2").arg(m_title, message));
    });

    m_frames.setInterval(33);
    connect(&m_frames, &QTimer::timeout, this, &Backend::tick);
    connect(&m_watcher, &QFutureWatcher<QStringList>::finished, this, [this] { finishScan(m_watcher.result()); });
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
        folder = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
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

void Backend::finishScan(const QStringList &paths) {
    m_scanning = false;
    emit scanningChanged();

    const QString current = m_current >= 0 ? m_tracks[m_current].path : QString();
    const QDir root(m_library);
    m_tracks.clear();
    for (const auto &path : paths) {
        QString name = root.relativeFilePath(path);
        name.chop(QFileInfo(name).suffix().size() + 1);
        m_tracks.append({path, name});
    }
    m_current = -1;
    for (int i = 0; i < m_tracks.size(); ++i)
        if (m_tracks[i].path == current)
            m_current = i;
    applyFilter();

    if (m_tracks.isEmpty())
        setError(tr("No music found in %1").arg(m_library));
    if (!m_pending.isEmpty()) {
        for (int row = 0; row < m_visible.size(); ++row)
            if (m_tracks[m_visible[row]].path == m_pending)
                play(row);
        m_pending.clear();
    }
}

void Backend::applyFilter() {
    m_visible.clear();
    for (int i = 0; i < m_tracks.size(); ++i)
        if (m_filter.isEmpty() || m_tracks[i].name.contains(m_filter, Qt::CaseInsensitive))
            m_visible.append(i);
    emit tracksChanged();
    emit currentChanged();
}

void Backend::setFilter(const QString &filter) {
    if (filter == m_filter)
        return;
    m_filter = filter;
    applyFilter();
}

QStringList Backend::tracks() const {
    QStringList names;
    for (int index : m_visible)
        names << m_tracks[index].name;
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
    setError({});
    loadCurrent();
    m_player.play();
}

void Backend::loadCurrent() {
    m_buffers.clear();
    m_analyzer.clear();
    m_title = m_tracks[m_current].name.section('/', -1);
    m_artist.clear();
    m_player.setSource(QUrl::fromLocalFile(m_tracks[m_current].path));
    emit currentChanged();
}

void Backend::readMetadata() {
    if (m_current < 0)
        return;
    const auto data = m_player.metaData();
    const QString title = data.stringValue(QMediaMetaData::Title);
    QString artist = data.value(QMediaMetaData::ContributingArtist).toStringList().join(", ");
    if (artist.isEmpty())
        artist = data.stringValue(QMediaMetaData::AlbumArtist);
    if (artist.isEmpty())
        artist = data.value(QMediaMetaData::Author).toStringList().join(", ");
    m_title = title.isEmpty() ? m_tracks[m_current].name.section('/', -1) : title;
    m_artist = artist;
    emit currentChanged();
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
    m_player.setPosition(std::clamp<qint64>(ms, 0, std::max<qint64>(0, duration())));
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
    m_shuffle = !m_shuffle;
    emit modeChanged();
}

void Backend::cycleRepeat() {
    m_repeat = (m_repeat + 1) % 3;
    emit modeChanged();
}

// Runs about thirty times a second: feed the analyzer the audio that has just
// reached the speakers, and publish the bar heights.
void Backend::tick() {
    QVector<float> levels;
    if (playing()) {
        const qint64 now = m_player.position() * 1000;  // buffer times are in microseconds
        bool fed = false;
        while (!m_buffers.isEmpty() && m_buffers.head().startTime() <= now) {
            m_analyzer.push(m_buffers.dequeue());
            fed = true;
        }
        levels = fed ? m_analyzer.compute() : m_analyzer.decay();
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
