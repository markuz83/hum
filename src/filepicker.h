#pragma once

#include <QObject>
#include <QString>

// Asks the user for a folder. Behind an interface so tests can answer without
// a desktop, and the real one can talk to xdg-desktop-portal.
class FilePicker : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    virtual void pickFolder(const QString &title) = 0;

signals:
    void picked(const QString &path);
    void failed(const QString &message);
};
