#include "app/Look.h"

#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QProxyStyle>
#include <QSettings>
#include <QStyleHints>
#include <QStyleOption>
#include <QTreeView>

#include <algorithm>
#include <initializer_list>

namespace {

#ifdef _WIN32
QString firstInstalled(std::initializer_list<const char*> families)
{
    for (const char* f : families)
        if (QFontDatabase::hasFamily(QLatin1String(f))) return QLatin1String(f);
    return QLatin1String(*std::prev(families.end()));
}

class WinUiStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    int pixelMetric(PixelMetric metric, const QStyleOption* option, const QWidget* widget) const override
    {
        switch (metric) {
        case PM_LayoutLeftMargin:
        case PM_LayoutTopMargin:
        case PM_LayoutRightMargin:
        case PM_LayoutBottomMargin:
            return qobject_cast<const QDialog*>(widget) ? 24 : 12;
        case PM_LayoutHorizontalSpacing:
        case PM_LayoutVerticalSpacing:
            return 12;
        case PM_ToolBarIconSize:
            return 22; // as KDE; Breeze draws other icons at 24
        case PM_IndicatorWidth:
        case PM_IndicatorHeight:
        case PM_ExclusiveIndicatorWidth:
        case PM_ExclusiveIndicatorHeight:
            return 20;
        case PM_MenuPanelWidth:
            return 1;
        case PM_MenuHMargin:
            return 0;
        case PM_MenuVMargin:
            return 2;
        default:
            return QProxyStyle::pixelMetric(metric, option, widget);
        }
    }

