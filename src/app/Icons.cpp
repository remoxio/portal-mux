#include "app/Icons.h"

#include <QApplication>
#include <QBuffer>
#include <QFile>
#include <QIconEngine>
#include <QImageReader>
#include <QPainter>
#include <QPalette>
#include <QPixmap>

namespace {

// SVG recoloured at paint time, so it follows palette changes
class TintedSvgEngine : public QIconEngine {
public:
    TintedSvgEngine(QByteArray svg, QColor color) : svg_(std::move(svg)), color_(color) {}

    QIconEngine* clone() const override { return new TintedSvgEngine(svg_, color_); }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override
    {
        const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, dpr));
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override { return scaledPixmap(size, mode, state, 1.0); }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State, qreal scale) override
    {
        const QPalette palette = QApplication::palette();
        QColor color = color_.isValid() ? color_
                     : mode == QIcon::Selected ? palette.color(QPalette::HighlightedText)
                                               : palette.color(QPalette::WindowText);
        if (mode == QIcon::Disabled) color.setAlphaF(color.alphaF() * 0.4);
        QByteArray svg = svg_;
        svg.replace("currentColor", color.name(QColor::HexRgb).toLatin1());
        QBuffer buffer(&svg);
        QImageReader reader(&buffer, "svg");
        reader.setScaledSize(size * scale);
        QImage image = reader.read().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (color.alpha() < 255) {
            QImage faded(image.size(), QImage::Format_ARGB32_Premultiplied);
            faded.fill(Qt::transparent);
            QPainter p(&faded);
            p.setOpacity(color.alphaF());
            p.drawImage(0, 0, image);
            p.end();
            image = faded;
        }
        QPixmap pixmap = QPixmap::fromImage(image);
        pixmap.setDevicePixelRatio(scale);
        return pixmap;
    }

private:
    QByteArray svg_;
    QColor color_;
};

QIcon tinted(const QString& path, QColor color)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QIcon(new TintedSvgEngine(file.readAll(), color));
}

} // namespace

namespace icons {

QIcon material(const char* name)
{
    return QIcon(QStringLiteral(":/icons/material-icon-theme/%1.svg").arg(QLatin1String(name)));
}

QIcon brand(const char* name)
{
    return tinted(QStringLiteral(":/icons/brands/%1.svg").arg(QLatin1String(name)), {});
}

QIcon lucide(const char* name)
{
    return tinted(QStringLiteral(":/icons/lucide/%1.svg").arg(QLatin1String(name)), {});
}

QIcon tabler(const char* name, QColor color)
{
    return tinted(QStringLiteral(":/icons/tabler/%1.svg").arg(QLatin1String(name)), color);
}

} // namespace icons
