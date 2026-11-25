#pragma once

#include "core/Session.h"

#include <QObject>
#include <QStyle>
#include <QThread>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

class QWidget;

// Blocks a worker on a UI-thread prompt; close() wakes waiters as declined
class PromptBridge {
public:
    PromptBridge(QObject* context, std::function<void()> declined) : context_(context), declined_(std::move(declined)) {}

    template <class Fn>
    bool ask(Fn fn)
    {
        auto run = [this, fn = std::move(fn)]() mutable {
            const bool ok = fn();
            if (!ok) QMetaObject::invokeMethod(context_, declined_, Qt::QueuedConnection);
            return ok;
        };
        if (QThread::currentThread() == context_->thread()) return run();
        auto state = std::make_shared<State>();
        QMetaObject::invokeMethod(context_, [this, state, run = std::move(run)]() mutable {
            const bool result = run();
            {
                std::lock_guard lock(mutex_);
                state->done = true;
                state->result = result;
            }
            cv_.notify_all();
        }, Qt::QueuedConnection);
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return state->done || closed_; });
        return state->done && state->result;
    }

    // From a worker: `show` puts up a panel on the UI thread and replies once
    bool askLater(std::function<void(std::function<void(bool)>)> show)
    {
        auto state = std::make_shared<State>();
        auto reply = [this, state](bool ok) {
            {
                std::lock_guard lock(mutex_);
                if (state->done || closed_) return;
                state->done = true;
                state->result = ok;
            }
            cv_.notify_all();
            if (!ok) QMetaObject::invokeMethod(context_, declined_, Qt::QueuedConnection);
        };
        QMetaObject::invokeMethod(context_, [show = std::move(show), reply] { show(reply); }, Qt::QueuedConnection);
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return state->done || closed_; });
        return state->done && state->result;
    }

    void close()
    {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

private:
    struct State {
        bool done = false, result = false;
    };
    QObject* context_;
    std::function<void()> declined_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool closed_ = false;
};

namespace prompts {

// Dialogs, blocking the app: for prompts that belong to no tab; `always`: an "Always trust" box
bool trust(QWidget* parent, const QString& title, const QString& details, bool* always = nullptr);
bool credentials(QWidget* parent, const QString& prompt, rdc::Credentials& creds);
// before something hard to undo: Cancel is the default, the action never focused
bool confirm(QWidget* parent, const QString& title, const QString& text, const QString& action,
             QStyle::StandardPixmap icon = QStyle::SP_MessageBoxQuestion);

// A tab's: a panel over `tab` that blocks only it; `reply` once, never if the tab goes first
void trustIn(QWidget* tab, const QString& title, const QString& details, bool* always, std::function<void(bool)> reply);
void credentialsIn(QWidget* tab, const QString& prompt, rdc::Credentials& creds, std::function<void(bool)> reply);
void errorIn(QWidget* tab, const QString& title, const QString& text, const QString& again, std::function<void(bool again)> reply = {});

// From a tab's session worker, waiting in `bridge`; on the UI thread itself, the dialog
bool trust(PromptBridge& bridge, QWidget* tab, const QString& title, const QString& details, bool* always = nullptr);
bool credentials(PromptBridge& bridge, QWidget* tab, const QString& prompt, rdc::Credentials& creds);

} // namespace prompts
