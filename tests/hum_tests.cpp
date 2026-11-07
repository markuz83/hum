#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

#include "analyzer.h"
#include "backend.h"
#include "mpris.h"
#include "barlink.h"
#include <QLocalSocket>
#include "filepicker.h"
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include "theme.h"
#include "translator.h"

// Records PropertiesChanged signals from the MPRIS player.
class ChangeListener : public QObject {
    Q_OBJECT
public:
    QList<QVariantMap> changes;
public slots:
    void onChanged(const QString &, const QVariantMap &changed, const QStringList &) { changes << changed; }
};

// Answers the folder chooser without a desktop.
class FakePicker : public FilePicker {
    Q_OBJECT
public:
    void pickFolder(const QString &) override { ++asked; if (!answer.isEmpty()) emit picked(answer); }
    QString answer;
    int asked = 0;
};

class HumTests : public QObject {
    Q_OBJECT

private slots:
    void scanFindsAudioRecursivelyAndInOrder()
    {
        QTemporaryDir dir;
        touch(dir.path() + "/10 - ten.mp3");
        touch(dir.path() + "/2 - two.ogg");
        touch(dir.path() + "/Album/a.FLAC");
        touch(dir.path() + "/cover.jpg");
        touch(dir.path() + "/notes.txt");

        const QStringList found = Backend::scan(dir.path());
        QStringList names;
        for (const auto &path : found)
            names << path.mid(dir.path().size() + 1);
        QCOMPARE(names, QStringList({"2 - two.ogg", "10 - ten.mp3", "Album/a.FLAC"}));
    }

    void filterNarrowsTheList()
    {
        QTemporaryDir dir;
        Backend backend;
        ready(backend, dir, {"Bowie/Heroes.mp3", "Bowie/Low.mp3", "Eno/Ambient.mp3"});
        QCOMPARE(backend.titles().size(), 3);
        backend.setFilter("bowie");
        QCOMPARE(backend.titles(), QStringList({"Heroes", "Low"}));
        backend.setFilter("");
        QCOMPARE(backend.titles().size(), 3);
    }

    void nextAndPreviousWrap()
    {
        QTemporaryDir dir;
        Backend backend;
        ready(backend, dir, {"a.mp3", "b.mp3", "c.mp3"});
        backend.play(1);
        QCOMPARE(backend.currentRow(), 1);
        backend.next();
        QCOMPARE(backend.currentRow(), 2);
        backend.next();
        QCOMPARE(backend.currentRow(), 0);
        backend.previous();
        QCOMPARE(backend.currentRow(), 2);
    }

    void shuffledPreviousRetracesWhatPlayed()
    {
        QTemporaryDir dir;
        Backend backend;
        ready(backend, dir, {"a.mp3", "b.mp3", "c.mp3", "d.mp3", "e.mp3", "f.mp3"});
        backend.setShuffle(true);
        backend.play(0);
        QStringList played{backend.currentPath()};
        for (int i = 0; i < 4; ++i) {
            backend.next();
            played << backend.currentPath();
        }
        // Going back follows the shuffled order in reverse, not the list's.
        for (int i = 3; i >= 0; --i) {
            backend.previous();
            QCOMPARE(backend.currentPath(), played[i]);
        }
    }

    void sleepTimerStepsThroughItsSettings()
    {
        Backend backend;
        QCOMPARE(backend.sleepSetting(), 0);
        QList<int> seen;
        for (int i = 0; i < 8; ++i) {
            backend.cycleSleep();
            seen << backend.sleepSetting();
        }
        QCOMPARE(seen, QList<int>({15, 30, 45, 60, 90, -1, 0, 15}));
        backend.setSleep(30);
        QVERIFY(backend.sleepRemaining() > 29 * 60 && backend.sleepRemaining() <= 30 * 60);
        backend.setSleep(0);
        QCOMPARE(backend.sleepRemaining(), 0);
    }

    void crossfadeStepsAndIsKeptInRange()
    {
        Backend backend;
        QList<int> seen;
        for (int i = 0; i < 8; ++i) {
            backend.cycleCrossfade();
            seen << backend.crossfade();
        }
        QCOMPARE(seen, QList<int>({2, 4, 6, 8, 10, 12, 0, 2}));
        backend.setCrossfade(99);
        QCOMPARE(backend.crossfade(), 12);
    }

