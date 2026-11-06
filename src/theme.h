#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QTimer>

class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString accent READ accent NOTIFY changed)
    Q_PROPERTY(QString accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(QString background READ background NOTIFY changed)
    Q_PROPERTY(QString foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QString muted READ muted NOTIFY changed)
    Q_PROPERTY(QString selection READ selection NOTIFY changed)
    Q_PROPERTY(QString raised READ raised NOTIFY changed)
    Q_PROPERTY(QString danger READ danger NOTIFY changed)
    Q_PROPERTY(bool dark READ dark NOTIFY changed)
    Q_PROPERTY(QString fontFamily READ fontFamily NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)
    Q_PROPERTY(QString windowMode READ windowMode NOTIFY changed)
    Q_PROPERTY(qreal windowAlpha READ windowAlpha NOTIFY changed)

public:
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprctl = {},
                   const QString &fcMatch = {});
    ~Theme() override;

    QString accent() const { return m_accent; }
    QString accentForeground() const;
    // The theme's palette: window ground, text, quiet text, a highlighted row,
    // a panel slightly lighter than the ground, and red for errors.
    QString background() const { return m_colors.value("background"); }
    QString foreground() const { return m_colors.value("foreground"); }
    QString muted() const { return m_colors.value("muted"); }
    QString selection() const { return m_colors.value("selection"); }
    QString raised() const { return m_colors.value("lighter_background"); }
    QString danger() const { return m_colors.value("red"); }
    bool dark() const { return m_dark; }
    // Omarchy's monospace font, from fontconfig, which `omarchy font set` writes.
    QString fontFamily() const { return m_fontFamily; }
    int radius() const { return m_radius; }
    // Omarchy's Appearance > Windows > Window Opacity: opaque, transparent, or glassy.
    QString windowMode() const { return m_windowMode; }
    // How solid the window background should be for that mode.
    qreal windowAlpha() const;

    static QString readAccent(const QString &path);
    static QString readColor(const QString &path, const QString &key, const QString &fallback);
    static bool readDark(const QString &path);
    static QString readWindowMode(const QString &togglesDirectory);

signals:
    void changed();

private:
    void reload();
    void refreshRounding();
    void refreshFont();

    QString m_directory;
    QString m_togglesDirectory;
    QString m_windowMode = "transparent";
    QString m_accent = "#FFD60A";
    // Without Omarchy these are the dark colors Hum was designed on.
    QHash<QString, QString> m_colors = defaultColors();
    bool m_dark = true;
    static QHash<QString, QString> defaultColors();
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprctl;
    QString m_fcMatch;
    QString m_fontsDirectory;
    QString m_fontFamily = "monospace";
    QProcess m_fontProcess;
    QTimer m_fontTimeout;
    int m_radius = 0;
    QProcess m_roundingProcess;
    QTimer m_roundingPoll, m_roundingTimeout;
};
