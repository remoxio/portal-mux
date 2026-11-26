#include "app/Icons.h"

#include <QApplication>
#include <QBuffer>
#include <QFile>
#include <QIconEngine>
#include <QImageReader>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QWidget>

namespace {

// SVG recoloured at paint time, from the painted widget's palette
class TintedSvgEngine : public QIconEngine {
public:
    TintedSvgEngine(QByteArray svg, QColor color, QPalette::ColorRole role = QPalette::WindowText)
        : svg_(std::move(svg)), color_(color), role_(role)
    {
    }

    QIconEngine* clone() const override { return new TintedSvgEngine(svg_, color_, role_); }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State) override
    {
        const QPaintDevice* device = painter->device();
        const auto* widget = dynamic_cast<const QWidget*>(device);
        painter->drawPixmap(rect, render(rect.size(), mode, device ? device->devicePixelRatioF() : 1.0,
                                         widget ? widget->palette() : QApplication::palette()));
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override { return scaledPixmap(size, mode, state, 1.0); }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State, qreal scale) override
    {
        return render(size, mode, scale, QApplication::palette());
    }

private:
    QPixmap render(const QSize& size, QIcon::Mode mode, qreal scale, const QPalette& palette) const
    {
        QColor color = color_.isValid() ? color_
                     : mode == QIcon::Selected ? palette.color(QPalette::HighlightedText)
                                               : palette.color(role_);
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

    QByteArray svg_;
    QColor color_;
    QPalette::ColorRole role_;
};

QIcon tinted(const QString& path, QColor color, QPalette::ColorRole role = QPalette::WindowText)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QIcon(new TintedSvgEngine(file.readAll(), color, role));
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

QIcon lucide(const char* name, QColor color)
{
    return tinted(QStringLiteral(":/icons/lucide/%1.svg").arg(QLatin1String(name)), color);
}

QIcon lucide(const char* name, QPalette::ColorRole role)
{
    return tinted(QStringLiteral(":/icons/lucide/%1.svg").arg(QLatin1String(name)), {}, role);
}

} // namespace icons
