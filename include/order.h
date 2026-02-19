#pragma once

#include "types.h"

namespace ob {

// Compact order struct — fits in one cache line (~64 bytes).
struct Order {
    OrderId   id;           // 8 bytes
    Timestamp timestamp;    // 8 bytes
    Price     price;        // 8 bytes (fixed-point)
    Quantity  quantity;     // 4 bytes (original quantity)
    Quantity  filled_qty;  // 4 bytes
    Side      side;        // 1 byte
    OrderType type;        // 1 byte
    uint8_t   padding[30]; // pad to 64 bytes

    Quantity remaining() const { return quantity - filled_qty; }
    bool is_filled() const { return filled_qty >= quantity; }
};

static_assert(sizeof(Order) == 64, "Order must be 64 bytes for cache-line alignment");

} // namespace ob
