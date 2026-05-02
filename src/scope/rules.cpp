#include "scope/rules.hpp"

#include <cstring>

namespace loginext::scope {

void clear(RuleTable& t) noexcept {
    for (std::size_t i = 0; i < rule_capacity; ++i) {
        t.slots[i].app_hash = 0;
    }
    t.count = 0;
}

bool insert(RuleTable& t, uint32_t app_hash, presets::PresetId p) noexcept {
    if (app_hash == 0) return false;                 // sentinel collision
    if (t.count >= rule_capacity / 2) return false;  // keep load factor < 0.5

    std::size_t mask = rule_capacity - 1;
    std::size_t i    = app_hash & mask;
    for (std::size_t step = 0; step < rule_capacity; ++step) {
        AppRule& r = t.slots[(i + step) & mask];
        if (r.app_hash == 0) {
            r.app_hash = app_hash;
            r.preset   = p;
            ++t.count;
            return true;
        }
        if (r.app_hash == app_hash) {                // overwrite duplicates
            r.preset = p;
            return true;
        }
    }
    return false;
}

} // namespace loginext::scope
