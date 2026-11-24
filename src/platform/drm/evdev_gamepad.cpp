#include "platform/drm/drm_input_internal.h"

#include "core/log.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pk {
namespace {

constexpr float kDeadzone = 0.25f;
constexpr float kAxisEpsilon = 0.01f;

struct AxisRange {
    int minimum = -32768;
    int maximum = 32767;
};

struct Device {
    int fd = -1;
    std::string path;
    AxisRange x_range;
    AxisRange y_range;
    int hat_x = 0;
    int hat_y = 0;
    float x = 0.0f;
    float y = 0.0f;
    float pending_x = 0.0f;
    float pending_y = 0.0f;
    bool axis_pending = false;
    std::deque<InputEvent> events;
};

template <typename T>
bool bit_set(const T* bits, int bit) {
    constexpr int kBitsPerWord = static_cast<int>(sizeof(T) * 8);
    return ((bits[bit / kBitsPerWord] >> (bit % kBitsPerWord)) & 1) != 0;
}

float normalize_axis(int value, AxisRange range) {
    const float center = (static_cast<float>(range.minimum) + range.maximum) * 0.5f;
    const float half = (static_cast<float>(range.maximum) - range.minimum) * 0.5f;
    if (half <= 0.0f) {
        return 0.0f;
    }
    const float normalized = std::clamp((static_cast<float>(value) - center) / half, -1.0f, 1.0f);
    if (std::fabs(normalized) < kDeadzone) {
        return 0.0f;
    }
    return normalized;
}

void push_button(Device& device, InputEvent::GamepadButton button, bool down) {
    InputEvent event;
    event.type = down ? InputEvent::Type::GamepadButtonDown : InputEvent::Type::GamepadButtonUp;
    event.button = button;
    device.events.push_back(event);
}

void push_axis(Device& device, InputEvent::GamepadAxis axis, float value) {
    InputEvent event;
    event.type = InputEvent::Type::GamepadAxis;
    event.axis = axis;
    event.value = value;
    device.events.push_back(event);
}

bool map_button(int code, InputEvent::GamepadButton* button) {
    switch (code) {
        case BTN_SOUTH: *button = InputEvent::GamepadButton::A; return true;
        case BTN_EAST: *button = InputEvent::GamepadButton::B; return true;
        case BTN_NORTH: *button = InputEvent::GamepadButton::X; return true;
        case BTN_WEST: *button = InputEvent::GamepadButton::Y; return true;
        case BTN_START: *button = InputEvent::GamepadButton::Start; return true;
        case BTN_SELECT: *button = InputEvent::GamepadButton::Back; return true;
        case BTN_TL: *button = InputEvent::GamepadButton::LeftBumper; return true;
        case BTN_TR: *button = InputEvent::GamepadButton::RightBumper; return true;
        case BTN_DPAD_UP: *button = InputEvent::GamepadButton::DPadUp; return true;
        case BTN_DPAD_DOWN: *button = InputEvent::GamepadButton::DPadDown; return true;
        case BTN_DPAD_LEFT: *button = InputEvent::GamepadButton::DPadLeft; return true;
        case BTN_DPAD_RIGHT: *button = InputEvent::GamepadButton::DPadRight; return true;
        default: return false;
    }
}

void set_hat(Device& device, bool horizontal, int raw_value) {
    const int value = raw_value < 0 ? -1 : (raw_value > 0 ? 1 : 0);
    int& state = horizontal ? device.hat_x : device.hat_y;
    if (state == value) {
        return;
    }
    auto release = [&](int direction) {
        if (direction < 0) {
            push_button(device, horizontal ? InputEvent::GamepadButton::DPadLeft
                                           : InputEvent::GamepadButton::DPadUp,
                        false);
        } else if (direction > 0) {
            push_button(device, horizontal ? InputEvent::GamepadButton::DPadRight
                                           : InputEvent::GamepadButton::DPadDown,
                        false);
        }
    };
    release(state);
    state = value;
    if (value < 0) {
        push_button(device, horizontal ? InputEvent::GamepadButton::DPadLeft
                                       : InputEvent::GamepadButton::DPadUp,
                    true);
    } else if (value > 0) {
        push_button(device, horizontal ? InputEvent::GamepadButton::DPadRight
                                       : InputEvent::GamepadButton::DPadDown,
                    true);
    }
}

void flush(Device& device) {
    if (!device.axis_pending) {
        return;
    }
    device.axis_pending = false;
    if (std::fabs(device.pending_x - device.x) >= kAxisEpsilon) {
        device.x = device.pending_x;
        push_axis(device, InputEvent::GamepadAxis::LeftX, device.x);
    }
    if (std::fabs(device.pending_y - device.y) >= kAxisEpsilon) {
        device.y = device.pending_y;
        push_axis(device, InputEvent::GamepadAxis::LeftY, device.y);
    }
}

void handle_event(Device& device, const input_event& event) {
    if (event.type == EV_SYN) {
        if (event.code == SYN_REPORT) {
            flush(device);
        }
        return;
    }
    if (event.type == EV_KEY) {
        if (event.value == 2) {
            return;
        }
        InputEvent::GamepadButton button;
        if (map_button(event.code, &button)) {
            push_button(device, button, event.value != 0);
        }
        return;
    }
    if (event.type != EV_ABS) {
        return;
    }
    if (event.code == ABS_HAT0X) {
        set_hat(device, true, event.value);
    } else if (event.code == ABS_HAT0Y) {
        set_hat(device, false, event.value);
    } else if (event.code == ABS_X) {
        device.pending_x = normalize_axis(event.value, device.x_range);
        device.axis_pending = true;
    } else if (event.code == ABS_Y) {
        device.pending_y = normalize_axis(event.value, device.y_range);
        device.axis_pending = true;
    }
}

void close_device(Device& device) {
    if (device.fd >= 0) {
        close(device.fd);
        device.fd = -1;
    }
}

bool describe(Device& device) {
    unsigned long ev_bits[EV_MAX / (8 * sizeof(unsigned long)) + 2] = {};
    if (ioctl(device.fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
        return false;
    }

    unsigned long key_bits[KEY_MAX / (8 * sizeof(unsigned long)) + 2] = {};
    if (!bit_set(ev_bits, EV_KEY) ||
        ioctl(device.fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
        return false;
    }
    // A standard gamepad exposes the south face button. Devices without it are
    // left to the pointer/keyboard reader.
    if (!bit_set(key_bits, BTN_SOUTH)) {
        return false;
    }

    unsigned long abs_bits[ABS_MAX / (8 * sizeof(unsigned long)) + 2] = {};
    if (bit_set(ev_bits, EV_ABS) &&
        ioctl(device.fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) >= 0) {
        input_absinfo info{};
        if (bit_set(abs_bits, ABS_X) && ioctl(device.fd, EVIOCGABS(ABS_X), &info) == 0) {
            device.x_range = AxisRange{info.minimum, info.maximum};
            device.pending_x = device.x = normalize_axis(info.value, device.x_range);
        }
        if (bit_set(abs_bits, ABS_Y) && ioctl(device.fd, EVIOCGABS(ABS_Y), &info) == 0) {
            device.y_range = AxisRange{info.minimum, info.maximum};
            device.pending_y = device.y = normalize_axis(info.value, device.y_range);
        }
    }
    return true;
}

class EvdevGamepadInput final : public InputSource {
public:
    ~EvdevGamepadInput() override {
        for (Device& device : devices_) {
            close_device(device);
        }
    }

    bool open_devices(std::string* error) {
        std::vector<std::string> paths;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator("/dev/input", ec)) {
            if (entry.path().filename().string().rfind("event", 0) == 0) {
                paths.push_back(entry.path().string());
            }
        }
        std::sort(paths.begin(), paths.end());
        if (paths.empty()) {
            return drm_input::set_error(error, "no /dev/input/event* devices");
        }

        std::string first_failure;
        for (const std::string& path : paths) {
            const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                if (first_failure.empty()) {
                    first_failure = "cannot open " + path + ": " + std::strerror(errno);
                }
                continue;
            }
            Device device;
            device.fd = fd;
            device.path = path;
            if (!describe(device)) {
                close(fd);
                continue;
            }
            devices_.push_back(std::move(device));
        }
        if (devices_.empty()) {
            return drm_input::set_error(
                error, first_failure.empty() ? "no gamepad under /dev/input" : first_failure);
        }
        return true;
    }

    bool poll(InputEvent& event) override {
        for (;;) {
            for (Device& device : devices_) {
                if (!device.events.empty()) {
                    event = device.events.front();
                    device.events.pop_front();
                    return true;
                }
            }
            if (devices_.empty()) {
                return false;
            }

            std::vector<pollfd> descriptors;
            std::vector<std::size_t> indices;
            descriptors.reserve(devices_.size());
            indices.reserve(devices_.size());
            for (std::size_t i = 0; i < devices_.size(); ++i) {
                if (devices_[i].fd < 0) {
                    continue;
                }
                descriptors.push_back(pollfd{devices_[i].fd, POLLIN, 0});
                indices.push_back(i);
            }
            if (descriptors.empty() ||
                ::poll(descriptors.data(), descriptors.size(), 0) <= 0) {
                return false;
            }

            bool produced = false;
            for (std::size_t i = 0; i < descriptors.size(); ++i) {
                if (!(descriptors[i].revents & (POLLIN | POLLERR | POLLHUP))) {
                    continue;
                }
                Device& device = devices_[indices[i]];
                read_device(device);
                produced = produced || !device.events.empty();
            }
            if (!produced) {
                return false;
            }
        }
    }

private:
    void read_device(Device& device) {
        input_event events[64];
        for (;;) {
            const ssize_t bytes = read(device.fd, events, sizeof(events));
            if (bytes < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    log::warn("input: gamepad %s disconnected: %s", device.path.c_str(),
                              std::strerror(errno));
                    close_device(device);
                }
                return;
            }
            if (bytes == 0) {
                return;
            }
            const std::size_t count = static_cast<std::size_t>(bytes) / sizeof(input_event);
            for (std::size_t i = 0; i < count; ++i) {
                handle_event(device, events[i]);
            }
            if (static_cast<std::size_t>(bytes) < sizeof(events)) {
                return;
            }
        }
    }

    std::vector<Device> devices_;
};

}  // namespace

std::unique_ptr<InputSource> create_evdev_gamepad_input(const Display&, std::string* error) {
    auto input = std::make_unique<EvdevGamepadInput>();
    if (!input->open_devices(error)) {
        return nullptr;
    }
    return input;
}

}  // namespace pk
