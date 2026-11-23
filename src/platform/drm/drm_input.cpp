#include "platform/drm/drm_input.h"

#include "core/log.h"
#include "platform/drm/drm_input_internal.h"

#include <memory>
#include <string>

namespace pk {
namespace {

class LazyInput final : public InputSource {
public:
    explicit LazyInput(const Display& display) : display_(display) {}

    bool poll(InputEvent& event) override {
        if (!input_ && !opened_) {
            opened_ = true;
            std::string error;
            input_ = create_backend(&error);
            if (!input_) {
                log::warn("input: %s; running without touch, pointer, or key input", error.c_str());
            }
        }
        return input_ && input_->poll(event);
    }

private:
    std::unique_ptr<InputSource> create_backend(std::string* error) {
#if defined(PUCK_KIOSK_HAVE_LIBINPUT)
        std::string libinput_error;
        if (auto input = create_libinput_input(display_, &libinput_error)) {
            log::info("input: libinput on seat0");
            return input;
        }
        log::warn("input: %s; falling back to evdev", libinput_error.c_str());
#endif
        auto input = create_evdev_input(display_, error);
        if (input) {
            log::info("input: evdev");
        }
        return input;
    }

    const Display& display_;
    bool opened_ = false;
    std::unique_ptr<InputSource> input_;
};

}  // namespace

std::unique_ptr<InputSource> create_drm_input(const Display& display) {
    return std::make_unique<LazyInput>(display);
}

}  // namespace pk
