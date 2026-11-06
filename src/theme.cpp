// Theme watching follows Omacut (MIT, David Heinemeier Hansson).
#include "theme.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <cmath>

Theme::Theme(const QString &directory, QObject *parent, const QString &hyprctl, const QString &fcMatch)
    : QObject(parent),
      m_directory(directory.isEmpty() ? QDir::homePath() + "/.local/state/omarchy/current" : directory),
      m_togglesDirectory(directory.isEmpty() ? QDir::homePath() + "/.local/state/omarchy/toggles/hypr"
                                             : directory + "/toggles/hypr") {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Theme::reload);

    // Corners follow Hyprland's effective rounding, which themes and personal
    // overrides can both set, so ask hyprctl rather than reading its config.
    // Without Hyprland the corners stay sharp, like Omarchy's default.
    m_hyprctl = !hyprctl.isEmpty() ? hyprctl
              : directory.isEmpty() && !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")
                  ? QStandardPaths::findExecutable("hyprctl") : QString();
    // Fontconfig is the source of truth for the font, and `omarchy font set`
    // rewrites ~/.config/fontconfig/fonts.conf, so watch that and ask
    // fc-match what "monospace" resolves to. Tests pass their own fc-match.
    m_fcMatch = !fcMatch.isEmpty() ? fcMatch : directory.isEmpty() ? QStandardPaths::findExecutable("fc-match") : QString();
    m_fontsDirectory = directory.isEmpty() ? QDir::homePath() + "/.config/fontconfig" : directory + "/fontconfig";
    m_fontTimeout.setSingleShot(true);
    m_fontTimeout.setInterval(1000);
    connect(&m_fontTimeout, &QTimer::timeout, &m_fontProcess, &QProcess::kill);
    connect(&m_fontProcess, &QProcess::errorOccurred, this, [this] { m_fontTimeout.stop(); });
    connect(&m_fontProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_fontTimeout.stop();
        const QString family = QString::fromUtf8(m_fontProcess.readAllStandardOutput()).section('\n', 0, 0).section(',', 0, 0).trimmed();
        if (code != 0 || status != QProcess::NormalExit || family.isEmpty() || family == m_fontFamily)
            return;
        m_fontFamily = family;
        emit changed();
    });

    m_roundingTimeout.setSingleShot(true);
    m_roundingTimeout.setInterval(500);
    connect(&m_roundingTimeout, &QTimer::timeout, &m_roundingProcess, &QProcess::kill);
    connect(&m_roundingProcess, &QProcess::errorOccurred, this, [this] { m_roundingTimeout.stop(); });
    connect(&m_roundingProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_roundingTimeout.stop();
        const auto value = QJsonDocument::fromJson(m_roundingProcess.readAllStandardOutput()).object().value("int");
        if (code != 0 || status != QProcess::NormalExit || !value.isDouble() || value.toInt(-1) < 0)
            return;
        if (value.toInt() != m_radius) {
            m_radius = value.toInt();
            emit changed();
        }
    });
    m_roundingPoll.setInterval(1000);
    connect(&m_roundingPoll, &QTimer::timeout, this, &Theme::refreshRounding);
    if (!m_hyprctl.isEmpty())
        m_roundingPoll.start();
    reload();
}

Theme::~Theme() {
    if (m_fontProcess.state() != QProcess::NotRunning) {
        m_fontProcess.kill();
        m_fontProcess.waitForFinished(1000);
    }
    if (m_roundingProcess.state() != QProcess::NotRunning) {
        m_roundingProcess.kill();
        m_roundingProcess.waitForFinished(1000);
    }
}

void Theme::refreshRounding() {
    if (m_hyprctl.isEmpty() || m_roundingProcess.state() != QProcess::NotRunning)
        return;
    m_roundingProcess.start(m_hyprctl, {"-j", "getoption", "decoration:rounding"});
    m_roundingTimeout.start();
}

void Theme::refreshFont() {
    if (m_fcMatch.isEmpty() || m_fontProcess.state() != QProcess::NotRunning)
        return;
    m_fontProcess.start(m_fcMatch, {"monospace", "-f", "%{family}\n"});
    m_fontTimeout.start();
}

QString Theme::readColor(const QString &path, const QString &key, const QString &fallback) {
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QRegularExpression expression(
            "^\\s*" + QRegularExpression::escape(key) + R"re(\s*=\s*["'](#[0-9a-fA-F]{6})["'])re",
            QRegularExpression::MultilineOption);
        const auto match = expression.match(QString::fromUtf8(file.readAll()));
        if (match.hasMatch())
            return match.captured(1);
    }
    return fallback;
}

QString Theme::readAccent(const QString &path) {
    return readColor(path, "accent", "#FFD60A");
}

// Light themes say so with mode = "light"; anything else is dark.
bool Theme::readDark(const QString &path) {
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QRegularExpression expression(R"re(^\s*mode\s*=\s*["']light["'])re", QRegularExpression::MultilineOption);
        return !expression.match(QString::fromUtf8(file.readAll())).hasMatch();
    }
    return true;
}

