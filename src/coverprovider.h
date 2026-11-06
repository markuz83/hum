#pragma once

#include <QImage>
#include <QQuickImageProvider>
#include <functional>

// Hands the current track's cover art to QML as image://cover/<id>. The id
// changes with every cover, so the Image item never shows a cached old one.
class CoverProvider : public QQuickImageProvider {
public:
    explicit CoverProvider(std::function<QImage()> cover)
        : QQuickImageProvider(QQuickImageProvider::Image), m_cover(std::move(cover)) {}

    QImage requestImage(const QString &, QSize *size, const QSize &requested) override {
        QImage image = m_cover();
        if (requested.isValid() && !image.isNull() && (image.width() > requested.width() || image.height() > requested.height()))
            image = image.scaled(requested, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        if (size)
            *size = image.size();
        return image;
    }

private:
    std::function<QImage()> m_cover;
};
