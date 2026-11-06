#include "plugininstall.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>
#include <QUuid>

namespace {
const char *PluginId = "markm.hum-visualizer";

// Packaged installs keep the plugin in /usr/share/hum; a development build
// finds it beside the source tree.
QString findSource() {
    const QStringList candidates{"/usr/share/hum/omarchy-plugin",
                                 QCoreApplication::applicationDirPath() + "/../omarchy-plugin",
                                 QCoreApplication::applicationDirPath() + "/../../omarchy-plugin"};
    for (const auto &path : candidates)
        if (QFileInfo::exists(path + "/manifest.json"))
            return QDir(path).absolutePath();
    return {};
}

bool copyFolder(const QString &from, const QString &to) {
    QDir().mkpath(to);
    const QDir source(from);
    for (const auto &entry : source.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        // The tests are for development and stay behind.
        if (entry.fileName() == "tests")
            continue;
        const QString target = to + "/" + entry.fileName();
        if (entry.isDir() ? !copyFolder(entry.filePath(), target) : !QFile::copy(entry.filePath(), target))
            return false;
    }
    return true;
}
}

int installBarPlugin(const QStringList &arguments) {
    QTextStream out(stdout), err(stderr);
    const bool enable = arguments.contains("--enable");
    const QString source = findSource();
    if (source.isEmpty()) {
        err << "hum: can't find the bar plugin files to install\n";
        return 1;
    }

    const QString plugins = QDir::homePath() + "/.config/omarchy/plugins";
    const QString target = plugins + "/" + PluginId;
    // Copy beside the target and swap, so a failure never leaves half a plugin
    // for the shell to load.
    const QString staging = plugins + "/." + PluginId + "." + QUuid::createUuid().toString(QUuid::Id128).left(8);
    if (!copyFolder(source, staging)) {
        QDir(staging).removeRecursively();
        err << "hum: couldn't copy the plugin to " << plugins << "\n";
        return 1;
    }
    QDir(target).removeRecursively();
    if (!QDir().rename(staging, target)) {
        QDir(staging).removeRecursively();
        err << "hum: couldn't put the plugin in place at " << target << "\n";
        return 1;
    }
    out << "Installed the Hum bar plugin to " << target << "\n";

    const QString omarchy = QStandardPaths::findExecutable("omarchy");
    if (omarchy.isEmpty()) {
        out << "Omarchy isn't installed here, so the plugin is not enabled.\n";
        return 0;
    }
    // The shell notices new files, but enabling needs it to know this one
    // already, so ask it to look now.
    const QString shell = QStandardPaths::findExecutable("omarchy-shell");
    if (!shell.isEmpty())
        QProcess::execute(shell, {"shell", "rescanPlugins"});
    if (!enable) {
        out << "Enable it with: hum plugin install --enable\n";
        return 0;
    }
    if (QProcess::execute(omarchy, {"plugin", "enable", PluginId}) != 0) {
        err << "hum: `omarchy plugin enable " << PluginId << "` failed\n";
        return 1;
    }
    out << "Enabled. Move it with: omarchy bar move " << PluginId << " --section left\n";
    return 0;
}
