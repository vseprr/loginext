#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>

// FNV-1a 32-bit. The hot path never sees a string — it sees the 32-bit hash
// the listener thread already computed for the focused window's app id (X11
// WM_CLASS, Hyprland window class, …). All comparisons on the event loop are
// integer compares against this single hash; std::string and friends never
// appear past the config-load boundary. See OPTIMIZATIONS.md §"Per-app scope".
//
// Hash 0 is reserved as "no specific app / global rule applies" so the
// hot-path lookup can short-circuit on it without a separate flag load.

namespace loginext::scope {

constexpr uint32_t fnv_offset = 0x811c9dc5u;
constexpr uint32_t fnv_prime  = 0x01000193u;

constexpr uint32_t hash_app(const char* s, std::size_t n) noexcept {
    uint32_t h = fnv_offset;
    for (std::size_t i = 0; i < n; ++i) {
        // Lower-case ASCII so "Firefox" / "firefox" / "FIREFOX" all collide.
        // Non-ASCII bytes pass through unchanged — fine for class names.
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + 32);
        h ^= static_cast<uint32_t>(c);
        h *= fnv_prime;
    }
    if (h == 0) h = fnv_prime;  // 0 is the "global" sentinel, never collide
    return h;
}

constexpr uint32_t hash_app(const char* s) noexcept {
    std::size_t n = 0;
    while (s[n] != '\0') ++n;
    return hash_app(s, n);
}

} // namespace loginext::scope