    void replayGainLevelsTracksAndFallsBack()
    {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty() || QStandardPaths::findExecutable("ffprobe").isEmpty())
            QSKIP("ffmpeg and ffprobe are needed to make tagged files");
        QTemporaryDir dir;
        auto gained = [&](const QString &name, const QStringList &extra) {
            QProcess ffmpeg;
            ffmpeg.start("ffmpeg", QStringList{"-v", "error", "-f", "lavfi", "-i", "anullsrc=r=22050:cl=mono", "-t", "1"}
                                       + extra + QStringList{"-y", dir.path() + "/" + name});
            QVERIFY(ffmpeg.waitForFinished(15000));
        };
        gained("a.flac", {"-metadata", "REPLAYGAIN_TRACK_GAIN=-6.00 dB", "-metadata", "REPLAYGAIN_ALBUM_GAIN=-2.00 dB"});
        gained("b.flac", {"-metadata", "REPLAYGAIN_ALBUM_GAIN=-4.00 dB"});
        gained("c.flac", {"-metadata", "title=Plain"});
        Backend backend;
        ready(backend, dir, {});
        const QString a = dir.path() + "/a.flac", b = dir.path() + "/b.flac", c = dir.path() + "/c.flac";
        QCOMPARE(backend.leveling(a), 1.0);  // off by default
        backend.setReplayGain(1);
        QVERIFY(qAbs(backend.leveling(a) - std::pow(10.0, -6.0 / 20)) < 1e-9);
        QVERIFY(qAbs(backend.leveling(b) - std::pow(10.0, -4.0 / 20)) < 1e-9);  // no track gain: uses the album's
        QCOMPARE(backend.leveling(c), 1.0);  // no gain at all
        backend.setReplayGain(2);
        QVERIFY(qAbs(backend.leveling(a) - std::pow(10.0, -2.0 / 20)) < 1e-9);
    }

    void sortModesReorderTheTitleList()
    {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty() || QStandardPaths::findExecutable("ffprobe").isEmpty())
            QSKIP("ffmpeg and ffprobe are needed to make tagged files");
        QTemporaryDir dir;
        auto tone = [&](const QString &name, const QString &title, int track, const QString &year, int seconds) {
            QProcess ffmpeg;
            ffmpeg.start("ffmpeg", {"-v", "error", "-f", "lavfi", "-i", "anullsrc=r=22050:cl=mono", "-t", QString::number(seconds),
                                    "-metadata", "title=" + title, "-metadata", "artist=A", "-metadata", "album=X",
                                    "-metadata", "track=" + QString::number(track), "-metadata", "date=" + year,
                                    "-y", dir.path() + "/" + name});
            QVERIFY(ffmpeg.waitForFinished(15000));
        };
        tone("1.flac", "Cee", 1, "2001", 3);
        tone("2.flac", "Bee", 2, "1999", 1);
        tone("3.flac", "Aye", 3, "", 2);
        // Modified times, oldest first: 3, 1, 2.
        auto age = [&](const QString &name, qint64 secs) {
            QFile file(dir.path() + "/" + name);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(QDateTime::fromSecsSinceEpoch(secs), QFileDevice::FileModificationTime));
        };
        age("3.flac", 1000000000);
        age("1.flac", 1100000000);
        age("2.flac", 1200000000);
        Backend backend;
        ready(backend, dir, {});
        auto names = [&] { QStringList out; for (const QString &t : backend.titles()) out << t.section("  ", -1); return out; };
        QCOMPARE(names(), QStringList({"Cee", "Bee", "Aye"}));            // album order
        backend.setSortMode(1);
        QCOMPARE(names(), QStringList({"Aye", "Bee", "Cee"}));            // title
        backend.setSortMode(2);
        QCOMPARE(names(), QStringList({"Bee", "Cee", "Aye"}));            // year; no year last
        backend.setSortMode(3);
        QCOMPARE(names(), QStringList({"Bee", "Cee", "Aye"}));            // newest file first
        backend.setSortMode(4);
        QCOMPARE(names(), QStringList({"Bee", "Aye", "Cee"}));            // shortest first
        QVERIFY(!backend.titles().first().contains("  "));                // no track numbers outside album order
    }

    void lyricsParseSyncedAndPlainText()
    {
        const auto synced = Backend::parseLyrics("[ar:Someone]\r\n[00:12.50]Second\r\n[00:01.5][00:30]Chorus\n[ti:Song]\n");
        QCOMPARE(synced.lines, QStringList({"Chorus", "Second", "Chorus"}));
        QCOMPARE(synced.times, QList<qint64>({1500, 12500, 30000}));
        const auto plain = Backend::parseLyrics("\n[Verse 1]\nLine one\n\nLine two\n\n");
        QCOMPARE(plain.lines, QStringList({"[Verse 1]", "Line one", "", "Line two"}));
        QVERIFY(plain.times.isEmpty());
    }

    void lyricsComeFromALrcFileOrTheTags()
    {
        QTemporaryDir dir;
        touch(dir.path() + "/song.mp3");
        write(dir.path() + "/song.LRC", "[00:05.00]Hello\n[00:09.00]World\n");
        auto lyrics = Backend::readLyrics(dir.path() + "/song.mp3", {});
        QCOMPARE(lyrics.lines, QStringList({"Hello", "World"}));
        QCOMPARE(lyrics.times.size(), 2);

        const QString ffprobe = QStandardPaths::findExecutable("ffprobe");
        if (ffprobe.isEmpty() || QStandardPaths::findExecutable("ffmpeg").isEmpty())
            return;
        QProcess ffmpeg;
        ffmpeg.start("ffmpeg", {"-v", "error", "-f", "lavfi", "-i", "anullsrc=r=22050:cl=mono", "-t", "1",
                                "-metadata", "lyrics=First line\nSecond line", "-y", dir.path() + "/tagged.flac"});
        QVERIFY(ffmpeg.waitForFinished(15000));
        lyrics = Backend::readLyrics(dir.path() + "/tagged.flac", ffprobe);
        QCOMPARE(lyrics.lines, QStringList({"First line", "Second line"}));
        QVERIFY(lyrics.times.isEmpty());
        QVERIFY(Backend::readLyrics(dir.path() + "/missing.flac", ffprobe).lines.isEmpty());
    }

    void lyricLineFollowsThePosition()
    {
        QTemporaryDir dir;
        touch(dir.path() + "/a.mp3");
        write(dir.path() + "/a.lrc", "[00:02.00]one\n[00:04.00]two\n");
        Backend backend;
        ready(backend, dir, {});
        backend.setShowLyrics(true);
        backend.play(0);
        QTRY_COMPARE(backend.lyricLines(), QStringList({"one", "two"}));
        QCOMPARE(backend.lyricIndexAt(500), -1);
        QCOMPARE(backend.lyricIndexAt(2000), 0);
        QCOMPARE(backend.lyricIndexAt(9000), 1);
    }

    void libraryFollowsItsFolderAndKeepsTheSelection()
    {
        QTemporaryDir dir;
        QDir().mkpath(dir.path() + "/Band/Album");
        touch(dir.path() + "/Band/Album/one.mp3");
        touch(dir.path() + "/Other/two.mp3");
        QDir().mkpath(dir.path() + "/Other");
        touch(dir.path() + "/Other/two.mp3");
        Backend backend;
        backend.setCacheFile(dir.path() + "/.cache/tags.json");
        backend.open(dir.path());
        QTRY_VERIFY(!backend.scanning() && !backend.reading() && backend.libraryCount() == 2);
        backend.selectArtist(1);
        const int row = backend.artistRow();
        // A file appears in a folder already known, then one in a brand new folder.
        touch(dir.path() + "/Band/Album/three.mp3");
        QTRY_COMPARE_WITH_TIMEOUT(backend.libraryCount(), 3, 8000);
        QCOMPARE(backend.artistRow(), row);
        QDir().mkpath(dir.path() + "/Fresh");
        touch(dir.path() + "/Fresh/four.mp3");
        QTRY_COMPARE_WITH_TIMEOUT(backend.libraryCount(), 4, 8000);
        QFile::remove(dir.path() + "/Other/two.mp3");
        QTRY_COMPARE_WITH_TIMEOUT(backend.libraryCount(), 3, 8000);
        QCOMPARE(backend.artistRow(), row);
    }

    void currentTrackSurvivesFiltering()
    {
        QTemporaryDir dir;
        Backend backend;
        ready(backend, dir, {"a.mp3", "b.mp3", "c.mp3"});
        backend.play(2);
        backend.setFilter("c");
        QCOMPARE(backend.currentRow(), 0);
        backend.setFilter("a");
        QCOMPARE(backend.currentRow(), -1);
    }

    void probeOutputBecomesTags()
    {
        const auto tags = Backend::parseProbe(R"({"format": {"tags": {"TITLE": "Heroes", "ARTIST": "David Bowie",
            "album": "Heroes", "track": "3/10", "disc": "1/2"}}})");
        QCOMPARE(tags.title, QString("Heroes"));
        QCOMPARE(tags.artist, QString("David Bowie"));
        QCOMPARE(tags.album, QString("Heroes"));
        QCOMPARE(tags.track, 3);
        QCOMPARE(tags.disc, 1);
        // A compilation's album artist wins over the track's artist.
        QCOMPARE(Backend::parseProbe(R"({"format": {"tags": {"artist": "A", "album_artist": "Various"}}})").artist,
                 QString("Various"));
        QVERIFY(Backend::parseProbe("not json").title.isEmpty());
    }

    void probeReadsTagsKeptOnTheStreamAndReplayGain()
    {
        // Opus and Ogg put their tags on the audio stream, not the file.
        auto tags = Backend::parseProbe(R"({"streams": [{"codec_type": "audio", "tags": {"TITLE": "Song", "ARTIST": "Band",
            "R128_TRACK_GAIN": "-512", "REPLAYGAIN_ALBUM_GAIN": "-3.25 dB"}}], "format": {"tags": {}}})");
        QCOMPARE(tags.title, QString("Song"));
        QCOMPARE(tags.artist, QString("Band"));
        QCOMPARE(tags.trackGain, 3.0);  // -512/256 + 5: Opus is -23 LUFS, ReplayGain -18
        QCOMPARE(tags.albumGain, -3.25);
        // A cover picture's tags are not the music's.
        tags = Backend::parseProbe(R"({"streams": [{"codec_type": "video", "tags": {"title": "cover"}}], "format": {"tags": {}}})");
        QVERIFY(tags.title.isEmpty());
        QVERIFY(qIsNaN(tags.trackGain));
        // Where both exist, the file's tags win and the year is the first four digits.
        tags = Backend::parseProbe(R"({"streams": [{"codec_type": "audio", "tags": {"title": "Old"}}],
            "format": {"tags": {"title": "New", "date": "1977-10-14"}}})");
        QCOMPARE(tags.title, QString("New"));
        QCOMPARE(tags.year, 1977);
    }

    void probeReadsTheLength()
    {
        QCOMPARE(Backend::parseProbe(R"({"format": {"duration": "273.720000", "tags": {}}})").seconds, 274);
        QCOMPARE(Backend::parseProbe(R"({"format": {"tags": {}}})").seconds, 0);
    }

    void foldersStandInForMissingTags()
    {
        auto tags = Backend::fallbackTags("Bowie/Low/Warszawa");
        QCOMPARE(tags.artist, QString("Bowie"));
        QCOMPARE(tags.album, QString("Low"));
        QCOMPARE(tags.title, QString("Warszawa"));
        tags = Backend::fallbackTags("Loose song");
        QVERIFY(tags.artist.isEmpty());
        QCOMPARE(tags.title, QString("Loose song"));
    }

    void columnsNarrowAsYouChoose()
    {
        QTemporaryDir dir;
        Backend backend;
        ready(backend, dir, {"Bowie/Low/b.mp3", "Bowie/Low/a.mp3", "Bowie/Heroes/c.mp3", "Eno/Apollo/d.mp3"});
        QCOMPARE(backend.artists(), QStringList({"Bowie", "Eno"}));
        QCOMPARE(backend.albums(), QStringList({"Apollo", "Heroes", "Low"}));
        QCOMPARE(backend.titles().size(), 4);

        backend.selectArtist(1);  // row 0 is "All", so 1 is Bowie
        QCOMPARE(backend.albums(), QStringList({"Heroes", "Low"}));
        QCOMPARE(backend.titles(), QStringList({"c", "a", "b"}));

        backend.selectAlbum(2);
        QCOMPARE(backend.titles(), QStringList({"a", "b"}));

        backend.selectArtist(2);  // a new artist starts at All albums
        QCOMPARE(backend.albumRow(), 0);
        QCOMPARE(backend.titles(), QStringList({"d"}));
        backend.selectArtist(0);
        QCOMPARE(backend.titles().size(), 4);
    }

    void filterKeepsTheSelectionWhileItStillExists()
    {
        QTemporaryDir dir;
        Backend backend;
        ready(backend, dir, {"Bowie/Low/a.mp3", "Eno/Apollo/d.mp3"});
        backend.selectArtist(2);
        backend.setFilter("apollo");
        QCOMPARE(backend.artistRow(), 1);
        backend.setFilter("low");  // Eno is gone, so the choice falls back to All
        QCOMPARE(backend.artistRow(), 0);
    }

    void tagsFromRealFilesGroupTheLibrary()
    {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty() || QStandardPaths::findExecutable("ffprobe").isEmpty())
            QSKIP("ffmpeg and ffprobe are needed to make tagged files");
        QTemporaryDir dir;
        // Names that disagree with the tags, so only the tags can be right.
        make(dir.path() + "/x1.mp3", "Second", "Zed", "Z Album", 2);
        make(dir.path() + "/x2.mp3", "First", "Zed", "Z Album", 1);
        make(dir.path() + "/x3.mp3", "Other", "Abe", "A Album", 1);

        Backend backend;
        ready(backend, dir, {});
        QCOMPARE(backend.artists(), QStringList({"Abe", "Zed"}));
        QCOMPARE(backend.titles(), QStringList({"01  Other", "01  First", "02  Second"}));
        // Each test file is a short tone, so every title has a length.
        for (const QVariant &seconds : backend.titleSeconds())
            QVERIFY(seconds.toInt() > 0);

        // A second run reads the cache and gets the same answer.
        QVERIFY(QFileInfo::exists(dir.path() + "/.cache/tags.json"));
        const auto again = Backend::readTags(Backend::scan(dir.path()), dir.path() + "/.cache/tags.json");
        QCOMPARE(again.tags.size(), 3);
    }

    void mprisExposesAndControlsThePlayer()
    {
        if (!QDBusConnection::sessionBus().isConnected())
            QSKIP("needs a D-Bus session bus");
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty() || QStandardPaths::findExecutable("ffprobe").isEmpty())
            QSKIP("ffmpeg and ffprobe are needed to make tagged files");
        QTemporaryDir dir;
        make(dir.path() + "/1.mp3", "First", "Zed", "Z Album", 1);
        make(dir.path() + "/2.mp3", "Second", "Zed", "Z Album", 2);
        Backend backend;
        backend.setVolume(0);
        ready(backend, dir, {});

        const QString name = "humtest" + QString::number(QCoreApplication::applicationPid());
        Mpris mpris(&backend, name, dir.path() + "/art");
        QVERIFY(mpris.registered());

        ChangeListener listener;
        QDBusConnection::sessionBus().connect(mpris.serviceName(), "/org/mpris/MediaPlayer2",
            "org.freedesktop.DBus.Properties", "PropertiesChanged", &listener, SLOT(onChanged(QString,QVariantMap,QStringList)));

        QDBusInterface root(mpris.serviceName(), "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2");
        QDBusInterface player(mpris.serviceName(), "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player");
        QVERIFY(root.isValid() && player.isValid());
        QCOMPARE(root.property("Identity").toString(), QString("Hum"));
        QCOMPARE(root.property("DesktopEntry").toString(), QString("hum"));
        QCOMPARE(player.property("PlaybackStatus").toString(), QString("Stopped"));
        QCOMPARE(player.property("CanControl").toBool(), true);

        // Writable properties go through to the app.
        QVERIFY(player.setProperty("LoopStatus", "Track"));
        QCOMPARE(backend.repeat(), int(Backend::RepeatOne));
        QCOMPARE(player.property("LoopStatus").toString(), QString("Track"));
        QVERIFY(player.setProperty("Shuffle", true));
        QVERIFY(backend.shuffle());
        player.setProperty("Shuffle", false);

        // Playing announces the new state and track.
        player.call("PlayPause");
        QTRY_COMPARE(player.property("PlaybackStatus").toString(), QString("Playing"));
        QCOMPARE(backend.currentRow(), 0);
        QTRY_VERIFY(std::any_of(listener.changes.begin(), listener.changes.end(),
                                [](const QVariantMap &m) { return m.value("PlaybackStatus") == "Playing" && m.contains("Metadata"); }));

        const QVariant raw = player.property("Metadata");
        const QVariantMap metadata = raw.metaType() == QMetaType::fromType<QDBusArgument>()
            ? qdbus_cast<QVariantMap>(raw.value<QDBusArgument>()) : raw.toMap();
        QCOMPARE(metadata.value("xesam:title").toString(), QString("First"));
        QCOMPARE(metadata.value("xesam:album").toString(), QString("Z Album"));
        QCOMPARE(metadata.value("xesam:artist").toStringList(), QStringList({"Zed"}));
        QVERIFY(metadata.value("mpris:trackid").value<QDBusObjectPath>().path().startsWith("/org/hum/track/"));

        player.setProperty("LoopStatus", "None");
        player.call("Next");
        QTRY_COMPARE(backend.currentRow(), 1);
        QTRY_COMPARE(player.property("PlaybackStatus").toString(), QString("Playing"));
        player.call("Pause");
        QTRY_COMPARE(player.property("PlaybackStatus").toString(), QString("Paused"));
        player.call("Stop");
        QTRY_COMPARE(player.property("PlaybackStatus").toString(), QString("Stopped"));
    }

    void settingsAndTrackComeBackNextTime()
    {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty() || QStandardPaths::findExecutable("ffprobe").isEmpty())
            QSKIP("ffmpeg and ffprobe are needed to make tagged files");
        QTemporaryDir dir;
        const QString state = dir.path() + "/config/state.json";
        make(dir.path() + "/1.mp3", "First", "Zed", "Z Album", 1);
        make(dir.path() + "/2.mp3", "Second", "Zed", "Z Album", 2);

        {
            Backend backend;
            backend.setStateFile(state);
            backend.setVolume(0);  // silent for the test; saved as 0 below
            ready(backend, dir, {});
            backend.setVolume(0.3);
            backend.setShuffle(true);
            backend.setRepeat(Backend::RepeatAll);
            backend.setVisualizerStyle(2);
            backend.play(1);
            QTRY_VERIFY(backend.duration() > 1000);
            backend.seek(2000);
            backend.toggle();  // pause, so nothing plays aloud
            backend.saveState();
        }
        QVERIFY(QFileInfo::exists(state));

        Backend again;
        again.setStateFile(state);
        QCOMPARE(again.volume(), 0.3);
        QVERIFY(again.shuffle());
        QCOMPARE(again.repeat(), int(Backend::RepeatAll));
        QCOMPARE(again.visualizerStyle(), 2);
        again.setVolume(0);
        ready(again, dir, {});
        // The same track is waiting, paused, near where it was left.
        QCOMPARE(again.currentRow(), 1);
        QVERIFY(!again.playing());
        QTRY_VERIFY(qAbs(again.position() - 2000) < 400);
    }

    void aSavedTrackThatIsGoneIsQuietlyForgotten()
    {
        QTemporaryDir dir;
        const QString state = dir.path() + "/state.json";
        write(state, QByteArray("{\"track\": \"") + dir.path().toUtf8() + "/deleted.mp3\", \"position\": 5000, \"library\": \""
                         + dir.path().toUtf8() + "\", \"volume\": 0, \"queue\": [\"" + dir.path().toUtf8() + "/deleted.mp3\"]}");
        Backend backend;
        backend.setStateFile(state);
        ready(backend, dir, {"a.mp3", "b.mp3"});
        // Nothing is resumed and nothing is reported: the library just opens.
        QCOMPARE(backend.currentRow(), -1);
        QVERIFY(!backend.playing());
        QVERIFY(backend.error().isEmpty());
        QCOMPARE(backend.queue().size(), 0);  // a queued file that is gone leaves the queue too
        backend.saveState();
        QFile saved(state);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QVERIFY(!QString::fromUtf8(saved.readAll()).contains("deleted.mp3"));
    }

    void aCommandLineFileBeatsTheSavedTrack()
    {
        QTemporaryDir dir;
        const QString state = dir.path() + "/state.json";
        write(state, QByteArray("{\"track\": \"") + dir.path().toUtf8() + "/a.mp3\", \"position\": 5000}");
        touch(dir.path() + "/a.mp3");
        touch(dir.path() + "/b.mp3");
        Backend backend;
        backend.setStateFile(state);
        backend.setVolume(0);
        backend.setCacheFile(dir.path() + "/tags.json");
        QSignalSpy scanned(&backend, &Backend::scanningChanged);
        backend.open(dir.path() + "/b.mp3");
        while (backend.scanning())
            scanned.wait(2000);
        QCOMPARE(backend.title(), QString("b"));
    }

    void chosenFolderBecomesTheLibraryAndIsRemembered()
    {
        QTemporaryDir home, music;
        touch(music.path() + "/Artist/Album/song.mp3");
        const QString state = home.path() + "/state.json";
        FakePicker picker;
        {
            Backend backend;
            backend.setStateFile(state);
            backend.setCacheFile(home.path() + "/tags.json");
            backend.setFilePicker(&picker);
            // Cancelling changes nothing.
            backend.chooseLibrary();
            QCOMPARE(picker.asked, 1);
            QVERIFY(backend.libraryPath().isEmpty());

            picker.answer = music.path();
            QSignalSpy scanned(&backend, &Backend::scanningChanged);
            backend.chooseLibrary();
            while (backend.scanning() || scanned.isEmpty())
                scanned.wait(2000);
            QCOMPARE(backend.libraryPath(), music.path());
            backend.saveState();
        }
        // The next launch, with no folder given, opens the chosen one.
        Backend again;
        again.setStateFile(state);
        again.setCacheFile(home.path() + "/tags.json");
        QSignalSpy scanned(&again, &Backend::scanningChanged);
        again.open();
        while (again.scanning() || scanned.isEmpty())
            scanned.wait(2000);
        QCOMPARE(again.libraryPath(), music.path());
    }

    void withoutAStateFileNothingIsWritten()
    {
        QTemporaryDir dir;
        Backend backend;
        backend.setVolume(0.5);
        backend.saveState();
        QVERIFY(QDir(dir.path()).isEmpty());
    }

    void barLinkEncodesBarsAndTracks()
    {
        QVariantList levels;
        for (int i = 0; i < 48; ++i)
            levels << (i < 24 ? 1.0 : 0.0);
        // 48 bands pool down to 4 bars: two full, two empty.
        QCOMPARE(BarLink::encodeLevels(levels, 4), QString("L 99 99 0 0"));
        QCOMPARE(BarLink::encodeLevels({}, 4), QString("L"));
        // A lone peak survives pooling instead of being averaged away.
        QVariantList spike;
        for (int i = 0; i < 48; ++i)
            spike << (i == 5 ? 0.8 : 0.0);
        QCOMPARE(BarLink::encodeLevels(spike, 4), QString("L 79 0 0 0"));
        // Tabs and newlines would break the framing, so they become spaces.
        QCOMPARE(BarLink::encodeTrack("Two\nLines\tHere", "Some Artist"), QString("N Two Lines Here\tSome Artist"));
        QCOMPARE(BarLink::encodeTrack(QString(500, 'x'), "").section('\t', 0, 0).size(), 2 + 200);
    }

    void barLinkGreetsStreamsAndObeys()
    {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty() || QStandardPaths::findExecutable("ffprobe").isEmpty())
            QSKIP("ffmpeg and ffprobe are needed to make tagged files");
        QTemporaryDir dir;
        make(dir.path() + "/1.mp3", "First", "Zed", "Z Album", 1);
        make(dir.path() + "/2.mp3", "Second", "Zed", "Z Album", 2);
        Backend backend;
        backend.setVolume(0.5);
        ready(backend, dir, {});

        BarLink link(&backend, dir.path() + "/bar.sock");
        QVERIFY(link.listening());

        // Client and server share this thread, so lines are gathered as the
        // event loop delivers them rather than by blocking reads.
        QLocalSocket client;
        QStringList lines;
        QObject::connect(&client, &QLocalSocket::readyRead, [&] {
            while (client.canReadLine())
                lines << QString::fromUtf8(client.readLine()).trimmed();
        });
        client.connectToServer(link.path());
        // The greeting says what is playing before anything has happened.
        QTRY_VERIFY(lines.size() >= 2);
        QCOMPARE(lines[0], QString("S Stopped"));
        QCOMPARE(lines[1], QString("N"));  // no track yet; trimming drops the trailing tab

        // A command from the bar starts playback, and the stream follows.
        client.write("playpause\n");
        QTRY_VERIFY(lines.contains("S Playing"));
        QTRY_VERIFY(std::any_of(lines.begin(), lines.end(), [](const QString &l) { return l.startsWith("L "); }));
        QTRY_VERIFY(lines.contains("N First\tZed"));
        for (const auto &line : lines)
            if (line.startsWith("L "))
                QCOMPARE(line.split(' ').size(), 25);  // "L" plus 24 bars
        QCOMPARE(backend.currentRow(), 0);

        // Volume moves in percent, and unknown lines are ignored.
        client.write("volume -20\nnonsense\nnext\n");
        QTRY_COMPARE(backend.currentRow(), 1);
        QTRY_COMPARE(backend.volume(), 0.3);
        client.write("playpause\n");
        QTRY_COMPARE(backend.playbackState(), QString("Paused"));
    }

    void queueKeepsOrderAndPlaysBeforeTheLibrary()
    {
        QTemporaryDir dir;
        Backend backend;
        backend.setVolume(0);
        ready(backend, dir, {"a.mp3", "b.mp3", "c.mp3", "d.mp3"});
        auto titles = [&] {
            QStringList out;
            for (const auto &item : backend.queue())
                out << item.toMap().value("title").toString();
            return out;
        };

        backend.enqueue(2);                       // c to the end
        backend.enqueue(3);                       // then d
        backend.enqueue(1, true);                 // b to the front
        QCOMPARE(titles(), QStringList({"b", "c", "d"}));

        // Playing through the queue takes entries off the front, ahead of the list.
        backend.play(0);                          // a
        backend.next();
        QCOMPARE(backend.title(), QString("b"));
        QCOMPARE(titles(), QStringList({"c", "d"}));
        backend.next();
        backend.next();
        QCOMPARE(backend.title(), QString("d"));
        QVERIFY(backend.queue().isEmpty());
        // With the queue empty the library carries on after the track that played.
        backend.next();
        QCOMPARE(backend.currentRow(), 0);        // d was last, so it wraps to a
    }

    void libraryContinuesAfterAQueuedTrackNotFromTheTop()
    {
        QTemporaryDir dir;
        Backend backend;
        backend.setVolume(0);
        ready(backend, dir, {"a.mp3", "b.mp3", "c.mp3", "d.mp3"});
        backend.selectArtist(0);
        backend.enqueue(1);                       // b
        backend.play(2);                          // c is playing
        backend.next();                           // queue first: b
        QCOMPARE(backend.title(), QString("b"));
        backend.setFilter("");                    // b is visible, so the list carries on after it
        backend.next();
        QCOMPARE(backend.title(), QString("c"));
    }

    void queueGroupsAndWholeListsAreAdded()
    {
        QTemporaryDir dir;
        Backend backend;
        ready(backend, dir, {"Bowie/Low/a.mp3", "Bowie/Low/b.mp3", "Bowie/Heroes/c.mp3", "Eno/Apollo/d.mp3"});
        backend.enqueueGroup(0, 2);               // artist row 2 is Eno
        QCOMPARE(backend.queue().size(), 1);
        backend.clearQueue();
        backend.selectArtist(1);                  // Bowie
        backend.enqueueGroup(1, 0);               // all of Bowie's albums
        QCOMPARE(backend.queue().size(), 3);
        backend.clearQueue();
        backend.enqueueGroup(1, 2);               // album row 2 is Low
        QCOMPARE(backend.queue().size(), 2);
        backend.clearQueue();
        backend.selectArtist(0);
        backend.enqueueVisible();
        QCOMPARE(backend.queue().size(), 4);
        backend.enqueueVisible(true);             // play-next puts a block in front, in order
        QCOMPARE(backend.queue().size(), 8);
    }

    void queueCanBeReorderedRemovedAndUndone()
    {
        QTemporaryDir dir;
        Backend backend;
        ready(backend, dir, {"a.mp3", "b.mp3", "c.mp3"});
        auto titles = [&] {
            QStringList out;
            for (const auto &item : backend.queue())
                out << item.toMap().value("title").toString();
            return out;
        };
        for (int i = 0; i < 3; ++i)
            backend.enqueue(i);
        backend.moveInQueue(0, 2);
        QCOMPARE(titles(), QStringList({"b", "c", "a"}));
        backend.moveInQueue(5, 0);                // out of range: ignored
        QCOMPARE(titles(), QStringList({"b", "c", "a"}));
        backend.removeFromQueue(1);
        QCOMPARE(titles(), QStringList({"b", "a"}));
        backend.clearQueue();
        QVERIFY(titles().isEmpty());

        backend.undoQueue();                      // un-clear
        QCOMPARE(titles(), QStringList({"b", "a"}));
        backend.undoQueue();                      // un-remove
        QCOMPARE(titles(), QStringList({"b", "c", "a"}));
        backend.redoQueue();
        QCOMPARE(titles(), QStringList({"b", "a"}));
        // A new edit starts history over, so redo no longer applies.
        backend.enqueue(0);
        backend.redoQueue();
        QCOMPARE(titles(), QStringList({"b", "a", "a"}));
    }

    void playFromQueueTakesThatEntryOut()
    {
        QTemporaryDir dir;
        Backend backend;
        backend.setVolume(0);
        ready(backend, dir, {"a.mp3", "b.mp3", "c.mp3"});
        backend.enqueue(0);
        backend.enqueue(1);
        backend.enqueue(2);
        backend.playFromQueue(1);
        QCOMPARE(backend.title(), QString("b"));
        QCOMPARE(backend.queue().size(), 2);
        backend.undoQueue();                      // playing wasn't an edit, so nothing to undo
        QCOMPARE(backend.queue().size(), 2);
    }

    void queueAndPaneSizeAreRemembered()
    {
        QTemporaryDir dir;
        const QString state = dir.path() + "/state.json";
        touch(dir.path() + "/lib/a.mp3");
        touch(dir.path() + "/lib/b.mp3");
        const QString lib = dir.path() + "/lib";
        {
            Backend backend;
            backend.setStateFile(state);
            backend.setCacheFile(dir.path() + "/tags.json");
            QSignalSpy scanned(&backend, &Backend::scanningChanged);
            backend.open(lib);
            while (backend.scanning() || scanned.isEmpty())
                scanned.wait(2000);
            while (backend.reading())
                QTest::qWait(50);
            backend.enqueue(1);
            backend.setQueueRatio(0.45);
            backend.saveState();
        }
        // An entry for a file that has since vanished is dropped, the rest stay.
        write(state, QByteArray("{\"queueRatio\": 0.45, \"queue\": [\"") + lib.toUtf8() + "/b.mp3\", \"/gone/x.mp3\"]}");
        Backend again;
        again.setStateFile(state);
        again.setCacheFile(dir.path() + "/tags.json");
        QCOMPARE(again.queueRatio(), 0.45);
        // Nothing can be shown for the saved queue until the library has been
        // read, so the queue announces itself again once it has.
        QVERIFY(again.queue().isEmpty());
        QSignalSpy queueSpy(&again, &Backend::queueChanged);
        QSignalSpy scanned(&again, &Backend::scanningChanged);
        again.open(lib);
        while (again.scanning() || scanned.isEmpty())
            scanned.wait(2000);
        QVERIFY(!queueSpy.isEmpty());
        QCOMPARE(again.queue().size(), 1);
        QCOMPARE(again.queue().first().toMap().value("title").toString(), QString("b"));
    }

    void queueRatioStaysInRange()
    {
        Backend backend;
        QCOMPARE(backend.queueRatio(), 1.0 / 3.0);
        backend.setQueueRatio(0.99);
        QCOMPARE(backend.queueRatio(), 0.6);
        backend.setQueueRatio(0.0);
        QCOMPARE(backend.queueRatio(), 0.15);
    }

    void visualizerRateCyclesAndIsSaved()
    {
        QTemporaryDir dir;
        const QString state = dir.path() + "/state.json";
        {
            Backend backend;
            backend.setStateFile(state);
            backend.setVolume(0);
            QCOMPARE(backend.visualizerRate(), 0);  // follows the display by default
            backend.cycleVisualizerRate();
            QCOMPARE(backend.visualizerRate(), 30);
            backend.cycleVisualizerRate();
            QCOMPARE(backend.visualizerRate(), 60);
            backend.setVisualizerRate(100);  // a custom rate steps up to the next preset
            backend.cycleVisualizerRate();
            QCOMPARE(backend.visualizerRate(), 120);
            backend.setVisualizerRate(240);
            backend.cycleVisualizerRate();
            QCOMPARE(backend.visualizerRate(), 0);  // and wraps back to the display
            backend.setVisualizerRate(5000);
            QCOMPARE(backend.visualizerRate(), 360);
            backend.setVisualizerRate(3);
            QCOMPARE(backend.visualizerRate(), 10);
            backend.setVisualizerRate(75);
            backend.saveState();
        }
        Backend again;
        again.setStateFile(state);
        QCOMPARE(again.visualizerRate(), 75);
    }

    void folderCoverFindsConventionalNamesIgnoringCase()
    {
        auto folderWith = [&](const QStringList &files) {
            auto dir = std::make_unique<QTemporaryDir>();
            for (const auto &f : files)
                touch(dir->path() + "/" + f);
            return dir;
        };
        auto name = [](const QString &path) { return QFileInfo(path).fileName(); };

        auto d1 = folderWith({"track.mp3", "Cover.JPG", "booklet.jpg"});
        QCOMPARE(name(Backend::findFolderCover(d1->path())), QString("Cover.JPG"));
        // cover beats folder, which beats front.
        auto d2 = folderWith({"front.png", "folder.jpg", "x.jpg"});
        QCOMPARE(name(Backend::findFolderCover(d2->path())), QString("folder.jpg"));
        // Windows Media Player's large cover wins over its small one.
        auto d3 = folderWith({"AlbumArtSmall.jpg", "AlbumArt_{1234}_Large.jpg"});
        QCOMPARE(name(Backend::findFolderCover(d3->path())), QString("AlbumArt_{1234}_Large.jpg"));
        // Not an image, so not a cover.
        auto d4 = folderWith({"track.mp3", "cover.txt", "notes.pdf"});
        QVERIFY(Backend::findFolderCover(d4->path()).isEmpty());
    }

    void folderCoverFallsBackToTheOnlyImage()
    {
        QTemporaryDir one, many;
        touch(one.path() + "/Some Album Name.webp");
        QCOMPARE(QFileInfo(Backend::findFolderCover(one.path())).fileName(), QString("Some Album Name.webp"));
        // Several unnamed images could be scans or a back cover; guessing is worse than none.
        touch(many.path() + "/scan1.jpg");
        touch(many.path() + "/scan2.jpg");
        QVERIFY(Backend::findFolderCover(many.path()).isEmpty());
    }

    void largeImagesAreDecodedAtAReducedSize()
    {
        QTemporaryDir dir;
        QImage big(3000, 2000, QImage::Format_RGB32);
        big.fill(Qt::red);
        QVERIFY(big.save(dir.path() + "/big.png"));
        const QImage loaded = Backend::loadScaledImage(dir.path() + "/big.png", 800);
        QCOMPARE(loaded.width(), 800);
        QCOMPARE(loaded.height(), 533);
        QVERIFY(Backend::loadScaledImage(dir.path() + "/missing.png").isNull());
    }

    void emptyLibraryIsReported()
    {
        QTemporaryDir dir;
        Backend backend;
        ready(backend, dir, {});
        QVERIFY(backend.error().contains("No music found"));
    }

    void missingFolderIsReported()
    {
        Backend backend;
        backend.open("/nonexistent/hum-test");
        QVERIFY(backend.error().contains("doesn't exist"));
    }

    void volumeStaysInRange()
    {
        Backend backend;
        backend.setVolume(3);
        QCOMPARE(backend.volume(), 1.0);
        backend.changeVolume(-5);
        QCOMPARE(backend.volume(), 0.0);
    }

    void analyzerSilenceIsFlat()
    {
        Analyzer analyzer;
        QVector<float> silence(Analyzer::FftSize, 0.0f);
        analyzer.push(silence.constData(), silence.size(), 44100);
        for (float level : analyzer.compute())
            QCOMPARE(level, 0.0f);
    }

    void analyzerPeaksAtTheTone()
    {
        Analyzer analyzer;
        const auto levels = settle(analyzer, 0.8f, 1000.0, 120);

        const int peak = int(std::max_element(levels.begin(), levels.end()) - levels.begin());
        QVERIFY(analyzer.bandFrequency(peak) <= 1000.0 * 1.1);
        QVERIFY(analyzer.bandFrequency(peak + 1) >= 1000.0 / 1.1);
        QVERIFY(levels[peak] > 0.5f);
        QVERIFY(levels[2] < 0.1f);
    }

    void analyzerLearnsItsSensitivity()
    {
        // A very quiet tone ends up as tall as a loud one, and neither clips.
        Analyzer quiet, loud;
        const auto quietLevels = settle(quiet, 0.02f, 1000.0, 400);
        const auto loudLevels = settle(loud, 0.9f, 1000.0, 400);
        QVERIFY(*std::max_element(quietLevels.begin(), quietLevels.end()) > 0.5f);
        QVERIFY(*std::max_element(loudLevels.begin(), loudLevels.end()) <= 1.0f);
    }

    void analyzerFallsGradually()
    {
        Analyzer analyzer;
        const auto before = settle(analyzer, 0.8f, 1000.0, 120);
        const int peak = int(std::max_element(before.begin(), before.end()) - before.begin());
        // Silence arrives; the bar eases down instead of dropping at once.
        QVector<float> silence(Analyzer::FftSize, 0.0f);
        analyzer.push(silence.constData(), silence.size(), 44100);
        QVector<float> after;
        for (int i = 0; i < 16; ++i)
            after = analyzer.compute();
        QVERIFY(after[peak] < before[peak] * 0.9f);
        QVERIFY(after[peak] > 0.0f);
    }

    void analyzerDecaysToZero()
    {
        Analyzer analyzer;
        QVector<float> tone(Analyzer::FftSize, 0.5f);
        analyzer.push(tone.constData(), tone.size(), 44100);
        analyzer.compute();
        QVector<float> levels;
        for (int i = 0; i < 80; ++i)
            levels = analyzer.decay();
        for (float level : levels)
            QCOMPARE(level, 0.0f);
    }

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        QFile colors(dir.path() + "/theme/colors.toml");
        QVERIFY(colors.open(QIODevice::WriteOnly));
        colors.write("accent = \"#112233\"\n");
        colors.close();

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void themeReadsThePalette()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml",
              "mode = \"light\"\nbackground = \"#FAF4ED\"\nforeground = \"#575279\"\n"
              "muted = '#9893a5'\nselection = \"#DFDAD9\"\nred = \"#B4637A\"\n");

        Theme theme(dir.path());
        QCOMPARE(theme.background(), QString("#FAF4ED"));
        QCOMPARE(theme.foreground(), QString("#575279"));
        QCOMPARE(theme.muted(), QString("#9893a5"));
        QCOMPARE(theme.danger(), QString("#B4637A"));
        QVERIFY(!theme.dark());
        // No lighter_background in this theme, so panels start from the selection
        // color and are darkened, toward this light theme's text, until they show.
        QVERIFY(QColor(theme.raised()) != QColor("#DFDAD9"));
        QVERIFY(Theme::contrastRatio(QColor(theme.raised()), QColor("#FAF4ED")) >= 1.6);
    }

    void dimTextAndHighlightsStayReadable()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        // A theme whose muted and selection colors are barely visible on its
        // background, like the ones that looked washed out on glass.
        write(dir.path() + "/theme/colors.toml",
              "background = \"#0B0F14\"\nforeground = \"#7CFFB2\"\nmuted = \"#2a3340\"\nselection = \"#0e1319\"\n");
        Theme theme(dir.path());
        const QColor background(theme.background());
        // Transparent, the default here, asks for more than a solid window.
        QVERIFY(Theme::contrastRatio(QColor(theme.dim()), background) >= 6.5);
        QVERIFY(Theme::contrastRatio(QColor(theme.highlight()), background) >= 2.1);
        QVERIFY(Theme::contrastRatio(QColor(theme.raised()), background) >= 1.6);
        // The brightened color stays in the theme: it is the muted color mixed toward the foreground.
        QVERIFY(QColor(theme.dim()) != QColor(theme.foreground()));

        QDir(dir.path()).mkpath("toggles/hypr");
        touch(dir.path() + "/toggles/hypr/window-opaque.lua");
        QTRY_COMPARE(theme.windowMode(), QString("opaque"));
        QVERIFY(Theme::contrastRatio(QColor(theme.dim()), background) >= 4.6);
        QVERIFY(Theme::contrastRatio(QColor(theme.dim()), background) < 6.5);
    }

    void aColorThatAlreadyReadsIsLeftAlone()
    {
        const QColor background("#000000"), fine("#cccccc");
        QCOMPARE(Theme::ensureContrast(fine, QColor("#ffffff"), background, 4.5), fine);
        // Unreachable targets end at the destination color rather than looping.
        QCOMPARE(Theme::ensureContrast(QColor("#101010"), QColor("#202020"), background, 21), QColor("#202020"));
        QVERIFY(qFuzzyCompare(Theme::contrastRatio(QColor("#ffffff"), background), 21.0));
    }

    void themeFollowsAThemeSwitch()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "background = \"#111111\"\n");
        Theme theme(dir.path());
        QCOMPARE(theme.background(), QString("#111111"));
        QVERIFY(theme.dark());

        write(dir.path() + "/theme/colors.toml", "mode = \"light\"\nbackground = \"#EEEEEE\"\n");
        QTRY_COMPARE(theme.background(), QString("#EEEEEE"));
        QVERIFY(!theme.dark());
    }

    void themeFollowsTheMonospaceFont()
    {
        QTemporaryDir dir;
        const QString fontFile = dir.filePath("family"), command = dir.filePath("fc-match");
        write(command, "#!/bin/sh\ncat '" + fontFile.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(fontFile, "JetBrainsMono Nerd Font,JetBrainsMono NF\n");

        Theme theme(dir.path(), nullptr, {}, command);
        QTRY_COMPARE(theme.fontFamily(), QString("JetBrainsMono Nerd Font"));

        // omarchy-font-set rewrites fonts.conf, which is what the theme watches.
        QDir(dir.path()).mkpath("fontconfig");
        write(fontFile, "CaskaydiaMono Nerd Font\n");
        write(dir.path() + "/fontconfig/fonts.conf", "<fontconfig/>");
        QTRY_COMPARE(theme.fontFamily(), QString("CaskaydiaMono Nerd Font"));
    }

    void themeFollowsWindowOpacity()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.windowMode(), QString("transparent"));
        QCOMPARE(theme.windowAlpha(), 0.985);

        QDir(dir.path()).mkpath("toggles/hypr");
        touch(dir.path() + "/toggles/hypr/window-glassy.lua");
        QTRY_COMPARE(theme.windowMode(), QString("glassy"));
        QCOMPARE(theme.windowAlpha(), 0.65);

        QFile::remove(dir.path() + "/toggles/hypr/window-glassy.lua");
        touch(dir.path() + "/toggles/hypr/window-opaque.lua");
        QTRY_COMPARE(theme.windowMode(), QString("opaque"));
        QCOMPARE(theme.windowAlpha(), 1.0);

        QFile::remove(dir.path() + "/toggles/hypr/window-opaque.lua");
        QTRY_COMPARE(theme.windowMode(), QString("transparent"));
    }

    void themeNoticesTheTogglesFolderBeingCreated()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QDir(dir.path()).mkpath("toggles/hypr");
        touch(dir.path() + "/toggles/hypr/window-glassy.lua");
        QTRY_COMPARE(theme.windowMode(), QString("glassy"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        auto write = [](const QString &path, const QByteArray &data) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
        };
        QVERIFY(write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n"));
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(write(response, "{\"int\":0}"));

        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        QVERIFY(write(response, "{\"int\":6}"));
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pt-BR\n");
        write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})");
        write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorPicksPluralForms()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "ru\n");
        write(dir.path() + "/en.json", R"({"%n clips": {"one": "%n clip", "other": "%n clips"}})");
        write(dir.path() + "/ru.json", R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QString("%n клип"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 22), QString("%n клипа"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QString("%n клипов"));
        QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
        QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
    }

