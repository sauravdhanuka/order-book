#pragma once

#include "types.h"
#include "order.h"
#include "trade.h"
#include "order_book.h"
#include "object_pool.h"
#include <vector>
#include <cstdint>

namespace ob {

class MatchingEngine {
public:
    // Process an incoming order: match against resting orders, return trades.
    // Unmatched remainder of limit orders is added to the book.
    // The engine takes ownership of Order allocation via the object pool.
    std::vector<Trade> process_order(Side side, OrderType type, Price price, Quantity quantity);

    // Cancel an order by ID
    bool cancel_order(OrderId order_id);

    // Access to the book (for printing, queries)
    const OrderBook& book() const { return book_; }
    OrderBook& book() { return book_; }

    // Stats
    uint64_t next_order_id() const { return next_order_id_; }
    uint64_t trade_count() const { return trade_count_; }
    uint64_t orders_processed() const { return orders_processed_; }

    // Access to pool (for benchmarking comparisons)
    const ObjectPool<Order>& pool() const { return pool_; }

private:
    OrderBook book_;
    ObjectPool<Order> pool_;
    OrderId next_order_id_ = 1;
    Timestamp next_timestamp_ = 1;
    uint64_t trade_count_ = 0;
    uint64_t orders_processed_ = 0;

    // Match an incoming buy order against the ask side
    void match_buy(Order* order, std::vector<Trade>& trades);

    // Match an incoming sell order against the bid side
    void match_sell(Order* order, std::vector<Trade>& trades);

    // Execute a trade between two orders
    Trade execute_trade(Order* buyer, Order* seller, Quantity qty, Price price);
};

} // namespace ob
