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

// Maximum keys per combo + 1 SYN. Matches presets::KeyCombo::keys.
constexpr int max_combo_events = 5;

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

    // Register the union of keys every shipping preset can emit, derived
    // straight from the preset table. uinput silently drops events for keys
    // the device never declared at UI_SET_KEYBIT time — so a preset whose
    // keys aren't enumerated here looks like "nothing happens" with no
    // syscall failure to log. Iterating the table at init guarantees a new
    // preset added to preset.hpp can never silently break the emitter.
    // Hot path is unaffected: this runs once during init_emitter().
    for (uint8_t i = 0; i < presets::preset_count; ++i) {
        const presets::Preset& p = presets::preset_for(static_cast<presets::PresetId>(i));
        for (const auto& combo : { p.on_left, p.on_right }) {
            for (uint8_t k = 0; k < combo.count; ++k) {
                uint16_t code = combo.keys[k];
                if (code == 0) continue;
                if (ioctl(fd, UI_SET_KEYBIT, code) < 0) return -1;
            }
        }
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

// Single write() emits all KEY events + SYN_REPORT atomically — preserves
// the original NBT timing where the entire combo lands in one input frame.
void press_combo(int fd, const presets::KeyCombo& combo) noexcept {
    if (combo.count == 0) return;
    input_event batch[max_combo_events];
    int n = 0;
    for (uint8_t i = 0; i < combo.count; ++i) {
        batch[n] = {};
        batch[n].type  = EV_KEY;
        batch[n].code  = combo.keys[i];
        batch[n].value = 1;
        ++n;
    }
    batch[n] = {};
    batch[n].type = EV_SYN;
    batch[n].code = SYN_REPORT;
    ++n;

    ssize_t r = write(fd, batch, static_cast<size_t>(n) * sizeof(input_event));
    if (r < 0) {
        std::fprintf(stderr, "[loginext] key press write failed: %s\n", std::strerror(errno));
    }
}

void release_combo(int fd, const presets::KeyCombo& combo) noexcept {
    if (combo.count == 0) return;
    input_event batch[max_combo_events];
    int n = 0;
    // Reverse order — releases the trigger key before its modifiers, which
    // matches how a human keyboardist (and the original NBT path) released
    // Ctrl+Tab.
    for (int i = combo.count - 1; i >= 0; --i) {
        batch[n] = {};
        batch[n].type  = EV_KEY;
        batch[n].code  = combo.keys[i];
        batch[n].value = 0;
        ++n;
    }
    batch[n] = {};
    batch[n].type = EV_SYN;
    batch[n].code = SYN_REPORT;
    ++n;

    ssize_t r = write(fd, batch, static_cast<size_t>(n) * sizeof(input_event));
    if (r < 0) {
        std::fprintf(stderr, "[loginext] key release write failed: %s\n", std::strerror(errno));
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

void emit_combo_down(EmitterHandle& em, const presets::KeyCombo& combo) noexcept {
    press_combo(em.kbd_fd, combo);
}

void emit_combo_up(EmitterHandle& em, const presets::KeyCombo& combo) noexcept {
    release_combo(em.kbd_fd, combo);
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
