#pragma once

#include "types.h"
#include "order.h"
#include "price_level.h"
#include <map>
#include <unordered_map>
#include <optional>
#include <functional>

namespace ob {

class OrderBook {
public:
    // Add a resting order to the book
    void add_order(Order* order);

    // Cancel an order by ID. Returns the removed order (caller handles deallocation).
    Order* cancel_order(OrderId order_id);

    // Top-of-book access
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    // Volume at a specific price level
    Quantity get_volume_at_price(Side side, Price price) const;

    // Iterators for matching engine to walk the book
    // Bids: highest price first
    auto& bids() { return bids_; }
    const auto& bids() const { return bids_; }

    // Asks: lowest price first
    auto& asks() { return asks_; }
    const auto& asks() const { return asks_; }

    // Check if order exists in the book
    bool has_order(OrderId id) const;

    // Remove order from lookup only (used by matching engine which handles level cleanup itself)
    void remove_from_lookup(OrderId id);

    // Book statistics
    size_t bid_level_count() const { return bids_.size(); }
    size_t ask_level_count() const { return asks_.size(); }
    size_t total_order_count() const { return order_lookup_.size(); }

    // Print book state (for debugging / PRINT command)
    void print(std::ostream& os) const;

private:
    // Bids sorted highest-first, asks sorted lowest-first
    std::map<Price, PriceLevel, std::greater<>> bids_;
    std::map<Price, PriceLevel> asks_;

    // O(1) lookup for cancel
    std::unordered_map<OrderId, Order*> order_lookup_;

    void remove_empty_level(Side side, Price price);
};

} // namespace ob
