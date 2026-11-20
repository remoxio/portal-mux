#include "app/Prompts.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace prompts {

bool trust(QWidget* parent, const QString& title, const QString& details, bool* always)
{
    // not QMessageBox: themes press the focused button on Enter; No is the default, Yes never focused
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Security"));
    auto* icon = new QLabel;
    icon->setPixmap(dialog.style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(48, 48));
    auto* text = new QLabel(QStringLiteral("<b>%1</b>").arg(title.toHtmlEscaped()));
    text->setWordWrap(true);
    auto* info = new QLabel(details);
    info->setWordWrap(true);
    info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* yes = new QPushButton(QObject::tr("&Yes"));
    auto* no = new QPushButton(QObject::tr("&No"));
    yes->setAutoDefault(false);
    yes->setFocusPolicy(Qt::NoFocus);
    no->setDefault(true);
    QObject::connect(yes, &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(no, &QPushButton::clicked, &dialog, &QDialog::reject);

    auto* grid = new QGridLayout(&dialog);
    grid->addWidget(icon, 0, 0, 2, 1, Qt::AlignTop);
    grid->addWidget(text, 0, 1);
    grid->addWidget(info, 1, 1);
    auto* buttons = new QHBoxLayout;
    auto* remember = always ? new QCheckBox(QObject::tr("&Always trust")) : nullptr;
    if (remember) {
        remember->setToolTip(QObject::tr("Don't ask again for this connection (Edit → Security to undo)"));
        buttons->addWidget(remember);
    }
    buttons->addStretch();
    buttons->addWidget(yes);
    buttons->addWidget(no);
    grid->addLayout(buttons, 2, 0, 1, 2);
    grid->setColumnStretch(1, 1);
    dialog.setMinimumWidth(480);
    no->setFocus();
    if (dialog.exec() != QDialog::Accepted) return false;
    if (remember) *always = remember->isChecked();
    return true;
}

bool credentials(QWidget* parent, const QString& prompt, rdc::Credentials& creds)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Credentials"));
    auto* form = new QFormLayout(&dialog);
    form->addRow(new QLabel(prompt));
    auto* user = new QLineEdit(QString::fromStdString(creds.username));
    auto* domain = new QLineEdit(QString::fromStdString(creds.domain));
    auto* password = new QLineEdit(QString::fromStdString(creds.password));
    password->setEchoMode(QLineEdit::Password);
    form->addRow(QObject::tr("&User name:"), user);
    form->addRow(QObject::tr("&Domain:"), domain);
    form->addRow(QObject::tr("&Password:"), password);
    form->setRowVisible(user, creds.needsUser);
    form->setRowVisible(domain, creds.needsDomain);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    (creds.needsUser && user->text().isEmpty() ? user : password)->setFocus();

    if (dialog.exec() != QDialog::Accepted) return false;
    creds.username = user->text().toStdString();
    creds.domain = domain->text().toStdString();
    creds.password = password->text().toStdString();
    return true;
}

namespace {

// Over a tab: dims it, takes its clicks and keys, holds a card in the middle; the rest of the app stays usable
class Scrim : public QWidget {
public:
    explicit Scrim(QWidget* tab) : QWidget(hostOf(tab)), owner_(tab), card_(new QFrame(this))
    {
        card_->setContentsMargins(8, 8, 8, 8); // drawn by paintEvent
        setBackgroundRole(QPalette::Window); // not the display's Shadow, whose text colour is Light
        setForegroundRole(QPalette::WindowText);
        parentWidget()->installEventFilter(this);
    }

    QFrame* card() const { return card_; }
    std::function<void()> enter, escape; // Return (on no focused button) and Escape

    void open(QWidget* focus)
    {
        focus_ = focus;
        previousProxy_ = owner_->focusProxy();
        owner_->setFocusProxy(focus); // focus given to the tab lands here, not in the session
        place();
        show();
        if (owner_->isVisible()) focus->setFocus();
    }

