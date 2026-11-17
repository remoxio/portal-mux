#pragma once

#include "core/Types.h"

#include <cstdint>
#include <mutex>
#include <vector>

namespace rdc {

// Front buffer shared between a session worker and the UI.
// 32-bit BGRX (0xFFRRGGBB); update() forces the 4th byte to 0xFF
class Framebuffer {
public:
    static constexpr int BytesPerPixel = 4;

    void resize(int width, int height);

    void update(const Rect& area, const uint8_t* src, int srcStride);

    // fn runs under the lock; skipped when nothing changed
    template <class Fn>
    void consume(Fn&& fn)
    {
        std::lock_guard lock(mutex_);
        if (dirty_.empty()) return;
        fn(reinterpret_cast<const uint8_t*>(pixels_.data()), width_, height_, width_ * BytesPerPixel, dirty_);
        dirty_ = {};
    }

private:
    std::mutex mutex_;
    std::vector<uint32_t> pixels_;
    int width_ = 0, height_ = 0;
    Rect dirty_;
};

} // namespace rdc