    QSize sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& size, const QWidget* widget) const override
    {
        QSize s = QProxyStyle::sizeFromContents(type, option, size, widget);
        switch (type) {
        case CT_LineEdit:
        case CT_ComboBox:
        case CT_SpinBox:
        case CT_PushButton:
            s.setHeight(std::max(s.height(), 32));
            break;
        case CT_MenuItem: // MenuFlyoutItem: margin 4,2, padding 11,4,11,5, icon and check columns 28
            if (const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option)) {
                if (item->menuItemType == QStyleOptionMenuItem::Separator) return {size.width(), 3};
                int w = size.width() + 2 * (4 + 11);
                if (item->menuHasCheckableItems) w += 28;
                if (item->maxIconWidth > 0) w += 28;
                if (item->text.contains(QLatin1Char('	'))) w += 24;
                if (item->menuItemType == QStyleOptionMenuItem::SubMenu) w += 24 + 12;
                return {w, std::max(size.height() + 9, 28) + 4};
            }
            break;
        case CT_ItemViewItem: // TreeViewItem 28 + 2·2, ListViewItem 40 with 16 + 12 padding
            if (qobject_cast<const QTreeView*>(widget)) {
                s.setHeight(std::max(s.height(), 32));
            } else {
                s.setHeight(std::max(s.height(), 40));
                s.rwidth() += 2 * listShift;
            }
            break;
        default:
            break;
        }
        return s;
    }

    QRect subElementRect(SubElement element, const QStyleOption* option, const QWidget* widget) const override
    {
        QRect r = QProxyStyle::subElementRect(element, option, widget);
        if ((element == SE_ItemViewItemDecoration || element == SE_ItemViewItemText || element == SE_ItemViewItemCheckIndicator) &&
            widget && !qobject_cast<const QTreeView*>(widget))
            r.translate(listShift, 0);
        return r;
    }

    void polish(QWidget* widget) override
    {
        QProxyStyle::polish(widget);
        if (qobject_cast<QMenu*>(widget)) { // rounded by us: DWM doesn't round them here
            widget->setAttribute(Qt::WA_TranslucentBackground);
            widget->setWindowFlag(Qt::FramelessWindowHint);
            widget->setWindowFlag(Qt::NoDropShadowWindowHint);
            widget->installEventFilter(this);
        }
    }

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        // under the menu bar item's highlight, which is inset by 5
        auto* menu = static_cast<QMenu*>(watched);
        if (event->type() == QEvent::Show && qobject_cast<QMenuBar*>(menu->parentWidget())) menu->move(menu->pos() + QPoint(5, 0));
        return false;
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* p, const QWidget* widget) const override
    {
        if (element != PE_PanelMenu || !qobject_cast<const QMenu*>(widget)) return QProxyStyle::drawPrimitive(element, option, p, widget);
        const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(QColor(0, 0, 0, dark ? 0x33 : 0x0f));
        p->setBrush(dark ? QColor(0x2c, 0x2c, 0x2c) : QColor(0xf9, 0xf9, 0xf9));
        p->drawRoundedRect(QRectF(option->rect).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
        p->restore();
    }

    void drawControl(ControlElement element, const QStyleOption* option, QPainter* p, const QWidget* widget) const override
    {
        if (const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option); element == CE_MenuItem && item)
            return drawMenuItem(item, p, widget);
        // checked tool buttons: the icon in the text's colour, on the accent
        const auto* tool = qstyleoption_cast<const QStyleOptionToolButton*>(option);
        if (element != CE_ToolButtonLabel || !tool || !(tool->state & State_On) || !(tool->state & State_Enabled) || tool->icon.isNull())
            return QProxyStyle::drawControl(element, option, p, widget);
        QPixmap pixmap = tool->icon.pixmap(tool->iconSize, p->device()->devicePixelRatio());
        QPainter tint(&pixmap);
        tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tint.fillRect(pixmap.rect(), QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark ? Qt::black : Qt::white);
        tint.end();
        QStyleOptionToolButton label(*tool);
        label.icon = QIcon(pixmap);
        QProxyStyle::drawControl(element, &label, p, widget);
    }

    // group boxes as WinUI cards
    void drawComplexControl(ComplexControl control, const QStyleOptionComplex* option, QPainter* p, const QWidget* widget) const override
    {
        const auto* box = qstyleoption_cast<const QStyleOptionGroupBox*>(option);
        if (control != CC_GroupBox || !box || (box->subControls & SC_GroupBoxCheckBox)) {
            QProxyStyle::drawComplexControl(control, option, p, widget);
            return;
        }
        QColor fill = box->palette.color(QPalette::WindowText), line = fill;
        fill.setAlphaF(0.05);
        line.setAlphaF(0.08);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(line);
        p->setBrush(fill);
        p->drawRoundedRect(QRectF(subControlRect(control, option, SC_GroupBoxFrame, widget)).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
        p->setFont(titleFont(widget));
        proxy()->drawItemText(p, subControlRect(control, option, SC_GroupBoxLabel, widget), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextShowMnemonic,
                              box->palette, box->state & State_Enabled, box->text, QPalette::WindowText);
        p->restore();
    }

    QRect subControlRect(ComplexControl control, const QStyleOptionComplex* option, SubControl sc, const QWidget* widget) const override
    {
        const auto* box = qstyleoption_cast<const QStyleOptionGroupBox*>(option);
        if (control != CC_GroupBox || !box || (box->subControls & SC_GroupBoxCheckBox)) return QProxyStyle::subControlRect(control, option, sc, widget);
        const int title = box->text.isEmpty() ? 0 : QFontMetrics(titleFont(widget)).height();
        const QRect frame = box->rect.adjusted(0, title ? title + 8 : 0, 0, 0);
        switch (sc) {
        case SC_GroupBoxLabel:
            return {box->rect.topLeft(), QSize(box->rect.width(), title)};
        case SC_GroupBoxFrame:
            return frame;
        case SC_GroupBoxContents:
            return frame.adjusted(4, 4, -4, -4);
        default:
            return QProxyStyle::subControlRect(control, option, sc, widget);
        }
    }

private:
    void drawMenuItem(const QStyleOptionMenuItem* item, QPainter* p, const QWidget* widget) const
    {
        const QRect r = item->rect;
        QColor text = item->palette.color(QPalette::Text);
        p->save();
        if (item->menuItemType == QStyleOptionMenuItem::Separator) {
            text.setAlphaF(0.08);
            p->fillRect(r.left(), r.top() + 1, r.width(), 1, text);
            return p->restore();
        }
        const bool enabled = item->state & State_Enabled;
        p->setRenderHint(QPainter::Antialiasing);
        if (enabled && (item->state & State_Selected)) {
            QColor hover = text;
            hover.setAlphaF(0.06);
            p->setPen(Qt::NoPen);
            p->setBrush(hover);
            p->drawRoundedRect(QRectF(r.adjusted(4, 2, -4, -2)), 4, 4);
        }
        if (!enabled) text.setAlphaF(0.36);
        QFont glyphs(firstInstalled({"Segoe Fluent Icons", "Segoe MDL2 Assets"}));
        glyphs.setPixelSize(12);
        int x = r.left() + 15;
        if (item->menuHasCheckableItems) {
            if (item->checked && item->checkType == QStyleOptionMenuItem::Exclusive) {
                p->setPen(Qt::NoPen);
                p->setBrush(text);
                p->drawEllipse(QPointF(x + 8, r.center().y() + 0.5), 3, 3);
            } else if (item->checked) {
                p->setFont(glyphs);
                p->setPen(text);
                p->drawText(QRect(x, r.top(), 16, r.height()), Qt::AlignCenter, QStringLiteral(u"")); // CheckMark
            }
            x += 28;
        }
        if (item->maxIconWidth > 0) {
            item->icon.paint(p, QRect(x, r.center().y() - 7, 16, 16), Qt::AlignCenter, enabled ? QIcon::Normal : QIcon::Disabled,
                             item->checked ? QIcon::On : QIcon::Off);
            x += 28;
        }
        const QRect textRect(x, r.top(), r.right() - 15 - x, r.height());
        int flags = Qt::AlignVCenter | Qt::TextShowMnemonic | Qt::TextSingleLine;
        if (!proxy()->styleHint(SH_UnderlineShortcut, item, widget)) flags |= Qt::TextHideMnemonic;
        const qsizetype tab = item->text.indexOf(QLatin1Char('	'));
        p->setFont(item->font);
        p->setPen(text);
        p->drawText(textRect, flags | Qt::AlignLeft, item->text.left(tab));
        if (item->menuItemType == QStyleOptionMenuItem::SubMenu) {
            p->setFont(glyphs);
            p->drawText(textRect, Qt::AlignVCenter | Qt::AlignRight, QStringLiteral(u"")); // ChevronRight
        } else if (tab >= 0) {
            QColor secondary = text;
            secondary.setAlphaF(text.alphaF() * 0.7);
            p->setPen(secondary);
            p->drawText(textRect, flags | Qt::AlignRight, item->text.mid(tab + 1));
        }
        p->restore();
    }

    static constexpr int listShift = 12; // Qt's 4 px item margin to WinUI's 16

    static QFont titleFont(const QWidget* widget)
    {
        QFont font = widget ? widget->font() : QApplication::font();
        font.setWeight(QFont::DemiBold);
        return font;
    }
};
#endif

} // namespace

namespace look {

QFont systemFont(QFontDatabase::SystemFont kind)
{
    QFont font = QFontDatabase::systemFont(kind);
#ifdef _WIN32
    if (kind != QFontDatabase::GeneralFont && kind != QFontDatabase::FixedFont) return font;
    font.setFamily(kind == QFontDatabase::FixedFont ? firstInstalled({"Cascadia Mono", "Consolas"})
                                                    : firstInstalled({"Segoe UI Variable Text", "Segoe UI Variable", "Segoe UI"}));
    // Accessibility > Text size
    const int scale = QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Accessibility"), QSettings::NativeFormat)
                          .value(QStringLiteral("TextScaleFactor"), 100).toInt();
    font.setPointSize(qRound(10 * std::clamp(scale, 100, 225) / 100.0));
#endif
    return font;
}

void apply()
{
#ifdef _WIN32
    QApplication::setStyle(new WinUiStyle(QApplication::style()->name()));
    QApplication::setFont(systemFont(QFontDatabase::GeneralFont));
#endif
}

} // namespace look
