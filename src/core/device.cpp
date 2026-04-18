#include "core/device.hpp"
#include "config/constants.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

namespace loginext::core {

namespace {

bool matches_product(uint16_t pid) noexcept {
    for (auto id : config::product_ids) {
        if (pid == id) return true;
    }
    return false;
}

// Fixed-size path buffer avoids heap allocation
struct PathBuf {
    char data[128];

    bool build(const char* dir, const char* name) noexcept {
        int n = std::snprintf(data, sizeof(data), "%s/%s", dir, name);
        return n > 0 && static_cast<size_t>(n) < sizeof(data);
    }
};

} // namespace

DeviceHandle find_device() noexcept {
    DIR* dir = opendir(config::input_dir);
    if (!dir) {
        std::fprintf(stderr, "[loginext] failed to open %s: %s\n",
                     config::input_dir, std::strerror(errno));
        return {};
    }

    DeviceHandle result{};
    PathBuf path{};

    while (const dirent* entry = readdir(dir)) {
        // Only care about eventN nodes
        if (std::strncmp(entry->d_name, "event", 5) != 0) continue;
        if (!path.build(config::input_dir, entry->d_name)) continue;

        int fd = open(path.data, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        libevdev* evdev = nullptr;
        if (libevdev_new_from_fd(fd, &evdev) < 0) {
            close(fd);
            continue;
        }

        uint16_t vid = static_cast<uint16_t>(libevdev_get_id_vendor(evdev));
        uint16_t pid = static_cast<uint16_t>(libevdev_get_id_product(evdev));

        if (vid == config::vendor_id && matches_product(pid)) {
            // Consumer Control node also reports REL_HWHEEL —
            // require the full mouse axis set to pick the real mouse node
            if (libevdev_has_event_code(evdev, EV_REL, REL_X) &&
                libevdev_has_event_code(evdev, EV_REL, REL_Y) &&
                libevdev_has_event_code(evdev, EV_REL, REL_HWHEEL)) {
                result.fd = fd;
                result.evdev = evdev;
                std::fprintf(stderr, "[loginext] found MX Master 3S at %s (%s)\n",
                             path.data, libevdev_get_name(evdev));
                break;
            }
        }

        libevdev_free(evdev);
        close(fd);
    }

    closedir(dir);
    return result;
}

int grab_device(DeviceHandle& dev) noexcept {
    int rc = libevdev_grab(dev.evdev, LIBEVDEV_GRAB);
    if (rc < 0) {
        std::fprintf(stderr, "[loginext] grab failed: %s\n", std::strerror(-rc));
    } else {
        std::fprintf(stderr, "[loginext] exclusive grab acquired\n");
    }
    return rc;
}

void release_device(DeviceHandle& dev) noexcept {
    if (dev.evdev) {
        libevdev_grab(dev.evdev, LIBEVDEV_UNGRAB);
        libevdev_free(dev.evdev);
        dev.evdev = nullptr;
    }
    if (dev.fd >= 0) {
        close(dev.fd);
        dev.fd = -1;
    }
    std::fprintf(stderr, "[loginext] device released\n");
}

} // namespace loginext::core
