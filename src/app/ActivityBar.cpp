#include "app/ActivityBar.h"

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOptionFocusRect>
#include <QStylePainter>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int Width = 48, IconSize = 24;

class ActivityButton : public QToolButton {
public:
    bool lit = false;

    explicit ActivityButton(QWidget* parent) : QToolButton(parent)
    {
        setFixedSize(Width, Width);
        setIconSize(QSize(IconSize, IconSize));
        setAutoRaise(true);
        setFocusPolicy(Qt::TabFocus);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QStylePainter p(this);
        p.setOpacity(lit || underMouse() ? 1.0 : 0.55);
        icon().paint(&p, QRect((width() - IconSize) / 2, (height() - IconSize) / 2, IconSize, IconSize));
        p.setOpacity(1.0);
        if (lit) p.fillRect(QRect(0, 0, 2, height()), palette().color(QPalette::Highlight));
        if (hasFocus()) {
            QStyleOptionFocusRect focus;
            focus.initFrom(this);
            p.drawPrimitive(QStyle::PE_FrameFocusRect, focus);
        }
    }
};

} // namespace

ActivityBar::ActivityBar(QWidget* parent) : QWidget(parent), layout_(new QVBoxLayout(this))
{
    setFixedWidth(Width);
    setContextMenuPolicy(Qt::ActionsContextMenu);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(0);
    layout_->addStretch();
}

void ActivityBar::addView(const QIcon& icon, const QString& name)
{
    auto* button = new ActivityButton(this);
    button->setIcon(icon);
    button->setToolTip(name);
    button->setAccessibleName(name);
    button->installEventFilter(this);
    const int index = int(buttons_.size());
    connect(button, &QToolButton::clicked, this, [this, index] { emit clicked(index); });
    layout_->insertWidget(index, button); // before the stretch
    buttons_.push_back(button);
}

void ActivityBar::setViewHidden(int index, bool hidden) { buttons_[size_t(index)]->setVisible(!hidden); }

void ActivityBar::setCurrent(int index)
{
    for (size_t i = 0; i < buttons_.size(); ++i) {
        static_cast<ActivityButton*>(buttons_[i])->lit = int(i) == index;
        buttons_[i]->update();
    }
}

bool ActivityBar::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonDblClick && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        const auto it = std::find(buttons_.begin(), buttons_.end(), watched);
        if (it != buttons_.end()) {
            emit doubleClicked(int(it - buttons_.begin()));
            return true; // not a second click
        }
    }
    return QWidget::eventFilter(watched, event);
}
