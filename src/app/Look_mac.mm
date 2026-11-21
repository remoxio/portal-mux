#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QFocusFrame>
#include <QFormLayout>
#include <QGroupBox>
#include <QIcon>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QStyleHints>
#include <QStyleOption>
#include <QTabBar>
#include <QTabWidget>
#include <QTranslator>

#import <AppKit/AppKit.h>

#include <algorithm>

namespace {

// as macOS 26 draws them
class MacStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget, QStyleHintReturn* data) const override
    {
        if (hint == SH_FormLayoutFieldGrowthPolicy) return QFormLayout::ExpandingFieldsGrow; // text fields to the edge, pop-ups fit
        return QProxyStyle::styleHint(hint, option, widget, data);
    }

    int pixelMetric(PixelMetric metric, const QStyleOption* option, const QWidget* widget) const override
    {
        if (metric == PM_ToolBarIconSize) return 22; // Breeze's, as on KDE and Windows
        if (metric == PM_TabBarTabOverlap && tabView(widget)) return 0;
        if (metric == PM_TabBarBaseOverlap && tabView(widget)) return tabHeight / 2; // the bar sits on the pane's edge
        if ((metric == PM_LayoutLeftMargin || metric == PM_LayoutTopMargin || metric == PM_LayoutRightMargin || metric == PM_LayoutBottomMargin) &&
            card(widget))
            return 0; // the card's own padding
        return QProxyStyle::pixelMetric(metric, option, widget);
    }

    int layoutSpacing(QSizePolicy::ControlType control1, QSizePolicy::ControlType control2, Qt::Orientation orientation,
                      const QStyleOption* option, const QWidget* widget) const override
    {
        if (orientation != Qt::Vertical || control2 == QSizePolicy::ButtonBox)
            return QProxyStyle::layoutSpacing(control1, control2, orientation, option, widget);
        const auto checks = QSizePolicy::CheckBox | QSizePolicy::RadioButton;
        return (control1 & checks) && (control2 & checks) ? 8 : 12;
    }

    QSize sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& size, const QWidget* widget) const override
    {
        if (const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option); type == CT_TabBarTab && tab && tabView(widget))
            return {option->fontMetrics.size(Qt::TextShowMnemonic, tab->text).width() + 24, tabHeight};
        QSize s = QProxyStyle::sizeFromContents(type, option, size, widget);
        if (type == CT_SpinBox) s.rwidth() += 12; // Qt's is a few points short of its text
        if (type == CT_LineEdit || type == CT_SpinBox) s.setHeight(std::max(s.height(), 24));
        return s;
    }

    void drawControl(ControlElement element, const QStyleOption* option, QPainter* p, const QWidget* widget) const override
    {
        if (const auto* frame = qobject_cast<const QFocusFrame*>(widget); element == CE_FocusFrame && frame) {
            // Qt's ring has smaller corners than the text field
            const int h = proxy()->pixelMetric(PM_FocusFrameHMargin, option, widget), v = proxy()->pixelMetric(PM_FocusFrameVMargin, option, widget);
            QRectF field;
            if (const auto* spin = qobject_cast<const QAbstractSpinBox*>(frame->widget())) {
                QStyleOptionSpinBox box;
                box.initFrom(spin);
                box.rect = spin->rect();
                box.buttonSymbols = spin->buttonSymbols();
                field = proxy()->subControlRect(CC_SpinBox, &box, SC_SpinBoxEditField, spin).translated(h, v);
            } else if (qobject_cast<const QLineEdit*>(frame->widget())) {
                field = QRectF(option->rect).adjusted(h, v, -h, -v);
            }
            if (!field.isEmpty()) return drawFieldRing(field.adjusted(1, 1, -1, -1), p); // where Qt draws the bezel
        }
        const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option);
        if (!tab || !tabView(widget) || (element != CE_TabBarTabShape && element != CE_TabBarTabLabel))
            return QProxyStyle::drawControl(element, option, p, widget);
        if (element == CE_TabBarTabLabel) {
            int flags = Qt::AlignCenter | Qt::TextShowMnemonic;
            if (!proxy()->styleHint(SH_UnderlineShortcut, tab, widget)) flags |= Qt::TextHideMnemonic;
            proxy()->drawItemText(p, tab->rect, flags, tab->palette, tab->state & State_Enabled, tab->text, QPalette::WindowText);
            return;
        }
        // a segment: rounded at the bar's ends, the selected one darker
        const QRectF r = tab->rect;
        const bool first = tab->position == QStyleOptionTab::Beginning || tab->position == QStyleOptionTab::OnlyOneTab;
        const bool last = tab->position == QStyleOptionTab::End || tab->position == QStyleOptionTab::OnlyOneTab;
        QColor ink = tab->palette.color(QPalette::WindowText);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        p->setClipRect(r);
        ink.setAlphaF(0.08);
        p->setBrush(ink);
        p->drawRoundedRect(r.adjusted(first ? 0 : -tabRadius, 0, last ? 0 : tabRadius, 0), tabRadius, tabRadius);
        if (tab->state & State_Selected) {
            ink.setAlphaF(0.2);
            p->setBrush(ink);
            p->drawRoundedRect(r, tabRadius, tabRadius);
        } else if (!last && tab->selectedPosition != QStyleOptionTab::NextIsSelected) {
            ink.setAlphaF(0.17);
            p->fillRect(QRectF(r.right() - 1, r.center().y() - 6.5, 1, 13), ink);
        }
        p->restore();
    }

    // group boxes as cards: a small title over a rounded box
    void drawComplexControl(ComplexControl control, const QStyleOptionComplex* option, QPainter* p, const QWidget* widget) const override
    {
        const auto* box = qstyleoption_cast<const QStyleOptionGroupBox*>(option);
        if (control != CC_GroupBox || !box || !card(widget)) return QProxyStyle::drawComplexControl(control, option, p, widget);
        QColor ink = box->palette.color(QPalette::WindowText);
        ink.setAlphaF(0.03);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        p->setBrush(ink);
        p->drawRoundedRect(QRectF(subControlRect(control, option, SC_GroupBoxFrame, widget)), 12, 12);
        p->setFont(titleFont(widget));
        proxy()->drawItemText(p, subControlRect(control, option, SC_GroupBoxLabel, widget), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextHideMnemonic,
                              box->palette, box->state & State_Enabled, box->text, QPalette::WindowText);
        p->restore();
    }

    QRect subElementRect(SubElement element, const QStyleOption* option, const QWidget* widget) const override
    {
        if (element == SE_GroupBoxLayoutItem && card(widget)) return option->rect; // Qt's leaves the title out, so it crowds what is above
        return QProxyStyle::subElementRect(element, option, widget);
    }

    QRect subControlRect(ComplexControl control, const QStyleOptionComplex* option, SubControl sc, const QWidget* widget) const override
    {
        const auto* box = qstyleoption_cast<const QStyleOptionGroupBox*>(option);
        if (control != CC_GroupBox || !box || !card(widget)) return QProxyStyle::subControlRect(control, option, sc, widget);
        const int title = box->text.isEmpty() ? 0 : QFontMetrics(titleFont(widget)).height();
        const QRect frame = box->rect.adjusted(0, title ? title + 6 : 0, 0, 0);
        switch (sc) {
        case SC_GroupBoxLabel:
            return QRect(box->rect.left() + 10, box->rect.top(), box->rect.width() - 10, title);
        case SC_GroupBoxFrame:
            return frame;
        case SC_GroupBoxContents:
            return frame.adjusted(16, 16, -16, -16);
        default:
            return QProxyStyle::subControlRect(control, option, sc, widget);
        }
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* p, const QWidget* widget) const override
    {
        if (element != PE_FrameTabWidget || !tabView(widget)) return QProxyStyle::drawPrimitive(element, option, p, widget);
        QColor ink = option->palette.color(QPalette::WindowText);
        const bool dark = ink.lightness() > 128;
        const qreal hairline = 1 / p->device()->devicePixelRatioF();
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        ink.setAlphaF(dark ? 0.15 : 0.06);
        p->setPen(QPen(ink, hairline));
        ink.setAlphaF(0.03);
        p->setBrush(ink);
        const qreal inset = hairline / 2;
        p->drawRoundedRect(QRectF(option->rect).adjusted(inset, inset, -inset, -inset), 5, 5);
        p->restore();
    }

    void polish(QWidget* widget) override
    {
        QProxyStyle::polish(widget);
        if (auto* combo = qobject_cast<QComboBox*>(widget); combo && combo->isEditable()) // a text field with a list
            combo->setSizePolicy(QSizePolicy::Expanding, combo->sizePolicy().verticalPolicy());
    }

    void polish(QPalette& palette) override
    {
        QProxyStyle::polish(palette);
        // AppKit's text background: Qt's dark one differs, and its text fields lose their bezel
        __block NSColor* base = nil;
        [NSApp.effectiveAppearance performAsCurrentDrawingAppearance:^{
            base = [NSColor.textBackgroundColor colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
        }];
        if (base) palette.setColor(QPalette::Base, QColor::fromRgbF(base.redComponent, base.greenComponent, base.blueComponent, base.alphaComponent));
    }

    using QProxyStyle::polish;

    bool event(QEvent* event) override
    {
        const bool done = QProxyStyle::event(event);
        // Qt rings a spin box's inner line edit, inside the spin box's clip: ring the spin box
        if (auto* spin = qobject_cast<QAbstractSpinBox*>(QApplication::focusWidget()); spin && event->type() == QEvent::FocusIn)
            for (QFocusFrame* frame : spin->window()->findChildren<QFocusFrame*>())
                if (frame->widget() && frame->widget()->parentWidget() == spin) frame->setWidget(spin);
        return done;
    }

private:
    static void drawFieldRing(const QRectF& field, QPainter* p)
    {
        QPainterPath ring;
        ring.setFillRule(Qt::OddEvenFill);
        ring.addRoundedRect(field, fieldRadius, fieldRadius);
        ring.addRoundedRect(field.adjusted(-ringWidth, -ringWidth, ringWidth, ringWidth), fieldRadius + ringWidth, fieldRadius + ringWidth);
        __block NSColor* color = nil;
        [NSApp.effectiveAppearance performAsCurrentDrawingAppearance:^{
            color = [NSColor.keyboardFocusIndicatorColor colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
        }];
        if (!color) return;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->fillPath(ring, QColor::fromRgbF(color.redComponent, color.greenComponent, color.blueComponent, color.alphaComponent));
        p->restore();
    }

    static constexpr int tabHeight = 24;
    static constexpr qreal tabRadius = 7, fieldRadius = 6.5, ringWidth = 3.5;

    static bool card(const QWidget* widget)
    {
        const auto* box = qobject_cast<const QGroupBox*>(widget);
        return box && !box->isCheckable() && !box->isFlat();
    }

    static QFont titleFont(const QWidget* widget)
    {
        QFont font = widget ? widget->font() : QApplication::font();
        font.setPointSizeF(font.pointSizeF() - 1);
        return font;
    }

    // a QTabWidget's tabs on top: AppKit's tab view; document mode stays Qt's
    static bool tabView(const QWidget* widget)
    {
        if (const auto* bar = qobject_cast<const QTabBar*>(widget)) return !bar->documentMode() && bar->shape() == QTabBar::RoundedNorth;
        if (const auto* tabs = qobject_cast<const QTabWidget*>(widget)) return !tabs->documentMode() && tabs->tabPosition() == QTabWidget::North;
        return false;
    }
};

// macOS 13 renamed it; Qt's app menu still says "Preferences..."
class AppMenuText : public QTranslator {
public:
    using QTranslator::QTranslator;

    QString translate(const char* context, const char* source, const char*, int) const override
    {
        return qstrcmp(context, "MAC_APPLICATION_MENU") == 0 && qstrcmp(source, "Preferences...") == 0 ? QStringLiteral("Settings…") : QString();
    }

    bool isEmpty() const override { return false; }
};

} // namespace

namespace look {

void applyMac()
{
    QApplication::setStyle(new MacStyle(QApplication::style()->name()));
    QCoreApplication::installTranslator(new AppMenuText(qApp));
    // bundled Breeze, as on the other systems
    QIcon::setThemeSearchPaths({QStringLiteral(":/icons")});
    auto follow = [] {
        QIcon::setThemeName(QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark ? QStringLiteral("breeze-dark")
                                                                                                   : QStringLiteral("breeze"));
    };
    follow();
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, qApp, follow);
}

} // namespace look
