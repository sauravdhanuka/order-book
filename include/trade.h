#pragma once

#include "types.h"

namespace ob {

struct Trade {
    OrderId   buyer_order_id;
    OrderId   seller_order_id;
    Price     price;
    Quantity  quantity;
    Timestamp timestamp;
};

} // namespace ob