private:
    static void touch(const QString &path)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
    }

    // Scans a temporary library and waits for the result.
    static void ready(Backend &backend, QTemporaryDir &dir, const QStringList &files)
    {
        for (const auto &file : files)
            touch(dir.path() + "/" + file);
        // Each test gets its own tag cache, so none touch the user's.
        backend.setCacheFile(dir.path() + "/.cache/tags.json");
        QSignalSpy scanned(&backend, &Backend::scanningChanged);
        QSignalSpy read(&backend, &Backend::readingChanged);
        backend.open(dir.path());
        while (backend.scanning())
            scanned.wait(2000);
        while (backend.reading())
            read.wait(5000);
    }


    // Feeds a steady tone for a while, like a track playing, and returns the bars.
    static QVector<float> settle(Analyzer &analyzer, float amplitude, double hz, int frames)
    {
        QVector<float> tone(Analyzer::FftSize);
        for (int i = 0; i < tone.size(); ++i)
            tone[i] = amplitude * std::sin(2.0 * M_PI * hz * i / 44100.0);
        QVector<float> levels;
        for (int i = 0; i < frames; ++i) {
            analyzer.push(tone.constData(), tone.size(), 44100);
            levels = analyzer.compute();
        }
        return levels;
    }

    // A short silent mp3 with tags, made with ffmpeg.
    static void make(const QString &path, const QString &title, const QString &artist, const QString &album, int track)
    {
        QProcess ffmpeg;
        ffmpeg.start("ffmpeg", {"-v", "error", "-f", "lavfi", "-i", "anullsrc=r=22050:cl=mono", "-t", "5",
                                "-metadata", "title=" + title, "-metadata", "artist=" + artist,
                                "-metadata", "album=" + album, "-metadata", "track=" + QString::number(track), "-y", path});
        QVERIFY(ffmpeg.waitForFinished(15000));
        QCOMPARE(ffmpeg.exitCode(), 0);
    }

    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }
};

QTEST_MAIN(HumTests)
#include "hum_tests.moc"
