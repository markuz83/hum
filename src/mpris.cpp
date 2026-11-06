#include "mpris.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QMimeDatabase>
#include <QStandardPaths>
#include <QUrl>

#include "backend.h"

static const char *ObjectPath = "/org/mpris/MediaPlayer2";

QStringList MprisRoot::supportedMimeTypes() const {
    return {"audio/mpeg", "audio/ogg", "audio/flac", "audio/x-wav", "audio/mp4", "audio/opus", "audio/x-vorbis+ogg"};
}

void MprisRoot::Quit() {
    QCoreApplication::quit();
}

MprisPlayer::MprisPlayer(Backend *backend, QObject *parent) : QDBusAbstractAdaptor(parent), m_backend(backend) {
    connect(backend, &Backend::seeked, this, [this](qint64 ms) { emit Seeked(ms * 1000); });
}

QString MprisPlayer::playbackStatus() const {
    return m_backend->playbackState();
}

QString MprisPlayer::loopStatus() const {
    return m_backend->repeat() == Backend::RepeatOne ? "Track" : m_backend->repeat() == Backend::RepeatAll ? "Playlist" : "None";
}

void MprisPlayer::setLoopStatus(const QString &status) {
    m_backend->setRepeat(status == "Track" ? Backend::RepeatOne : status == "Playlist" ? Backend::RepeatAll : Backend::RepeatOff);
}

bool MprisPlayer::shuffle() const {
    return m_backend->shuffle();
}

void MprisPlayer::setShuffle(bool shuffle) {
    m_backend->setShuffle(shuffle);
}

double MprisPlayer::volume() const {
    return m_backend->muted() ? 0.0 : m_backend->volume();
}

void MprisPlayer::setVolume(double volume) {
    m_backend->setVolume(volume);
}

// Position is read on demand and never announced, as the spec asks.
qlonglong MprisPlayer::position() const {
    return m_backend->position() * 1000;
}

bool MprisPlayer::canGoNext() const {
    return m_backend->canGoNext();
}

bool MprisPlayer::canPlay() const {
    return m_backend->canGoNext();
}

bool MprisPlayer::canSeek() const {
    return !m_backend->currentPath().isEmpty();
}

QDBusObjectPath MprisPlayer::trackId() const {
    const QString path = m_backend->currentPath();
    if (path.isEmpty())
        return QDBusObjectPath("/org/mpris/MediaPlayer2/TrackList/NoTrack");
    return QDBusObjectPath("/org/hum/track/" + QString::number(qHash(path)));
}

QVariantMap MprisPlayer::metadata() const {
    QVariantMap map;
    map["mpris:trackid"] = QVariant::fromValue(trackId());
    if (m_backend->currentPath().isEmpty())
        return map;
    map["mpris:length"] = qlonglong(m_backend->duration()) * 1000;
    map["xesam:url"] = QUrl::fromLocalFile(m_backend->currentPath()).toString();
    map["xesam:title"] = m_backend->title();
    if (!m_backend->artist().isEmpty())
        map["xesam:artist"] = QStringList{m_backend->artist()};
    if (!m_backend->album().isEmpty())
        map["xesam:album"] = m_backend->album();
    if (!m_artUrl.isEmpty())
        map["mpris:artUrl"] = m_artUrl;
    return map;
}

void MprisPlayer::Next() { m_backend->next(); }
void MprisPlayer::Previous() { m_backend->previous(); }
void MprisPlayer::Pause() { if (m_backend->playing()) m_backend->toggle(); }
void MprisPlayer::PlayPause() { m_backend->toggle(); }
void MprisPlayer::Stop() { m_backend->stop(); }
void MprisPlayer::Play() { if (!m_backend->playing()) m_backend->toggle(); }
void MprisPlayer::Seek(qlonglong offset) { m_backend->seekBy(offset / 1000); }

