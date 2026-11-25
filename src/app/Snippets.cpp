#include "app/Snippets.h"
#include "app/Icons.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

namespace snippets {

enum { NameRole = Qt::UserRole, CommandRole, RunRole };

QString Snippet::label() const
{
    return name.trimmed().isEmpty() ? command.trimmed().section(QLatin1Char('\n'), 0, 0) : name.trimmed();
}

QList<Snippet> load(QSettings* settings)
{
    QList<Snippet> list;
    const int n = settings->beginReadArray(QStringLiteral("snippets"));
    for (int i = 0; i < n; ++i) {
        settings->setArrayIndex(i);
        list.append({settings->value(QStringLiteral("name")).toString(), settings->value(QStringLiteral("command")).toString(),
                     settings->value(QStringLiteral("run"), true).toBool()});
    }
    settings->endArray();
    return list;
}

static void save(QSettings* settings, const QList<Snippet>& list)
{
    settings->remove(QStringLiteral("snippets"));
    settings->beginWriteArray(QStringLiteral("snippets"), int(list.size()));
    for (int i = 0; i < list.size(); ++i) {
        settings->setArrayIndex(i);
        settings->setValue(QStringLiteral("name"), list[i].name);
        settings->setValue(QStringLiteral("command"), list[i].command);
        settings->setValue(QStringLiteral("run"), list[i].run);
    }
    settings->endArray();
}

void edit(QWidget* parent, QSettings* settings)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Snippets"));
    auto* list = new QListWidget;
    list->setDragDropMode(QAbstractItemView::InternalMove); // the menu's order
    auto* add = new QToolButton;
    add->setIcon(icons::lucide("plus"));
    add->setToolTip(QObject::tr("New snippet"));
    auto* remove = new QToolButton;
    remove->setIcon(icons::lucide("trash"));
    remove->setToolTip(QObject::tr("Delete the snippet"));
    auto* name = new QLineEdit;
    name->setPlaceholderText(QObject::tr("The command's first line"));
    auto* command = new QPlainTextEdit;
    command->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    command->setTabChangesFocus(true);
    auto* run = new QCheckBox(QObject::tr("Press Enter after it"));
    run->setToolTip(QObject::tr("Off: the command waits in the terminal, to be finished by hand"));

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch();
    auto* left = new QVBoxLayout;
    left->addWidget(list);
    left->addLayout(buttons);
    auto* form = new QFormLayout;
    form->addRow(QObject::tr("Name:"), name);
    form->addRow(QObject::tr("Command:"), command);
    form->addRow(QString(), run);
    auto* columns = new QHBoxLayout;
    columns->addLayout(left, 1);
    columns->addLayout(form, 2);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addLayout(columns);
    layout->addWidget(box);
    dialog.resize(720, 400);

    auto refresh = [&](QListWidgetItem* item) { // the item's text from its fields
        const Snippet s{item->data(NameRole).toString(), item->data(CommandRole).toString(), item->data(RunRole).toBool()};
        item->setText(s.label().isEmpty() ? QObject::tr("(empty)") : s.label());
    };
    auto append = [&](const Snippet& s) {
        auto* item = new QListWidgetItem(list);
        item->setData(NameRole, s.name);
        item->setData(CommandRole, s.command);
        item->setData(RunRole, s.run);
        refresh(item);
        return item;
    };
    for (const Snippet& s : load(settings)) append(s);

    bool filling = false;
    auto show = [&] {
        QListWidgetItem* item = list->currentItem();
        filling = true;
        name->setText(item ? item->data(NameRole).toString() : QString());
        command->setPlainText(item ? item->data(CommandRole).toString() : QString());
        run->setChecked(item ? item->data(RunRole).toBool() : true);
        filling = false;
        for (QWidget* w : std::initializer_list<QWidget*>{name, command, run, remove}) w->setEnabled(item);
    };
    auto store = [&] {
        QListWidgetItem* item = list->currentItem();
        if (filling || !item) return;
        item->setData(NameRole, name->text());
        item->setData(CommandRole, command->toPlainText());
        item->setData(RunRole, run->isChecked());
        refresh(item);
    };
    QObject::connect(list, &QListWidget::currentItemChanged, &dialog, show);
    QObject::connect(name, &QLineEdit::textChanged, &dialog, store);
    QObject::connect(command, &QPlainTextEdit::textChanged, &dialog, store);
    QObject::connect(run, &QCheckBox::toggled, &dialog, store);
    QObject::connect(add, &QToolButton::clicked, &dialog, [&] {
        list->setCurrentItem(append({}));
        command->setFocus();
    });
    QObject::connect(remove, &QToolButton::clicked, &dialog, [&] { delete list->currentItem(); });
    QObject::connect(box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    list->setCurrentRow(0);
    show();
    if (list->count() == 0) add->click();
    if (dialog.exec() != QDialog::Accepted) return;

    QList<Snippet> result;
    for (int i = 0; i < list->count(); ++i) {
        const QListWidgetItem* item = list->item(i);
        const Snippet s{item->data(NameRole).toString(), item->data(CommandRole).toString(), item->data(RunRole).toBool()};
        if (!s.command.trimmed().isEmpty()) result.append(s);
    }
    save(settings, result);
}

} // namespace snippets
