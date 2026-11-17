#include "core/TestPatternSession.h"

#include <chrono>
#include <cstring>

namespace rdc {

TestPatternSession::TestPatternSession(DisplayListener& listener, int width, int height)
    : listener_(listener), width_(std::max(width, 128)), height_(std::max(height, 128))
{
}

void TestPatternSession::connect()
{
    if (!worker_.joinable()) worker_ = std::jthread([this](std::stop_token st) { run(st); });
}

void TestPatternSession::disconnect()
{
    if (!worker_.joinable()) return;
    worker_.request_stop();
    worker_.join();
}

void TestPatternSession::sendPointer(const PointerEvent& event)
{
    std::lock_guard lock(inputMutex_);
    pointerQueue_.push_back(event);
}

void TestPatternSession::run(std::stop_token stop)
{
    listener_.onStateChanged(SessionState::Connecting);

    const int w = width_, h = height_, stride = w * Framebuffer::BytesPerPixel;
    std::vector<uint8_t> background(size_t(stride) * h), back;
    auto put = [&](std::vector<uint8_t>& buf, int x, int y, uint32_t rgb) {
        std::memcpy(&buf[size_t(y) * stride + size_t(x) * 4], &rgb, 4); // BGRX in memory
    };
    auto fill = [&](std::vector<uint8_t>& buf, const Rect& r, uint32_t rgb) {
        for (int y = r.y; y < r.y + r.height; ++y)
            for (int x = r.x; x < r.x + r.width; ++x) put(buf, x, y, rgb);
    };
    auto restore = [&](const Rect& r) {
        for (int y = r.y; y < r.y + r.height; ++y)
            std::memcpy(&back[size_t(y) * stride + size_t(r.x) * 4],
                        &background[size_t(y) * stride + size_t(r.x) * 4], size_t(r.width) * 4);
    };

    static constexpr uint32_t bars[] = {0xC0C0C0, 0xC0C000, 0x00C0C0, 0x00C000,
                                        0xC000C0, 0xC00000, 0x0000C0, 0x101010};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            put(background, x, y, y < h * 2 / 3 ? bars[x * 8 / w] : uint32_t(x * 255 / (w - 1)) * 0x010101u);
    back = background;

    const Rect screen{0, 0, w, h};
    framebuffer_.resize(w, h);
    listener_.onResize(w, h);
    framebuffer_.update(screen, back.data(), stride);
    listener_.onFrameUpdated(screen);
    listener_.onStateChanged(SessionState::Connected);

    Rect box{0, h / 3, 80, 80};
    int dx = 4;
    std::vector<PointerEvent> events;
    while (!stop.stop_requested()) {
        Rect dirty;

        {
            std::lock_guard lock(inputMutex_);
            events.swap(pointerQueue_);
        }
        for (const auto& e : events) {
            if (!(e.buttons & PointerEvent::Left)) continue;
            Rect dot = Rect{e.x - 3, e.y - 3, 6, 6}.intersected(screen);
            fill(background, dot, 0xFFFFFF);
            fill(back, dot, 0xFFFFFF);
            dirty = dirty.united(dot);
        }
        events.clear();

        Rect old = box;
        box.x += dx;
        if (box.x < 0 || box.x + box.width > w) {
            dx = -dx;
            box.x += 2 * dx;
        }
        restore(old);
        fill(back, box, 0xFF8000);
        dirty = dirty.united(old).united(box);

        framebuffer_.update(dirty, back.data(), stride);
        listener_.onFrameUpdated(dirty);
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    listener_.onStateChanged(SessionState::Disconnected);
}

} // namespace rdc