    void finish()
    {
        setEnabled(false); // a second click answers nothing
        const bool focused = isAncestorOf(QApplication::focusWidget());
        if (owner_ && owner_->focusProxy() && isAncestorOf(owner_->focusProxy())) owner_->setFocusProxy(previousProxy_);
        hide();
        if (focused && owner_) owner_->setFocus();
        deleteLater();
    }

protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::Resize) QTimer::singleShot(0, this, [this] { place(); }); // after the tab's own overlays
        return false;
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(0, 0, 0, 90));
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(palette().color(QPalette::Mid));
        p.setBrush(palette().color(QPalette::Window));
        p.drawRoundedRect(QRectF(card_->geometry()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    }

    void showEvent(QShowEvent*) override // its tab shown again: the answer is wanted here
    {
        QTimer::singleShot(0, this, [this] {
            if (isVisible() && isEnabled() && focus_) focus_->setFocus();
        });
    }

    void mousePressEvent(QMouseEvent* event) override { event->accept(); }
    void mouseReleaseEvent(QMouseEvent* event) override { event->accept(); }
    void mouseDoubleClickEvent(QMouseEvent* event) override { event->accept(); }
    void wheelEvent(QWheelEvent* event) override { event->accept(); }
    void contextMenuEvent(QContextMenuEvent* event) override { event->accept(); }

    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::ShortcutOverride) { // Return and Escape answer here, not the window's shortcuts
            const int key = static_cast<QKeyEvent*>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Escape) {
                event->accept();
                return true;
            }
        }
        return QWidget::event(event);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        event->accept(); // nothing reaches the session behind
        if (event->key() == Qt::Key_Escape && escape) return escape();
        if (event->key() != Qt::Key_Return && event->key() != Qt::Key_Enter) return;
        if (auto* button = qobject_cast<QAbstractButton*>(QApplication::focusWidget()); button && isAncestorOf(button)) return button->click();
        if (enter) enter();
    }

    bool focusNextPrevChild(bool next) override // Tab stays in the card
    {
        QList<QWidget*> chain;
        for (QWidget* w : card_->findChildren<QWidget*>())
            if (w->isVisibleTo(card_) && w->isEnabled() && (w->focusPolicy() & Qt::TabFocus)) chain << w;
        if (chain.isEmpty()) return true;
        const qsizetype at = chain.indexOf(QApplication::focusWidget());
        const qsizetype to = at < 0 ? 0 : (at + (next ? 1 : chain.size() - 1)) % chain.size();
        chain.at(to)->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
        return true;
    }

private:
    // a scrolled page (a remote display): over its visible part
    static QWidget* hostOf(QWidget* tab)
    {
        QWidget* parent = tab->parentWidget();
        auto* area = qobject_cast<QAbstractScrollArea*>(parent ? parent->parentWidget() : nullptr);
        return area && area->viewport() == parent ? parent : tab;
    }

    void place()
    {
        const QWidget* host = parentWidget();
        setGeometry(host->rect());
        const int width = std::clamp(host->width() - 32, 260, 500);
        card_->setFixedWidth(width);
        const QLayout* layout = card_->layout();
        card_->setFixedHeight(layout && layout->hasHeightForWidth() ? layout->totalHeightForWidth(width) : card_->sizeHint().height());
        card_->move(std::max(0, (host->width() - width) / 2), std::max(0, (host->height() - card_->height()) / 2));
        raise();
    }

    QPointer<QWidget> owner_, previousProxy_, focus_;
    QFrame* card_;
};

QLabel* warningIcon(QWidget* w)
{
    auto* icon = new QLabel;
    icon->setPixmap(w->style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(48, 48));
    return icon;
}

QLabel* wrapped(const QString& text)
{
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    return label;
}

} // namespace

void trustIn(QWidget* tab, const QString& title, const QString& details, bool* always, std::function<void(bool)> reply)
{
    auto* scrim = new Scrim(tab);
    auto* info = wrapped(details);
    info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* yes = new QPushButton(QObject::tr("&Yes"));
    auto* no = new QPushButton(QObject::tr("&No"));
    yes->setFocusPolicy(Qt::NoFocus); // Return never trusts
    auto* remember = always ? new QCheckBox(QObject::tr("&Always trust")) : nullptr;
    auto answer = [scrim, reply, always, remember](bool ok) {
        if (ok && remember) *always = remember->isChecked();
        scrim->finish();
        reply(ok);
    };
    QObject::connect(yes, &QPushButton::clicked, scrim, [answer] { answer(true); });
    QObject::connect(no, &QPushButton::clicked, scrim, [answer] { answer(false); });
    scrim->enter = scrim->escape = [answer] { answer(false); };

    auto* grid = new QGridLayout(scrim->card());
    grid->addWidget(warningIcon(scrim), 0, 0, 2, 1, Qt::AlignTop);
    grid->addWidget(wrapped(QStringLiteral("<b>%1</b>").arg(title.toHtmlEscaped())), 0, 1);
    grid->addWidget(info, 1, 1);
    auto* buttons = new QHBoxLayout;
    if (remember) {
        remember->setToolTip(QObject::tr("Don't ask again for this connection (Edit → Security to undo)"));
        buttons->addWidget(remember);
    }
    buttons->addStretch();
    buttons->addWidget(yes);
    buttons->addWidget(no);
    grid->addLayout(buttons, 2, 0, 1, 2);
    grid->setColumnStretch(1, 1);
    scrim->open(no);
}

