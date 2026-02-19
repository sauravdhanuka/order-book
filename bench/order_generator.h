#pragma once

#include "types.h"
#include <vector>
#include <random>

namespace ob {

struct GeneratedOrder {
    Side side;
    OrderType type;
    Price price;
    Quantity quantity;
    bool is_cancel;
    OrderId cancel_id;
};

class OrderGenerator {
public:
    explicit OrderGenerator(uint64_t seed = 42);

    // Generate a batch of random orders
    // cancel_pct: 0-100, what % are cancel orders
    // market_pct: 0-100, what % of new orders are market (vs limit)
    std::vector<GeneratedOrder> generate(
        size_t count,
        int cancel_pct = 5,
        int market_pct = 10,
        Price center_price = 10000,
        int spread_ticks = 100);

private:
    std::mt19937_64 rng_;
};

} // namespace ob
