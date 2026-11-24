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
        open_once();
        if (pointer_ && pointer_->poll(event)) {
            return true;
        }
        if (gamepad_ && gamepad_->poll(event)) {
            return true;
        }
        return false;
    }

private:
    void open_once() {
        if (opened_) {
            return;
        }
        opened_ = true;
        std::string error;
        pointer_ = create_backend(&error);
        if (!pointer_) {
            log::warn("input: %s; running without touch, pointer, or key input", error.c_str());
        }
        std::string gamepad_error;
        gamepad_ = create_evdev_gamepad_input(display_, &gamepad_error);
        if (gamepad_) {
            log::info("input: evdev gamepad");
        } else {
            log::info("input: no gamepad (%s)", gamepad_error.c_str());
        }
    }

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
    std::unique_ptr<InputSource> pointer_;
    std::unique_ptr<InputSource> gamepad_;
};

}  // namespace

std::unique_ptr<InputSource> create_drm_input(const Display& display) {
    return std::make_unique<LazyInput>(display);
}

}  // namespace pk
