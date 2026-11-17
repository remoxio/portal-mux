#pragma once

#include "core/Session.h"

#include <QObject>
#include <QThread>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

class QWidget;

namespace prompts {

// For SessionListener::askTrust / askCredentials. UI thread only.
// `always`: "Always trust" checkbox, set on yes
bool trust(QWidget* parent, const QString& title, const QString& details, bool* always = nullptr);
bool credentials(QWidget* parent, const QString& prompt, rdc::Credentials& creds);

} // namespace prompts

// Blocks a worker thread on a UI-thread dialog.
// close() wakes waiters with "declined", so views can join the worker.
// `declined` runs on the UI thread when the user cancels.
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