void credentialsIn(QWidget* tab, const QString& prompt, rdc::Credentials& creds, std::function<void(bool)> reply)
{
    auto* scrim = new Scrim(tab);
    auto* form = new QFormLayout(scrim->card());
    form->addRow(wrapped(QStringLiteral("<b>%1</b>").arg(QObject::tr("Credentials"))));
    form->addRow(wrapped(prompt));
    auto* user = new QLineEdit(QString::fromStdString(creds.username));
    auto* domain = new QLineEdit(QString::fromStdString(creds.domain));
    auto* password = new QLineEdit(QString::fromStdString(creds.password));
    password->setEchoMode(QLineEdit::Password);
    form->addRow(QObject::tr("&User name:"), user);
    form->addRow(QObject::tr("&Domain:"), domain);
    form->addRow(QObject::tr("&Password:"), password);
    form->setRowVisible(user, creds.needsUser);
    form->setRowVisible(domain, creds.needsDomain);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    auto answer = [scrim, reply, &creds, user, domain, password](bool ok) { // creds: the worker's, waiting for this
        if (ok) {
            creds.username = user->text().toStdString();
            creds.domain = domain->text().toStdString();
            creds.password = password->text().toStdString();
        }
        scrim->finish();
        reply(ok);
    };
    QObject::connect(buttons, &QDialogButtonBox::accepted, scrim, [answer] { answer(true); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, scrim, [answer] { answer(false); });
    scrim->enter = [answer] { answer(true); };
    scrim->escape = [answer] { answer(false); };
    scrim->open(creds.needsUser && user->text().isEmpty() ? user : password);
}

void errorIn(QWidget* tab, const QString& title, const QString& text, const QString& again, std::function<void(bool)> reply)
{
    auto* scrim = new Scrim(tab);
    auto* close = new QPushButton(again.isEmpty() ? QObject::tr("OK") : QObject::tr("&Close"));
    auto* retry = again.isEmpty() ? nullptr : new QPushButton(again);
    auto answer = [scrim, reply](bool retried) {
        scrim->finish();
        if (reply) reply(retried);
    };
    QObject::connect(close, &QPushButton::clicked, scrim, [answer] { answer(false); });
    if (retry) QObject::connect(retry, &QPushButton::clicked, scrim, [answer] { answer(true); });
    scrim->enter = scrim->escape = [answer] { answer(false); };

    auto* grid = new QGridLayout(scrim->card());
    grid->addWidget(warningIcon(scrim), 0, 0, 2, 1, Qt::AlignTop);
    grid->addWidget(wrapped(QStringLiteral("<b>%1</b>").arg(title.toHtmlEscaped())), 0, 1);
    auto* info = wrapped(text);
    info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    grid->addWidget(info, 1, 1);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    if (retry) buttons->addWidget(retry);
    buttons->addWidget(close);
    grid->addLayout(buttons, 2, 0, 1, 2);
    grid->setColumnStretch(1, 1);
    scrim->open(close);
}

bool trust(PromptBridge& bridge, QWidget* tab, const QString& title, const QString& details, bool* always)
{
    if (QThread::currentThread() == tab->thread()) return bridge.ask([&] { return trust(tab, title, details, always); });
    return bridge.askLater([=](std::function<void(bool)> reply) { trustIn(tab, title, details, always, std::move(reply)); });
}

bool credentials(PromptBridge& bridge, QWidget* tab, const QString& prompt, rdc::Credentials& creds)
{
    if (QThread::currentThread() == tab->thread()) return bridge.ask([&] { return credentials(tab, prompt, creds); });
    return bridge.askLater([tab, prompt, &creds](std::function<void(bool)> reply) { credentialsIn(tab, prompt, creds, std::move(reply)); });
}

} // namespace prompts
