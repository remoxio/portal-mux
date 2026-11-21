#pragma once

#include <QApplication>
#include <QEvent>
#include <QPainter>
#include <QStackedWidget>
#include <QTabBar>
#include <QVBoxLayout>

// Breeze's 30 px tabs are lower than the desktop's other tab bars: 34
class TallTabBar : public QTabBar {
protected:
#ifndef Q_OS_MACOS
    QSize tabSizeHint(int index) const override { return QTabBar::tabSizeHint(index) + QSize(0, 4); }
#endif
};

#ifdef Q_OS_MACOS
// macOS 26's tab bar: the tabs as pills (Look_mac.mm) in a rounded track across the window
class TabTrack : public QWidget {
public:
    explicit TabTrack(QTabBar* bar)
    {
        setContentsMargins(8, 6, 8, 6);
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(bar);
        layout->addStretch();
        bar->setProperty("pills", true);
        bar->setDrawBase(false);
        bar->setAttribute(Qt::WA_Hover);
        bar->setFont(QApplication::font());
        bar->installEventFilter(this); // hidden with it
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::ShowToParent) show();
        if (event->type() == QEvent::HideToParent) hide();
        if (event->type() == QEvent::ApplicationFontChange) static_cast<QWidget*>(watched)->setFont(QApplication::font());
        return false;
    }
    void paintEvent(QPaintEvent*) override
    {
        QColor ink = palette().color(QPalette::WindowText);
        ink.setAlphaF(ink.lightness() > 128 ? 0.065 : 0.05);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        const QRectF r = contentsRect();
        p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
    }
};
#endif

// QTabWidget's API, with the session's toolbar between the tabs and the pages
class SessionTabs : public QWidget {
    Q_OBJECT

public:
    explicit SessionTabs(QWidget* parent = nullptr) : QWidget(parent), bar_(new TallTabBar), stack_(new QStackedWidget)
    {
        layout_ = new QVBoxLayout(this);
        layout_->setContentsMargins(0, 0, 0, 0);
        layout_->setSpacing(0);
        layout_->addWidget(stack_, 1);
#ifdef Q_OS_MACOS
        layout_->insertWidget(0, new TabTrack(bar_)); // not in document mode: AppKit's is grey, in a small font
#else
        layout_->insertWidget(0, bar_);
        bar_->setDocumentMode(true);
        bar_->setDrawBase(true);
#endif
        bar_->setExpanding(false);
        connect(bar_, &QTabBar::currentChanged, this, [this](int index) {
            if (index >= 0 && index < stack_->count()) stack_->setCurrentIndex(index);
            emit currentChanged(index);
        });
        connect(bar_, &QTabBar::tabCloseRequested, this, &SessionTabs::tabCloseRequested);
        connect(bar_, &QTabBar::tabMoved, this, [this](int from, int to) {
            const QSignalBlocker blocker(stack_);
            QWidget* page = stack_->widget(from);
            stack_->removeWidget(page);
            stack_->insertWidget(to, page);
            stack_->setCurrentIndex(bar_->currentIndex());
        });
        connect(stack_, &QStackedWidget::widgetRemoved, bar_, &QTabBar::removeTab); // also a page deleted
    }

    void setToolBar(QWidget* toolBar) { layout_->insertWidget(1, toolBar); }
    QTabBar* tabBar() const { return bar_; }

    int addTab(QWidget* page, const QString& label) { return insertTab(-1, page, label); }
    int insertTab(int index, QWidget* page, const QString& label)
    {
        index = stack_->insertWidget(index, page);
        return bar_->insertTab(index, label);
    }
    void removeTab(int index)
    {
        if (QWidget* page = stack_->widget(index)) stack_->removeWidget(page);
    }

    QWidget* widget(int index) const { return stack_->widget(index); }
    QWidget* currentWidget() const { return stack_->currentWidget(); }
    int count() const { return stack_->count(); }
    int indexOf(const QWidget* page) const { return stack_->indexOf(page); }
    int currentIndex() const { return bar_->currentIndex(); }
    void setCurrentIndex(int index) { bar_->setCurrentIndex(index); }

    QString tabText(int index) const { return bar_->tabText(index); }
    QString tabToolTip(int index) const { return bar_->tabToolTip(index); }
    void setTabToolTip(int index, const QString& tip) { bar_->setTabToolTip(index, tip); }
    QIcon tabIcon(int index) const { return bar_->tabIcon(index); }
    void setTabIcon(int index, const QIcon& icon) { bar_->setTabIcon(index, icon); }
    void setTabsClosable(bool on) { bar_->setTabsClosable(on); }
    void setMovable(bool on) { bar_->setMovable(on); }

signals:
    void currentChanged(int index);
    void tabCloseRequested(int index);

private:
    QTabBar* bar_;
    QStackedWidget* stack_;
    QVBoxLayout* layout_;
};
