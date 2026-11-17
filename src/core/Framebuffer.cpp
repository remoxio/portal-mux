#include "core/Framebuffer.h"

#include <cstring>

namespace rdc {

void Framebuffer::resize(int width, int height)
{
    std::lock_guard lock(mutex_);
    width_ = std::max(width, 0);
    height_ = std::max(height, 0);
    pixels_.assign(size_t(width_) * height_, 0xFF000000u);
    dirty_ = {0, 0, width_, height_};
}

void Framebuffer::update(const Rect& area, const uint8_t* src, int srcStride)
{
    std::lock_guard lock(mutex_);
    Rect r = area.intersected({0, 0, width_, height_});
    if (r.empty()) return;
    const size_t rowBytes = size_t(r.width) * BytesPerPixel;
    for (int y = r.y; y < r.y + r.height; ++y) {
        auto* dst = reinterpret_cast<uint8_t*>(&pixels_[size_t(y) * width_ + r.x]);
        std::memcpy(dst, src + size_t(y) * srcStride + size_t(r.x) * BytesPerPixel, rowBytes);
        for (size_t i = 3; i < rowBytes; i += 4) dst[i] = 0xFF; // Qt needs X opaque
    }
    dirty_ = dirty_.united(r);
}

} // namespace rdc
