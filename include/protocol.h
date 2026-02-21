#pragma once

#include "types.h"
#include <cstdint>
#include <cstring>

namespace ob {

// Fixed-size binary messages — no parsing overhead, no heap allocation.

enum class MsgType : uint8_t {
    NEW_ORDER = 1,
    CANCEL    = 2,
    ACK       = 10,
    FILL      = 11,
    REJECT    = 12,
};

// Client → Server: 32 bytes
struct OrderMessage {
    uint8_t  msg_type;      // MsgType
    uint8_t  side;          // Side enum
    uint8_t  order_type;    // OrderType enum
    uint8_t  padding[5];
    uint64_t order_id;      // For CANCEL: id to cancel. For NEW_ORDER: ignored (server assigns)
    int64_t  price;         // Fixed-point
    uint32_t quantity;
    uint32_t reserved;
};
static_assert(sizeof(OrderMessage) == 32, "OrderMessage must be 32 bytes");

// Server → Client: 32 bytes
struct ResponseMessage {
    uint8_t  msg_type;      // MsgType: ACK, FILL, REJECT
    uint8_t  padding[3];
    uint32_t quantity;      // For FILL: fill qty
    uint64_t order_id;      // The order this response refers to
    int64_t  price;         // For FILL: fill price
    uint64_t match_id;      // For FILL: counterparty order id
};
static_assert(sizeof(ResponseMessage) == 32, "ResponseMessage must be 32 bytes");

// Serialize/deserialize via memcpy (trivially copyable structs)
inline void serialize(const OrderMessage& msg, char* buf) {
    std::memcpy(buf, &msg, sizeof(msg));
}

inline void deserialize(const char* buf, OrderMessage& msg) {
    std::memcpy(&msg, buf, sizeof(msg));
}

inline void serialize(const ResponseMessage& msg, char* buf) {
    std::memcpy(buf, &msg, sizeof(msg));
}

inline void deserialize(const char* buf, ResponseMessage& msg) {
    std::memcpy(&msg, buf, sizeof(msg));
}

} // namespace ob
