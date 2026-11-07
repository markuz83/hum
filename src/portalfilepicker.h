#pragma once

#include <QVariantMap>

#include "filepicker.h"

// Adapted from the portal file picker in Omacut (MIT, omacom/omacut).
//
// The folder chooser from xdg-desktop-portal (org.freedesktop.portal.FileChooser),
// the same dialog every other app on the desktop shows. Nothing is drawn by Qt.
class PortalFilePicker : public FilePicker {
    Q_OBJECT

public:
    using FilePicker::FilePicker;
    void pickFolder(const QString &title) override;

private slots:
    void onResponse(uint response, const QVariantMap &results);

private:
    int m_counter = 0;
};