void MprisPlayer::SetPosition(const QDBusObjectPath &id, qlonglong position) {
    // A request for a track that is no longer playing is ignored, per the spec.
    if (id == trackId())
        m_backend->seek(position / 1000);
}

void MprisPlayer::OpenUri(const QString &uri) {
    const QUrl url(uri);
    if (url.isLocalFile())
        m_backend->open(url.toLocalFile());
}

Mpris::Mpris(Backend *backend, const QString &name, const QString &artDirectory, QObject *parent)
    : QObject(parent), m_backend(backend),
      m_artDirectory(artDirectory.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation) : artDirectory) {
    new MprisRoot(this);
    m_player = new MprisPlayer(backend, this);

    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;
    m_service = "org.mpris.MediaPlayer2." + name;
    // A second copy takes a numbered name, as the spec describes.
    bool named = bus.registerService(m_service);
    if (!named) {
        m_service += ".instance" + QString::number(QCoreApplication::applicationPid());
        named = bus.registerService(m_service);
    }
    m_registered = named && bus.registerObject(ObjectPath, this);

    // Changes arrive in bursts (a new track changes title, length, and cover
    // together), so announce them once, a moment later.
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(30);
    connect(&m_debounce, &QTimer::timeout, this, &Mpris::publish);
    for (auto signal : {&Backend::currentChanged, &Backend::playingChanged, &Backend::modeChanged,
                        &Backend::volumeChanged, &Backend::coverChanged, &Backend::durationChanged,
                        &Backend::browserChanged, &Backend::queueChanged})
        connect(backend, signal, this, &Mpris::schedule);
    m_last = snapshot();
    m_lastMetadata = metadataSignature();
}

void Mpris::schedule() {
    if (m_registered)
        m_debounce.start();
}

QVariantMap Mpris::snapshot() const {
    return {{"PlaybackStatus", m_player->playbackStatus()}, {"LoopStatus", m_player->loopStatus()},
            {"Shuffle", m_player->shuffle()}, {"Volume", m_player->volume()},
            {"CanGoNext", m_player->canGoNext()}, {"CanGoPrevious", m_player->canGoNext()},
            {"CanPlay", m_player->canPlay()}, {"CanPause", m_player->canPlay()}, {"CanSeek", m_player->canSeek()}};
}

QString Mpris::metadataSignature() const {
    return QStringList{m_backend->currentPath(), m_backend->title(), m_backend->artist(), m_backend->album(),
                       QString::number(m_backend->duration()), QString::number(m_backend->coverId())}.join('\n');
}

// Programs that want the art need a file or URL, so the current cover is
// written once per cover under the cache folder.
void Mpris::writeArt() {
    if (!m_artFile.isEmpty())
        QFile::remove(m_artFile);
    m_artFile.clear();
    if (m_backend->hasCover()) {
        QDir().mkpath(m_artDirectory);
        const QString file = m_artDirectory + "/cover-" + QString::number(m_backend->coverId()) + ".png";
        if (m_backend->cover().save(file))
            m_artFile = file;
    }
    m_player->setArtUrl(m_artFile.isEmpty() ? QString() : QUrl::fromLocalFile(m_artFile).toString());
}

void Mpris::publish() {
    QVariantMap changed;
    const QVariantMap now = snapshot();
    for (auto it = now.begin(); it != now.end(); ++it)
        if (m_last.value(it.key()) != it.value())
            changed.insert(it.key(), it.value());
    m_last = now;

    const QString signature = metadataSignature();
    if (signature != m_lastMetadata) {
        m_lastMetadata = signature;
        writeArt();
        changed.insert("Metadata", m_player->metadata());
    }
    if (changed.isEmpty())
        return;

    QDBusMessage message = QDBusMessage::createSignal(ObjectPath, "org.freedesktop.DBus.Properties", "PropertiesChanged");
    message << "org.mpris.MediaPlayer2.Player" << changed << QStringList();
    QDBusConnection::sessionBus().send(message);
}
