#pragma once

#include <libevdev/libevdev.h>

namespace loginext::core {

struct DeviceHandle {
    int fd = -1;
    libevdev* evdev = nullptr;

    [[nodiscard]] bool valid() const noexcept { return fd >= 0 && evdev != nullptr; }
};

// Scan /dev/input/event* for the MX Master 3S by vendor/product ID.
// Returns a populated handle on success, an invalid handle on failure.
[[nodiscard]] DeviceHandle find_device() noexcept;

// Exclusive grab — prevents other consumers from seeing raw events.
[[nodiscard]] int grab_device(DeviceHandle& dev) noexcept;

// Ungrab, free evdev, close fd.
void release_device(DeviceHandle& dev) noexcept;

} // namespace loginext::core
