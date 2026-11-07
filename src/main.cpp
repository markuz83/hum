// hum — plays the music in your Music folder, with a visualizer.

#include <QCommandLineParser>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QStandardPaths>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QUrl>

#include "backend.h"
#include "barlink.h"
#include "cli.h"
#include "coverprovider.h"
#include "mpris.h"
#include "plugininstall.h"
#include "portalfilepicker.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    // `hum plugin install` needs no window, so it runs before any GUI exists.
    if (argc >= 3 && QString(argv[1]) == "plugin" && QString(argv[2]) == "install") {
        QCoreApplication cli(argc, argv);
        QStringList flags;
        for (int i = 3; i < argc; ++i)
            flags << argv[i];
        return installBarPlugin(flags);
    }

    // `hum next`, `hum status --json` and the like control a running Hum.
    if (argc >= 2 && Cli::isCommand(argv[1]) && !QFileInfo::exists(argv[1])) {
        QCoreApplication cli(argc, argv);
        QStringList args;
        for (int i = 1; i < argc; ++i)
            args << argv[i];
        QTextStream out(stdout), err(stderr);
        const int code = Cli::run(args, out, err);
        out.flush();
        err.flush();
        return code;
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("hum");
    app.setApplicationVersion("0.1.1");

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
    parser.setApplicationDescription(QObject::tr("Plays your music with a visualizer.") + "\n\n" + Cli::usage());
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("path", QObject::tr("A music folder, or a file to start playing."));
    parser.process(app);

    Theme theme;
    Backend backend;
    // Volume, shuffle, repeat, visualizer, and the last track come back next time.
    backend.setStateFile(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/state.json");
    PortalFilePicker picker;
    backend.setFilePicker(&picker);
    backend.open(parser.positionalArguments().value(0));

    // Media keys, `playerctl`, and desktop media widgets control Hum through this.
    Mpris mpris(&backend);
    // The Omarchy bar plugin draws the live spectrum from this socket.
    BarLink barLink(&backend);

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
