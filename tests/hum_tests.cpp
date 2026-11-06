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
        for (int i = 0; i < 8; ++i)
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
