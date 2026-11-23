#include "platform/drm/drm_input_internal.h"

#include "core/log.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
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

struct AxisRange {
    int minimum = 0;
    int maximum = 0;
};

struct Device {
    int fd = -1;
    std::string path;
    bool absolute = false;
    bool relative = false;
    int x_code = ABS_X;
    int y_code = ABS_Y;
    AxisRange x_range;
    AxisRange y_range;
    bool down = false;
    bool desired_down = false;
    bool state_changed = false;
    bool moved = false;
    float x = 0.0f;
    float y = 0.0f;
    std::deque<InputEvent> events;
};

template <typename T>
bool bit_set(const T* bits, int bit) {
    constexpr int kBitsPerWord = static_cast<int>(sizeof(T) * 8);
    return ((bits[bit / kBitsPerWord] >> (bit % kBitsPerWord)) & 1) != 0;
}

void push(Device& device, InputEvent::Type type, int key = 0) {
    InputEvent event;
    event.type = type;
    event.x = device.x;
    event.y = device.y;
    event.key = key;
    device.events.push_back(event);
}

void set_down(Device& device, bool down) {
    device.desired_down = down;
    device.state_changed = true;
}

void close_device(Device& device) {
    if (device.fd >= 0) {
        close(device.fd);
        device.fd = -1;
    }
}

// Emits at most one pointer event per input frame, so a tap reports the
// coordinates that arrived in the same SYN_REPORT.
void flush(Device& device) {
    bool emitted_state = false;
    if (device.state_changed && device.desired_down != device.down) {
        device.down = device.desired_down;
        push(device, device.down ? InputEvent::Type::PointerDown : InputEvent::Type::PointerUp);
        emitted_state = true;
    }
    if (!emitted_state && device.moved && (device.down || device.relative)) {
        push(device, InputEvent::Type::PointerMove);
    }
    device.state_changed = false;
    device.moved = false;
}

void handle_event(Device& device, const input_event& event, Size size) {
    if (event.type == EV_SYN) {
        if (event.code == SYN_REPORT) {
            flush(device);
        }
        return;
    }
    if (event.type == EV_ABS) {
        if (event.code == device.x_code) {
            device.x = drm_input::map_axis(event.value, device.x_range.minimum, device.x_range.maximum,
                                           size.width);
            device.moved = true;
        } else if (event.code == device.y_code) {
            device.y = drm_input::map_axis(event.value, device.y_range.minimum, device.y_range.maximum,
                                           size.height);
            device.moved = true;
        } else if (event.code == ABS_MT_TRACKING_ID) {
            set_down(device, event.value >= 0);
        }
        return;
    }
    if (event.type == EV_REL) {
        if (event.code == REL_X) {
            device.x = drm_input::clamp_to(device.x + static_cast<float>(event.value), size.width);
        } else if (event.code == REL_Y) {
            device.y = drm_input::clamp_to(device.y + static_cast<float>(event.value), size.height);
        } else {
            return;
        }
        device.moved = true;
        return;
    }
    if (event.type != EV_KEY) {
        return;
    }
    if (event.code == BTN_TOUCH || event.code == BTN_LEFT) {
        set_down(device, event.value != 0);
        return;
    }
    if (drm_input::is_button_code(event.code) || event.value == 2) {
        return;
    }
    if (drm_input::is_quit_key(event.code)) {
        if (event.value == 1) {
            push(device, InputEvent::Type::Quit, event.code);
        }
        return;
    }
    push(device, event.value != 0 ? InputEvent::Type::KeyDown : InputEvent::Type::KeyUp, event.code);
}

