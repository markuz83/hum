// hum — plays the music in your Music folder, with a visualizer.

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QStandardPaths>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QUrl>

#include "backend.h"
#include "coverprovider.h"
#include "mpris.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName("hum");
    app.setApplicationVersion("0.1.0");

    // Matches the window to hum.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("hum");
    app.setWindowIcon(QIcon::fromTheme("hum"));

    // The window background can be see-through, following Omarchy's window
    // opacity setting, so the surface needs an alpha channel.
    QQuickWindow::setDefaultAlphaBuffer(true);

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    QCommandLineParser parser;
    parser.setApplicationDescription(QObject::tr("Plays your music with a visualizer."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("path", QObject::tr("A music folder, or a file to start playing."));
    parser.process(app);

    Theme theme;
    Backend backend;
    // Volume, shuffle, repeat, visualizer, and the last track come back next time.
    backend.setStateFile(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/state.json");
    backend.open(parser.positionalArguments().value(0));

    // Media keys, `playerctl`, and desktop media widgets control Hum through this.
    Mpris mpris(&backend);

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.addImageProvider("cover", new CoverProvider([&backend] { return backend.cover(); }));
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