QHash<QString, QString> Theme::defaultColors() {
    return {{"background", "#0e0e10"}, {"foreground", "#ffffff"}, {"muted", "#8a8a93"},
            {"selection", "#26262c"}, {"lighter_background", "#1b1b1f"}, {"red", "#ff6b6b"}};
}

// Omarchy records the choice as a file under toggles/hypr: window-opaque.lua
// or window-glassy.lua, and neither means transparent, the default.
QString Theme::readWindowMode(const QString &togglesDirectory) {
    if (QFileInfo::exists(togglesDirectory + "/window-opaque.lua"))
        return "opaque";
    if (QFileInfo::exists(togglesDirectory + "/window-glassy.lua"))
        return "glassy";
    return "transparent";
}

// Transparent matches the 0.985 Omarchy gives its own see-through windows, and
// glassy the 0.65 it gives terminals, which the compositor then blurs behind.
qreal Theme::windowAlpha() const {
    return m_windowMode == "opaque" ? 1.0 : m_windowMode == "glassy" ? 0.65 : 0.985;
}

double Theme::contrastRatio(const QColor &a, const QColor &b) {
    auto luminance = [](const QColor &c) {
        auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
        return .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
    };
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + .05) / (std::min(la, lb) + .05);
}

QColor Theme::ensureContrast(const QColor &from, const QColor &toward, const QColor &background, double target) {
    for (int step = 0; step <= 20; ++step) {
        const double t = step / 20.0;
        const QColor mixed = QColor::fromRgbF(from.redF() + (toward.redF() - from.redF()) * t,
                                              from.greenF() + (toward.greenF() - from.greenF()) * t,
                                              from.blueF() + (toward.blueF() - from.blueF()) * t);
        if (contrastRatio(mixed, background) >= target)
            return mixed;
    }
    return toward;
}

// A window the wallpaper shows through needs more contrast than a solid one,
// since what is behind the text is lighter than the theme's own background.
QString Theme::dim() const {
    const double target = m_windowMode == "opaque" ? 4.6 : 6.5;
    return ensureContrast(QColor(muted()), QColor(foreground()), QColor(background()), target).name();
}

QString Theme::highlight() const {
    const double target = m_windowMode == "opaque" ? 1.7 : 2.1;
    return ensureContrast(QColor(selection()), QColor(foreground()), QColor(background()), target).name();
}

QString Theme::raised() const {
    const double target = m_windowMode == "opaque" ? 1.35 : 1.6;
    return ensureContrast(QColor(m_colors.value("lighter_background")), QColor(foreground()), QColor(background()), target).name();
}

// Black or white, whichever reads on the accent (WCAG relative luminance).
QString Theme::accentForeground() const {
    const QColor c(m_accent);
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    const double luminance = .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
    return luminance > .179 ? "black" : "white";
}

void Theme::reload() {
    refreshRounding();
    refreshFont();
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);

    // A theme switch replaces symlinks and files, so watch the parents too and
    // re-arm after every change. Watching the nearest existing ancestor means
    // installing Omarchy later works without restarting the app.
    QString ancestor = m_directory;
    while (!QFileInfo::exists(ancestor) && ancestor != "/")
        ancestor = QFileInfo(ancestor).absolutePath();
    // The window-opacity toggles live elsewhere in the state directory, and
    // their folder may not exist until the user first picks a mode.
    QString togglesAncestor = m_togglesDirectory;
    while (!QFileInfo::exists(togglesAncestor) && togglesAncestor != "/")
        togglesAncestor = QFileInfo(togglesAncestor).absolutePath();
    QStringList candidates{ancestor, QFileInfo(ancestor).absolutePath(), m_directory,
                           m_directory + "/theme", m_directory + "/theme/colors.toml",
                           togglesAncestor, m_togglesDirectory};
    // The fontconfig folder may not exist until the user first picks a font.
    QString fontsAncestor = m_fontsDirectory;
    while (!QFileInfo::exists(fontsAncestor) && fontsAncestor != "/")
        fontsAncestor = QFileInfo(fontsAncestor).absolutePath();
    candidates << fontsAncestor << m_fontsDirectory << m_fontsDirectory + "/fonts.conf";
    candidates.removeDuplicates();
    for (const auto &path : candidates)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);

    const QString colors = m_directory + "/theme/colors.toml";
    const auto accent = readAccent(colors);
    const auto mode = readWindowMode(m_togglesDirectory);
    const bool dark = readDark(colors);
    QHash<QString, QString> palette = defaultColors();
    for (auto it = palette.begin(); it != palette.end(); ++it)
        it.value() = readColor(colors, it.key(), it.value());
    // A theme without lighter_background still gets a distinct panel color.
    if (readColor(colors, "lighter_background", {}).isEmpty())
        palette["lighter_background"] = palette["selection"];
    if (accent != m_accent || mode != m_windowMode || dark != m_dark || palette != m_colors) {
        m_accent = accent;
        m_windowMode = mode;
        m_dark = dark;
        m_colors = palette;
        emit changed();
    }
}
