#pragma once

#include <cstdint>
#include <cinttypes>
#include <string>
#include <cstdio>

namespace ob {

// Fixed-point price in ticks. E.g., 150.25 with tick_size=100 → 15025.
// Avoids floating-point comparison bugs.
using Price = int64_t;

constexpr int PRICE_SCALE = 100; // 2 decimal places

inline Price price_from_double(double p) {
    return static_cast<Price>(p * PRICE_SCALE + 0.5);
}

inline double price_to_double(Price p) {
    return static_cast<double>(p) / PRICE_SCALE;
}

inline std::string price_to_string(Price p) {
    int64_t whole = p / PRICE_SCALE;
    int64_t frac = p % PRICE_SCALE;
    if (frac < 0) frac = -frac;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%" PRId64 ".%02" PRId64, whole, frac);
    return buf;
}

enum class Side : uint8_t {
    BUY  = 0,
    SELL = 1
};

enum class OrderType : uint8_t {
    LIMIT  = 0,
    MARKET = 1
};

using OrderId    = uint64_t;
using Quantity   = uint32_t;
using Timestamp  = uint64_t;

} // namespace ob
