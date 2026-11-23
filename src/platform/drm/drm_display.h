#pragma once

#include "platform/display.h"

#include <memory>
#include <string>

namespace pk {

// DRM/KMS + GBM + EGL direct scanout for embedded Linux boards. The frame is
// rendered into a GBM surface and flipped onto a connected connector without a
// compositor or window system.
class DrmDisplay final : public Display {
public:
    explicit DrmDisplay(std::string device_path = {});
    ~DrmDisplay() override;

    bool initialize(std::string* error) override;
    Size size() const override { return size_; }
    float dpi_scale() const override { return 1.0f; }
    void make_current() override;
    void swap() override;
    bool should_close() const override;
    const char* backend_name() const override { return "drm"; }

private:
    struct Impl;

    void fail(const std::string& message);

    std::unique_ptr<Impl> impl_;
    Size size_;
    bool failed_ = false;
};

}  // namespace pk
