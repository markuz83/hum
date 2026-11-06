#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

#include "analyzer.h"
#include "backend.h"
#include "theme.h"
#include "translator.h"

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
        QCOMPARE(backend.tracks().size(), 3);
        backend.setFilter("bowie");
        QCOMPARE(backend.tracks(), QStringList({"Bowie/Heroes", "Bowie/Low"}));
        backend.setFilter("");
        QCOMPARE(backend.tracks().size(), 3);
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
        QVector<float> tone(Analyzer::FftSize);
        for (int i = 0; i < tone.size(); ++i)
            tone[i] = 0.8f * std::sin(2.0 * M_PI * 1000.0 * i / 44100.0);
        analyzer.push(tone.constData(), tone.size(), 44100);
        QVector<float> levels;
        for (int i = 0; i < 5; ++i)
            levels = analyzer.compute();

        const int peak = int(std::max_element(levels.begin(), levels.end()) - levels.begin());
        QVERIFY(analyzer.bandFrequency(peak) <= 1000.0 * 1.1);
        QVERIFY(analyzer.bandFrequency(peak + 1) >= 1000.0 / 1.1);
        QVERIFY(levels[peak] > 0.5f);
        QVERIFY(levels[2] < 0.1f);
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
        QSignalSpy done(&backend, &Backend::scanningChanged);
        backend.open(dir.path());
        while (backend.scanning())
            done.wait(2000);
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
