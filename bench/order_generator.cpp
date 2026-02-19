#include "order_generator.h"

namespace ob {

OrderGenerator::OrderGenerator(uint64_t seed) : rng_(seed) {}

std::vector<GeneratedOrder> OrderGenerator::generate(
    size_t count, int cancel_pct, int market_pct,
    Price center_price, int spread_ticks)
{
    std::vector<GeneratedOrder> orders;
    orders.reserve(count);

    std::uniform_int_distribution<int> pct_dist(0, 99);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> price_dist(-spread_ticks, spread_ticks);
    std::uniform_int_distribution<Quantity> qty_dist(1, 1000);
    std::uniform_int_distribution<OrderId> id_dist(1, 1); // updated per iteration

    OrderId max_id = 0;

    for (size_t i = 0; i < count; ++i) {
        GeneratedOrder order{};
        order.is_cancel = false;

        // Decide if this is a cancel
        if (max_id > 0 && pct_dist(rng_) < cancel_pct) {
            order.is_cancel = true;
            std::uniform_int_distribution<OrderId> cancel_dist(1, max_id);
            order.cancel_id = cancel_dist(rng_);
            orders.push_back(order);
            continue;
        }

        order.side = side_dist(rng_) ? Side::BUY : Side::SELL;
        order.quantity = qty_dist(rng_);

        if (pct_dist(rng_) < market_pct) {
            order.type = OrderType::MARKET;
            order.price = 0;
        } else {
            order.type = OrderType::LIMIT;
            Price offset = price_dist(rng_);
            order.price = center_price + offset;
        }

        orders.push_back(order);
        ++max_id;
    }

    return orders;
}

} // namespace ob
