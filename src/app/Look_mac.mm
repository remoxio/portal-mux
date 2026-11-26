#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QCursor>
#include <QFocusFrame>
#include <QFormLayout>
#include <QGroupBox>
#include <QIcon>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QStyleHints>
#include <QStyleOption>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>
#include <QTranslator>

#import <AppKit/AppKit.h>

#include <algorithm>
#include <utility>

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
        if (capsules(widget)) {
            if (metric == PM_ToolBarIconSize) return 18;
            if (metric == PM_ToolBarSeparatorExtent) return 8;
            if (metric == PM_ToolBarItemSpacing || metric == PM_ToolBarItemMargin || metric == PM_ToolBarFrameWidth) return 0;
        }
        if (metric == PM_ToolBarIconSize) return 22; // Breeze's, as on KDE and Windows
        if (metric == PM_TabBarTabOverlap && (tabView(widget) || pills(widget))) return 0;
        if ((metric == PM_TabCloseIndicatorWidth || metric == PM_TabCloseIndicatorHeight) && widget && pills(widget->parentWidget())) return 16;
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
        if (const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option); type == CT_TabBarTab && tab && pills(widget))
            return {std::max(size.width() + tab->leftButtonSize.width() + tab->rightButtonSize.width() + 16, 100), pillTabHeight};
        if (const auto* tool = qstyleoption_cast<const QStyleOptionToolButton*>(option);
            type == CT_ToolButton && tool && widget && capsules(widget->parentWidget())) {
            const int width = capsuleContent(tool) + (tool->features & QStyleOptionToolButton::HasMenu ? 10 : 0);
            if (tool->toolButtonStyle != Qt::ToolButtonTextUnderIcon) return {width, capsuleHeight};
            const QFontMetrics label(labelFont(widget));
            return {std::max(width, label.horizontalAdvance(tool->text) + 10), capsuleHeight + 3 + label.height()};
        }
        QSize s = QProxyStyle::sizeFromContents(type, option, size, widget);
        if (type == CT_SpinBox) s.rwidth() += 12; // Qt's is a few points short of its text
        if (type == CT_LineEdit || type == CT_SpinBox) s.setHeight(std::max(s.height(), 24));
        return s;
    }

    void drawControl(ControlElement element, const QStyleOption* option, QPainter* p, const QWidget* widget) const override
    {
        if (element == CE_ToolBar && capsules(widget)) return drawCapsules(static_cast<const QToolBar*>(widget), p);
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
        if (tab && pills(widget) && (element == CE_TabBarTabShape || element == CE_TabBarTabLabel)) return drawPill(element, tab, p, widget);
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

    // a session tab: the current one a raised pill, the others only dimmer text
    void drawPill(ControlElement element, const QStyleOptionTab* tab, QPainter* p, const QWidget* widget) const
    {
        QColor ink = tab->palette.color(QPalette::WindowText);
        const bool dark = ink.lightness() > 128, current = tab->state & State_Selected;
        if (element == CE_TabBarTabLabel) {
            QStyleOptionTab label = *tab;
            if (!current) ink.setAlphaF(0.6);
            label.palette.setColor(QPalette::WindowText, ink);
            return QCommonStyle::drawControl(element, &label, p, widget); // AppKit's makes the current one white
        }
        const QRectF r = QRectF(tab->rect).adjusted(2, 2, -2, -2);
        const qreal radius = r.height() / 2, hairline = 1 / p->device()->devicePixelRatioF();
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        if (current) {
            ink.setAlphaF(dark ? 0.3 : 0.12);
            p->setPen(QPen(ink, hairline));
            QColor fill = tab->palette.color(QPalette::WindowText);
            fill.setAlphaF(0.19);
            p->setBrush(dark ? fill : QColor(Qt::white));
            p->drawRoundedRect(r.adjusted(hairline / 2, hairline / 2, -hairline / 2, -hairline / 2), radius, radius);
        } else if (tab->state & State_MouseOver && tab->state & State_Enabled) {
            ink.setAlphaF(0.06);
            p->setPen(Qt::NoPen);
            p->setBrush(ink);
            p->drawRoundedRect(r, radius, radius);
        }
        p->restore();
    }

    // a session tab's ✕: only while the pointer is on its tab
    static void drawPillClose(const QStyleOption* option, QPainter* p, const QWidget* button)
    {
        const auto* bar = static_cast<const QTabBar*>(button->parentWidget());
        const int index = bar->tabAt(button->geometry().center());
        if (!(option->state & State_MouseOver) && (!bar->underMouse() || bar->tabAt(bar->mapFromGlobal(QCursor::pos())) != index)) return;
        QColor ink = option->palette.color(QPalette::WindowText);
        const QRectF r = QRectF(option->rect).adjusted(0.5, 0.5, -0.5, -0.5);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        if (option->state & State_MouseOver) {
            ink.setAlphaF(0.12);
            p->setPen(Qt::NoPen);
            p->setBrush(ink);
            p->drawEllipse(r);
        }
        ink.setAlphaF(option->state & State_MouseOver ? 1 : 0.75);
        p->setPen(QPen(ink, 1.2, Qt::SolidLine, Qt::RoundCap));
        const qreal half = 3;
        const QPointF c = r.center();
        p->drawLine(c + QPointF(-half, -half), c + QPointF(half, half));
        p->drawLine(c + QPointF(-half, half), c + QPointF(half, -half));
        p->restore();
    }

    // a capsule behind each run of buttons between separators
    static void drawCapsules(const QToolBar* bar, QPainter* p)
    {
        QColor ink = bar->palette().color(QPalette::WindowText);
        const bool dark = ink.lightness() > 128;
        const qreal hairline = 1 / p->device()->devicePixelRatioF();
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        auto capsule = [&](const QRect& run) {
            if (run.isNull()) return;
            const QRectF r = QRectF(run).adjusted(hairline / 2, hairline / 2, -hairline / 2, -hairline / 2);
            QColor fill = ink;
            fill.setAlphaF(0.06);
            p->setBrush(dark ? fill : QColor(Qt::white));
            QColor edge = ink;
            edge.setAlphaF(dark ? 0.19 : 0.12);
            p->setPen(QPen(edge, hairline));
            p->drawRoundedRect(r, r.height() / 2, r.height() / 2);
        };
        QRect run;
        for (QAction* action : bar->actions()) {
            if (action->isSeparator()) capsule(std::exchange(run, QRect()));
            else if (QWidget* button = bar->widgetForAction(action); button && button->isVisible())
                run |= button->geometry().adjusted(0, 0, 0, std::min(0, capsuleHeight - button->height())); // not its label
        }
        capsule(run);
        p->restore();
    }

    // group boxes as cards: a small title over a rounded box
    void drawComplexControl(ComplexControl control, const QStyleOptionComplex* option, QPainter* p, const QWidget* widget) const override
    {
        if (const auto* spin = qstyleoption_cast<const QStyleOptionSpinBox*>(option);
            control == CC_SpinBox && spin && spin->frame && (spin->subControls & SC_SpinBoxFrame)) {
            drawField(QRectF(proxy()->subControlRect(CC_SpinBox, spin, SC_SpinBoxEditField, widget)).adjusted(1, 1, -1, -1), option, p);
            QStyleOptionSpinBox stepper = *spin;
            stepper.subControls &= ~SC_SpinBoxFrame;
            return QProxyStyle::drawComplexControl(control, &stepper, p, widget);
        }
        // a flat button outside a toolbar: its icon only, a faint fill under the pointer, as AppKit's accessory buttons
        if (const auto* tool = qstyleoption_cast<const QStyleOptionToolButton*>(option);
            control == CC_ToolButton && tool && (tool->state & State_AutoRaise) && widget && !qobject_cast<const QToolBar*>(widget->parentWidget())) {
            if ((tool->state & State_Enabled) && (tool->state & (State_MouseOver | State_Sunken | State_On))) {
                QColor fill = tool->palette.color(QPalette::Active, QPalette::WindowText);
                fill.setAlphaF(tool->state & (State_Sunken | State_On) ? 0.16 : 0.08);
                p->save();
                p->setRenderHint(QPainter::Antialiasing);
                p->setPen(Qt::NoPen);
                p->setBrush(fill);
                p->drawRoundedRect(QRectF(tool->rect).adjusted(1, 1, -1, -1), 5, 5);
                p->restore();
            }
            QStyleOptionToolButton label = *tool;
            label.state &= ~(State_Sunken | State_On); // no pressed offset
            return proxy()->drawControl(CE_ToolButtonLabel, &label, p, widget);
        }
        // a button in a capsule: a round highlight, not AppKit's bezel
        if (const auto* tool = qstyleoption_cast<const QStyleOptionToolButton*>(option);
            control == CC_ToolButton && tool && widget && capsules(widget->parentWidget())) {
            QRect area = tool->rect; // in the capsule; a label goes under it
            area.setHeight(capsuleHeight);
            QStyleOptionToolButton plain = *tool;
            if (plain.toolButtonStyle != Qt::ToolButtonTextOnly) plain.toolButtonStyle = Qt::ToolButtonIconOnly;
            const bool menu = tool->features & QStyleOptionToolButton::HasMenu && !(tool->features & QStyleOptionToolButton::MenuButtonPopup);
            plain.rect = QRect(0, 0, std::min(area.width(), capsuleContent(tool) + (menu ? 10 : 0)), capsuleHeight);
            plain.rect.moveCenter(area.center());
            const bool lit = tool->state & (State_On | State_Sunken), hover = tool->state & State_MouseOver && tool->state & State_Enabled;
            if (lit || hover) {
                QColor ink = tool->palette.color(QPalette::WindowText);
                ink.setAlphaF(lit ? 0.14 : 0.07);
                p->save();
                p->setRenderHint(QPainter::Antialiasing);
                p->setPen(Qt::NoPen);
                p->setBrush(ink);
                const QRectF r = QRectF(area).adjusted(2, 2, -2, -2);
                p->drawRoundedRect(r, r.height() / 2, r.height() / 2);
                p->restore();
            }
            plain.state &= ~(State_On | State_Sunken | State_MouseOver | State_Raised);
            if (tool->toolButtonStyle == Qt::ToolButtonTextUnderIcon) {
                const QFont font = labelFont(widget);
                const QRect label(tool->rect.left(), area.bottom() + 4, tool->rect.width(), tool->rect.bottom() - area.bottom() - 3);
                p->save();
                p->setFont(font);
                proxy()->drawItemText(p, label, Qt::AlignHCenter | Qt::AlignTop | Qt::TextHideMnemonic, tool->palette, tool->state & State_Enabled,
                                      QFontMetrics(font).elidedText(tool->text, Qt::ElideRight, label.width()), QPalette::WindowText);
                p->restore();
            }
            if (menu) {
                // a chevron beside the icon, not Qt's arrow in the corner
                const QPointF c(plain.rect.right() - 9, QRectF(area).center().y());
                plain.features &= ~QStyleOptionToolButton::HasMenu;
                plain.rect.setRight(plain.rect.right() - 10);
                QColor ink = tool->palette.color(tool->state & State_Enabled ? QPalette::Active : QPalette::Disabled, QPalette::WindowText);
                ink.setAlphaF(ink.alphaF() * 0.75);
                p->save();
                p->setRenderHint(QPainter::Antialiasing);
                p->setPen(QPen(ink, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p->drawPolyline(QList<QPointF>{c + QPointF(-3, -1.5), c + QPointF(0, 1.5), c + QPointF(3, -1.5)});
                p->restore();
            }
            if (tool->toolButtonStyle == Qt::ToolButtonTextOnly)
                return proxy()->drawItemText(p, plain.rect, Qt::AlignCenter | Qt::TextHideMnemonic, tool->palette, tool->state & State_Enabled, tool->text,
                                             QPalette::WindowText);
            return QProxyStyle::drawComplexControl(control, &plain, p, widget);
        }
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
        if (const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option); element == SE_TabBarTabText && tab && pills(widget) && tab->icon.isNull()) {
            const int side = std::max(tab->leftButtonSize.width(), tab->rightButtonSize.width()) + 12; // centred, clear of the close button
            return tab->rect.adjusted(side, 0, -side, 0);
        }
        if (const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option);
            (element == SE_TabBarTabLeftButton || element == SE_TabBarTabRightButton) && tab && pills(widget)) {
            // in the pill's round end, as Terminal's
            const QSize size = element == SE_TabBarTabLeftButton ? tab->leftButtonSize : tab->rightButtonSize;
            const int x = element == SE_TabBarTabLeftButton ? tab->rect.left() + 15 : tab->rect.right() - 14;
            return QRect(QPoint(x - size.width() / 2, tab->rect.center().y() + 1 - size.height() / 2), size);
        }
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
        if (element == PE_IndicatorTabClose && widget && pills(widget->parentWidget())) return drawPillClose(option, p, widget);
        if (element == PE_IndicatorToolBarSeparator && capsules(widget)) return; // the gap between capsules
        if (const auto* frame = qstyleoption_cast<const QStyleOptionFrame*>(option); element == PE_PanelLineEdit && frame) {
            if (frame->lineWidth > 0) return drawField(QRectF(option->rect).adjusted(1, 1, -1, -1), option, p);
            if (widget && qobject_cast<const QAbstractSpinBox*>(widget->parentWidget())) return; // on the spin box's field
        }
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
    // filled and outlined also unfocused, where AppKit's dark ones show only a faint outline
    static void drawField(const QRectF& field, const QStyleOption* option, QPainter* p)
    {
        QColor ink = option->palette.color(QPalette::Active, QPalette::WindowText);
        const qreal fade = option->state & State_Enabled ? 1 : 0.6;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        ink.setAlphaF(0.11 * fade);
        p->setPen(QPen(ink, 1));
        ink.setAlphaF(0.02 * fade);
        p->setBrush(ink);
        p->drawRoundedRect(field.adjusted(0.5, 0.5, -0.5, -0.5), fieldRadius - 0.5, fieldRadius - 0.5);
        p->restore();
    }

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

    static constexpr int tabHeight = 24, pillTabHeight = 28, capsuleHeight = 30;
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

    static bool pills(const QWidget* widget) { return widget && widget->property("pills").toBool(); } // SessionTabs.h
    static bool capsules(const QWidget* widget) { return widget && widget->property("capsules").toBool(); } // MainWindow.cpp

    static int capsuleContent(const QStyleOptionToolButton* tool) // a button's part of its capsule
    {
        return tool->toolButtonStyle == Qt::ToolButtonTextOnly ? tool->fontMetrics.horizontalAdvance(tool->text) + 20 : tool->iconSize.width() + 16;
    }

    static QFont labelFont(const QWidget* widget) // a toolbar's labels: 11 pt at the system's 13
    {
        QFont font = widget->font();
        font.setPointSizeF(font.pointSizeF() * 11 / 13);
        return font;
    }

    // a QTabWidget's tabs on top: AppKit's tab view; document mode stays Qt's
    static bool tabView(const QWidget* widget)
    {
        if (const auto* bar = qobject_cast<const QTabBar*>(widget)) return !bar->documentMode() && bar->shape() == QTabBar::RoundedNorth && !pills(bar);
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

void dialogTitleBar(QWidget* dialog)
{
    NSWindow* window = reinterpret_cast<NSView*>(dialog->winId()).window;
    [window standardWindowButton:NSWindowMiniaturizeButton].hidden = YES; // Qt only disables them
    [window standardWindowButton:NSWindowZoomButton].hidden = YES;
    window.titlebarAppearsTransparent = YES; // no line under it
}

void systemMenus(QMenu* windows, QMenu* help)
{
    NSApp.windowsMenu = windows->toNSMenu();
    NSApp.helpMenu = help->toNSMenu();
}

} // namespace look
