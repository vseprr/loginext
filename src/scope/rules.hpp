#pragma once

#include "presets/preset.hpp"

#include <cstddef>
#include <cstdint>

namespace loginext::scope {

// Fixed-capacity, open-addressing hash table of (app_hash → PresetId).
// Capacity is a power of two so the index step is a mask, not a mod.
// All hot-path lookups are pure integer arithmetic — no allocation, no
// string compare, no STL container.
//
// 64 slots covers the realistic ceiling (a user is unlikely to hand-write
// rules for more than ~30 distinct apps); load factor stays < 0.5 and the
// expected probe length stays at 1.x.
constexpr std::size_t rule_capacity = 64;
static_assert((rule_capacity & (rule_capacity - 1)) == 0,
              "rule_capacity must be a power of two");

struct AppRule {
    uint32_t          app_hash;   // 0 = empty slot
    presets::PresetId preset;
};

struct RuleTable {
    AppRule slots[rule_capacity]{};
    uint8_t count = 0;            // populated entries
};

// Reset to empty. Cheap; called on config reload before re-loading rules.
void clear(RuleTable& t) noexcept;

// Insert or overwrite. Returns false if the table is full or `app_hash` is 0
// (the global sentinel). Safe only off the hot path (load / reload).
bool insert(RuleTable& t, uint32_t app_hash, presets::PresetId p) noexcept;

// O(1) hot-path lookup. Returns true iff a rule exists for this hash;
// `out` receives the preset on hit and is left untouched on miss.
// noexcept + `inline` so the compiler can fold this into the on_event path.
[[nodiscard]] inline bool lookup(const RuleTable& t,
                                 uint32_t app_hash,
                                 presets::PresetId& out) noexcept {
    if (app_hash == 0) return false;            // global sentinel short-circuit
    std::size_t mask = rule_capacity - 1;
    std::size_t i    = app_hash & mask;
    // Bounded probe: at worst we scan the whole table, but capacity is
    // 64 entries and load factor is enforced low at insert-time.
    for (std::size_t step = 0; step < rule_capacity; ++step) {
        const AppRule& r = t.slots[(i + step) & mask];
        if (r.app_hash == 0)        return false;       // empty → terminate
        if (r.app_hash == app_hash) { out = r.preset; return true; }
    }
    return false;
}

} // namespace loginext::scope
