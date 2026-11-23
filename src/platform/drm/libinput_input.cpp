#include "platform/drm/drm_input_internal.h"

#include <fcntl.h>
#include <libinput.h>
#include <libudev.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

namespace pk {
namespace {

int open_restricted(const char* path, int flags, void*) {
    const int fd = open(path, flags | O_CLOEXEC);
    return fd < 0 ? -errno : fd;
}

void close_restricted(int fd, void*) {
    close(fd);
}

const libinput_interface kInterface = {open_restricted, close_restricted};

class LibinputInput final : public InputSource {
public:
    ~LibinputInput() override {
        if (libinput_) {
            libinput_unref(libinput_);
        }
        if (udev_) {
            udev_unref(udev_);
        }
    }

    bool initialize(const Display& display, std::string* error) {
        display_ = &display;
        udev_ = udev_new();
        if (!udev_) {
            return drm_input::set_error(error, "libinput: udev_new failed");
        }
        libinput_ = libinput_udev_create_context(&kInterface, nullptr, udev_);
        if (!libinput_) {
            return drm_input::set_error(error, "libinput: libinput_udev_create_context failed");
        }
        if (libinput_udev_assign_seat(libinput_, "seat0") != 0) {
            return drm_input::set_error(error, "libinput: cannot assign seat0 (is udev running?)");
        }
        const Size size = display.size();
        cursor_x_ = static_cast<float>(std::max(0, size.width / 2));
        cursor_y_ = static_cast<float>(std::max(0, size.height / 2));
        return true;
    }

    bool poll(InputEvent& event) override {
        if (events_.empty()) {
            drain();
        }
        if (events_.empty()) {
            return false;
        }
        event = events_.front();
        events_.pop_front();
        return true;
    }

private:
    void drain() {
        pollfd descriptor{libinput_get_fd(libinput_), POLLIN, 0};
        if (::poll(&descriptor, 1, 0) <= 0) {
            return;
        }
        if (libinput_dispatch(libinput_) != 0) {
            return;
        }
        libinput_event* event = nullptr;
        while ((event = libinput_get_event(libinput_)) != nullptr) {
            translate(event);
            libinput_event_destroy(event);
        }
    }

    void translate(libinput_event* event) {
        const Size size = display_->size();
        if (size.width <= 0 || size.height <= 0) {
            return;
        }
        switch (libinput_event_get_type(event)) {
            case LIBINPUT_EVENT_POINTER_MOTION: {
                libinput_event_pointer* pointer = libinput_event_get_pointer_event(event);
                cursor_x_ = drm_input::clamp_to(
                    cursor_x_ + static_cast<float>(libinput_event_pointer_get_dx(pointer)), size.width);
                cursor_y_ = drm_input::clamp_to(
                    cursor_y_ + static_cast<float>(libinput_event_pointer_get_dy(pointer)), size.height);
                push(InputEvent::Type::PointerMove, cursor_x_, cursor_y_);
                break;
            }
            case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
                libinput_event_pointer* pointer = libinput_event_get_pointer_event(event);
                cursor_x_ = drm_input::clamp_to(
                    static_cast<float>(libinput_event_pointer_get_absolute_x_transformed(
                        pointer, static_cast<std::uint32_t>(size.width))),
                    size.width);
                cursor_y_ = drm_input::clamp_to(
                    static_cast<float>(libinput_event_pointer_get_absolute_y_transformed(
                        pointer, static_cast<std::uint32_t>(size.height))),
                    size.height);
                push(InputEvent::Type::PointerMove, cursor_x_, cursor_y_);
                break;
            }
            case LIBINPUT_EVENT_POINTER_BUTTON: {
                libinput_event_pointer* pointer = libinput_event_get_pointer_event(event);
                if (libinput_event_pointer_get_button(pointer) != BTN_LEFT) {
                    break;
                }
                const bool pressed =
                    libinput_event_pointer_get_button_state(pointer) == LIBINPUT_BUTTON_STATE_PRESSED;
                push(pressed ? InputEvent::Type::PointerDown : InputEvent::Type::PointerUp, cursor_x_,
                     cursor_y_);
                break;
            }
            case LIBINPUT_EVENT_TOUCH_DOWN:
            case LIBINPUT_EVENT_TOUCH_MOTION: {
                libinput_event_touch* touch = libinput_event_get_touch_event(event);
                cursor_x_ = drm_input::clamp_to(
                    static_cast<float>(libinput_event_touch_get_x_transformed(
                        touch, static_cast<std::uint32_t>(size.width))),
                    size.width);
                cursor_y_ = drm_input::clamp_to(
                    static_cast<float>(libinput_event_touch_get_y_transformed(
                        touch, static_cast<std::uint32_t>(size.height))),
                    size.height);
                const bool down = libinput_event_get_type(event) == LIBINPUT_EVENT_TOUCH_DOWN;
                push(down ? InputEvent::Type::PointerDown : InputEvent::Type::PointerMove, cursor_x_,
                     cursor_y_);
                break;
            }
            case LIBINPUT_EVENT_TOUCH_UP:
            case LIBINPUT_EVENT_TOUCH_CANCEL:
                push(InputEvent::Type::PointerUp, cursor_x_, cursor_y_);
                break;
            case LIBINPUT_EVENT_KEYBOARD_KEY: {
                libinput_event_keyboard* keyboard = libinput_event_get_keyboard_event(event);
                const int key = libinput_event_keyboard_get_key(keyboard);
                const bool pressed =
                    libinput_event_keyboard_get_key_state(keyboard) == LIBINPUT_KEY_STATE_PRESSED;
                if (drm_input::is_quit_key(key)) {
                    if (pressed) {
                        push(InputEvent::Type::Quit, 0.0f, 0.0f, key);
                    }
                    break;
                }
                push(pressed ? InputEvent::Type::KeyDown : InputEvent::Type::KeyUp, 0.0f, 0.0f, key);
                break;
            }
            default:
                break;
        }
    }

    void push(InputEvent::Type type, float x, float y, int key = 0) {
        InputEvent event;
        event.type = type;
        event.x = x;
        event.y = y;
        event.key = key;
        events_.push_back(event);
    }

    const Display* display_ = nullptr;
    udev* udev_ = nullptr;
    libinput* libinput_ = nullptr;
    std::deque<InputEvent> events_;
    float cursor_x_ = 0.0f;
    float cursor_y_ = 0.0f;
};

}  // namespace

std::unique_ptr<InputSource> create_libinput_input(const Display& display, std::string* error) {
    auto input = std::make_unique<LibinputInput>();
    if (!input->initialize(display, error)) {
        return nullptr;
    }
    return input;
}

}  // namespace pk