bool describe(Device& device) {
    unsigned long ev_bits[EV_MAX / (8 * sizeof(unsigned long)) + 2] = {};
    if (ioctl(device.fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
        return false;
    }

    unsigned long key_bits[KEY_MAX / (8 * sizeof(unsigned long)) + 2] = {};
    const bool has_key_bits =
        bit_set(ev_bits, EV_KEY) &&
        ioctl(device.fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0;
    unsigned long abs_bits[ABS_MAX / (8 * sizeof(unsigned long)) + 2] = {};
    const bool has_abs_bits =
        bit_set(ev_bits, EV_ABS) &&
        ioctl(device.fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) >= 0;
    unsigned long rel_bits[REL_MAX / (8 * sizeof(unsigned long)) + 2] = {};
    const bool has_rel_bits =
        bit_set(ev_bits, EV_REL) &&
        ioctl(device.fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)), rel_bits) >= 0;

    if (has_abs_bits) {
        const bool multitouch =
            bit_set(abs_bits, ABS_MT_POSITION_X) && bit_set(abs_bits, ABS_MT_POSITION_Y);
        const bool single = bit_set(abs_bits, ABS_X) && bit_set(abs_bits, ABS_Y);
        if (multitouch || single) {
            device.x_code = multitouch ? ABS_MT_POSITION_X : ABS_X;
            device.y_code = multitouch ? ABS_MT_POSITION_Y : ABS_Y;
            input_absinfo x_info{};
            input_absinfo y_info{};
            if (ioctl(device.fd, EVIOCGABS(device.x_code), &x_info) == 0 &&
                ioctl(device.fd, EVIOCGABS(device.y_code), &y_info) == 0) {
                device.x_range = AxisRange{x_info.minimum, x_info.maximum};
                device.y_range = AxisRange{y_info.minimum, y_info.maximum};
                device.absolute = true;
            }
        }
    }
    if (has_rel_bits && bit_set(rel_bits, REL_X) && bit_set(rel_bits, REL_Y)) {
        device.relative = true;
    }

    const bool has_keyboard = has_key_bits && bit_set(key_bits, KEY_ESC);
    const bool has_touch_button = has_key_bits && bit_set(key_bits, BTN_TOUCH);
    const bool has_pointer_button = has_key_bits && bit_set(key_bits, BTN_LEFT);
    return device.absolute || device.relative || has_keyboard || has_touch_button ||
           has_pointer_button;
}

class EvdevInput final : public InputSource {
public:
    explicit EvdevInput(const Display& display) : display_(display) {}

    ~EvdevInput() override {
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
            return drm_input::set_error(
                error, "no /dev/input/event* devices (is the kernel input subsystem available?)");
        }

        const Size size = display_.size();
        std::string first_failure;
        for (const std::string& path : paths) {
            const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                if (first_failure.empty()) {
                    first_failure = "cannot open " + path + ": " + std::strerror(errno);
                    if (errno == EACCES || errno == EPERM) {
                        first_failure += " (add the user to the 'input' group or run as root)";
                    }
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
            device.x = static_cast<float>(std::max(0, size.width / 2));
            device.y = static_cast<float>(std::max(0, size.height / 2));
            if (device.absolute) {
                // The kernel drops ABS values that equal the current state, so
                // start from the device's position instead of the screen center.
                input_absinfo x_info{};
                input_absinfo y_info{};
                if (ioctl(fd, EVIOCGABS(device.x_code), &x_info) == 0 &&
                    ioctl(fd, EVIOCGABS(device.y_code), &y_info) == 0) {
                    device.x = drm_input::map_axis(x_info.value, device.x_range.minimum,
                                                   device.x_range.maximum, size.width);
                    device.y = drm_input::map_axis(y_info.value, device.y_range.minimum,
                                                   device.y_range.maximum, size.height);
                }
            }
            devices_.push_back(std::move(device));
        }
        if (devices_.empty()) {
            return drm_input::set_error(
                error, first_failure.empty() ? "no usable input devices under /dev/input"
                                             : first_failure);
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

            const Size size = display_.size();
            bool produced = false;
            for (std::size_t i = 0; i < descriptors.size(); ++i) {
                if (!(descriptors[i].revents & (POLLIN | POLLERR | POLLHUP))) {
                    continue;
                }
                Device& device = devices_[indices[i]];
                read_device(device, size);
                produced = produced || !device.events.empty();
            }
            if (!produced) {
                return false;
            }
        }
    }

private:
    void read_device(Device& device, Size size) {
        input_event events[64];
        for (;;) {
            const ssize_t bytes = read(device.fd, events, sizeof(events));
            if (bytes < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    log::warn("input: %s disconnected: %s", device.path.c_str(), std::strerror(errno));
                    close_device(device);
                }
                return;
            }
            if (bytes == 0) {
                return;
            }
            const std::size_t count = static_cast<std::size_t>(bytes) / sizeof(input_event);
            for (std::size_t i = 0; i < count; ++i) {
                handle_event(device, events[i], size);
            }
            if (static_cast<std::size_t>(bytes) < sizeof(events)) {
                return;
            }
        }
    }

    const Display& display_;
    std::vector<Device> devices_;
};

}  // namespace

std::unique_ptr<InputSource> create_evdev_input(const Display& display, std::string* error) {
    auto input = std::make_unique<EvdevInput>(display);
    if (!input->open_devices(error)) {
        return nullptr;
    }
    return input;
}

}  // namespace pk
