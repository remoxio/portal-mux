#pragma once

#include <QWidget>

#include <vector>

class QToolButton;
class QVBoxLayout;

// VS Code-style activity bar; its actions are its context menu
class ActivityBar : public QWidget {
    Q_OBJECT

public:
    explicit ActivityBar(QWidget* parent = nullptr);

    void addView(const QIcon& icon, const QString& name);
    void setViewHidden(int index, bool hidden);
    void setCurrent(int index); // -1: none lit (the panel is collapsed)

signals:
    void clicked(int index);
    void doubleClicked(int index);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QVBoxLayout* layout_;
    std::vector<QToolButton*> buttons_;
};
