#pragma once

#include "heuristics/scroll_state.hpp"

#include <cstddef>
#include <cstdint>
#include <linux/input-event-codes.h>

// ─────────────────────────────────────────────────────────────────────
//  Preset / Action layer.
//
//  A "preset" is a pure mapping from a heuristic Direction tick onto a
//  concrete output (today: a key combo). The heuristic engine never
//  references this layer — its only output is loginext::heuristics::Direction.
//
//  Adding a new preset is strictly additive:
//    1. Add an id to PresetId.
//    2. Add a constexpr Preset definition below.
//    3. Extend the switch in preset_for() — O(1) dispatch.
//
//  The NBT mapping (Ctrl+Tab / Ctrl+Shift+Tab) lives entirely here. Any
//  edit to NBT is contained to this file. The "feel" (timings, thresholds,
//  pacing) is governed by config::Profile and is untouched.
// ─────────────────────────────────────────────────────────────────────

namespace loginext::presets {

// Stable numeric ids — used as a flat-table index. Insert new presets at
// the end so existing config files keep their semantics.
enum class PresetId : uint8_t {
    TabNav = 0,
    Count,                        // sentinel; not a real preset
};
constexpr uint8_t preset_count = static_cast<uint8_t>(PresetId::Count);

// Up to four keys (modifiers + key) emitted as a single combo. The
// emitter presses them in array order on key-down and releases in
// reverse on key-up — matches the original NBT timing exactly.
struct KeyCombo {
    uint16_t keys[4];
    uint8_t  count;               // 0 = no-op (treated as silently dropped)
};

// One preset = one Direction → KeyCombo mapping. Future presets that
// need richer behaviour (e.g. mouse-button emission, command exec) will
// extend this with new variant arms; for now every shipping preset
// fits the key-combo model.
struct Preset {
    KeyCombo on_left;             // emitted on Direction::Left
    KeyCombo on_right;            // emitted on Direction::Right
};

// Identity / display strings. Kept here so the IPC surface stays in lockstep
// with the table below; touching one requires touching the other.
constexpr const char* preset_id_str(PresetId p) noexcept {
    switch (p) {
        case PresetId::TabNav: return "tab_nav";
        case PresetId::Count:  break;
    }
    return "tab_nav";
}

constexpr const char* preset_name(PresetId p) noexcept {
    switch (p) {
        case PresetId::TabNav: return "Navigate between tabs";
        case PresetId::Count:  break;
    }
    return "Navigate between tabs";
}

// ── Preset definitions ────────────────────────────────────────────────

// Navigate Between Tabs — original behaviour preserved exactly.
//   right tick → Ctrl+Tab          (forward)
//   left  tick → Ctrl+Shift+Tab    (back)
constexpr Preset preset_tab_nav = {
    .on_left  = { { KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_TAB, 0 }, 3 },
    .on_right = { { KEY_LEFTCTRL, KEY_TAB, 0, 0 },             2 },
};

// O(1) lookup. Falls back to NBT on out-of-range to keep the hot path
// branch-free of error handling — the value comes from a uint8_t enum
// validated at config-load time.
constexpr const Preset& preset_for(PresetId id) noexcept {
    switch (id) {
        case PresetId::TabNav: return preset_tab_nav;
        case PresetId::Count:  break;
    }
    return preset_tab_nav;
}

// Resolve a Direction tick under a given preset to a KeyCombo. Returns a
// zero-count combo for Direction::None so the caller can branch once at
// the enqueue site instead of inside this hot helper.
constexpr KeyCombo resolve(PresetId id, heuristics::Direction dir) noexcept {
    const Preset& p = preset_for(id);
    switch (dir) {
        case heuristics::Direction::Left:  return p.on_left;
        case heuristics::Direction::Right: return p.on_right;
        case heuristics::Direction::None:  break;
    }
    return KeyCombo{ {0,0,0,0}, 0 };
}

// Parse a preset id from its string form (used by the JSON config loader
// and the IPC `set_preset`). Returns false on miss; out is unchanged.
constexpr bool preset_id_from_str(const char* s, std::size_t n, PresetId& out) noexcept {
    auto eq = [&](const char* lit) noexcept {
        std::size_t i = 0;
        while (i < n && lit[i] != '\0' && lit[i] == s[i]) ++i;
        return i == n && lit[i] == '\0';
    };
    if (eq("tab_nav")) { out = PresetId::TabNav; return true; }
    return false;
}

} // namespace loginext::presets
