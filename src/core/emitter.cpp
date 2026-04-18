#include "core/emitter.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <linux/input.h>
#include <linux/uinput.h>
#include <unistd.h>

namespace loginext::core {

namespace {

int open_uinput() noexcept {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        std::fprintf(stderr, "[loginext] failed to open /dev/uinput: %s\n", std::strerror(errno));
    }
    return fd;
}

int setup_keyboard(int fd) noexcept {
    // Enable EV_KEY
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) return -1;

    // Register only the keys we need
    for (int key : {KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_TAB}) {
        if (ioctl(fd, UI_SET_KEYBIT, key) < 0) return -1;
    }

    uinput_setup setup{};
    std::strncpy(setup.name, "loginext-kbd", sizeof(setup.name) - 1);
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor  = 0x1234;
    setup.id.product = 0x0001;
    setup.id.version = 1;

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) return -1;
    if (ioctl(fd, UI_DEV_CREATE) < 0) return -1;

    return 0;
}

int setup_mouse(int fd) noexcept {
    // Enable EV_KEY for mouse buttons
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) return -1;
    for (int btn : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA}) {
        if (ioctl(fd, UI_SET_KEYBIT, btn) < 0) return -1;
    }

    // Enable EV_REL for mouse axes
    if (ioctl(fd, UI_SET_EVBIT, EV_REL) < 0) return -1;
    for (int rel : {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL,
                    REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES}) {
        if (ioctl(fd, UI_SET_RELBIT, rel) < 0) return -1;
    }

    // Enable EV_MSC for scan codes (passthrough from physical device)
    if (ioctl(fd, UI_SET_EVBIT, EV_MSC) < 0) return -1;
    if (ioctl(fd, UI_SET_MSCBIT, MSC_SCAN) < 0) return -1;

    uinput_setup setup{};
    std::strncpy(setup.name, "loginext-mouse", sizeof(setup.name) - 1);
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor  = 0x1234;
    setup.id.product = 0x0002;
    setup.id.version = 1;

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) return -1;
    if (ioctl(fd, UI_DEV_CREATE) < 0) return -1;

    return 0;
}

// Press key down, then up, with SYN between — batched into a single write()
void tap_key_combo(int fd, const int* keys, int count) noexcept {
    // Max: 3 keys down + SYN + 3 keys up + SYN = 8 events
    input_event batch[8];
    int n = 0;

    // Press all keys
    for (int i = 0; i < count; ++i) {
        batch[n] = {};
        batch[n].type  = EV_KEY;
        batch[n].code  = static_cast<uint16_t>(keys[i]);
        batch[n].value = 1;
        ++n;
    }
    batch[n] = {};
    batch[n].type = EV_SYN;
    batch[n].code = SYN_REPORT;
    ++n;

    // Release all keys (reverse order)
    for (int i = count - 1; i >= 0; --i) {
        batch[n] = {};
        batch[n].type  = EV_KEY;
        batch[n].code  = static_cast<uint16_t>(keys[i]);
        batch[n].value = 0;
        ++n;
    }
    batch[n] = {};
    batch[n].type = EV_SYN;
    batch[n].code = SYN_REPORT;
    ++n;

    ssize_t r = write(fd, batch, static_cast<size_t>(n) * sizeof(input_event));
    if (r < 0) {
        std::fprintf(stderr, "[loginext] key combo write failed: %s\n", std::strerror(errno));
    }
}

} // namespace

int init_emitter(EmitterHandle& em) noexcept {
    em.kbd_fd = open_uinput();
    if (em.kbd_fd < 0) return -1;

    if (setup_keyboard(em.kbd_fd) < 0) {
        std::fprintf(stderr, "[loginext] keyboard uinput setup failed: %s\n", std::strerror(errno));
        close(em.kbd_fd);
        em.kbd_fd = -1;
        return -1;
    }

    em.mouse_fd = open_uinput();
    if (em.mouse_fd < 0) {
        ioctl(em.kbd_fd, UI_DEV_DESTROY);
        close(em.kbd_fd);
        em.kbd_fd = -1;
        return -1;
    }

    if (setup_mouse(em.mouse_fd) < 0) {
        std::fprintf(stderr, "[loginext] mouse uinput setup failed: %s\n", std::strerror(errno));
        ioctl(em.kbd_fd, UI_DEV_DESTROY);
        close(em.kbd_fd);
        em.kbd_fd = -1;
        close(em.mouse_fd);
        em.mouse_fd = -1;
        return -1;
    }

    // Give uinput devices time to register with the input subsystem
    usleep(100'000);  // 100ms — only at init, not hot path

    std::fprintf(stderr, "[loginext] uinput emitter initialized (kbd + mouse)\n");
    return 0;
}

void emit_tab_next(EmitterHandle& em) noexcept {
    const int keys[] = {KEY_LEFTCTRL, KEY_TAB};
    tap_key_combo(em.kbd_fd, keys, 2);
}

void emit_tab_prev(EmitterHandle& em) noexcept {
    const int keys[] = {KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_TAB};
    tap_key_combo(em.kbd_fd, keys, 3);
}

void emit_action(EmitterHandle& em, loginext::heuristics::ActionResult action) noexcept {
    switch (action) {
        case loginext::heuristics::ActionResult::TabNext:
            emit_tab_next(em);
            break;
        case loginext::heuristics::ActionResult::TabPrev:
            emit_tab_prev(em);
            break;
        case loginext::heuristics::ActionResult::None:
            break;
    }
}

void emit_passthrough(EmitterHandle& em, const struct input_event& ev) noexcept {
    ssize_t r = write(em.mouse_fd, &ev, sizeof(ev));
    if (r < 0 && errno != EAGAIN) {
        std::fprintf(stderr, "[loginext] passthrough write failed: %s\n", std::strerror(errno));
    }
}

void destroy_emitter(EmitterHandle& em) noexcept {
    if (em.kbd_fd >= 0) {
        ioctl(em.kbd_fd, UI_DEV_DESTROY);
        close(em.kbd_fd);
        em.kbd_fd = -1;
    }
    if (em.mouse_fd >= 0) {
        ioctl(em.mouse_fd, UI_DEV_DESTROY);
        close(em.mouse_fd);
        em.mouse_fd = -1;
    }
    std::fprintf(stderr, "[loginext] uinput emitter destroyed\n");
}

} // namespace loginext::core
