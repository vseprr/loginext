#pragma once

#include <cstdint>

namespace loginext::config {

// Logitech MX Master 3S identification
// Bolt receiver reports 046d:b034, USB wired reports 046d:c548
constexpr uint16_t vendor_id  = 0x046d;
constexpr uint16_t product_ids[] = {0xb034, 0xc548};

constexpr const char* input_dir = "/dev/input";

// epoll tuning
constexpr int max_epoll_events = 8;

// Ring buffer capacity for queued actions (structural, not behavioral)
constexpr int max_queued_actions = 8;

} // namespace loginext::config
