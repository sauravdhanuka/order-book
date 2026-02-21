#include "order_book.h"
#include <iostream>
#include <iomanip>

namespace ob {

void OrderBook::add_order(Order* order) {
    order_lookup_[order->id] = order;

    if (order->side == Side::BUY) {
        bids_[order->price].add(order);
    } else {
        asks_[order->price].add(order);
    }
}

Order* OrderBook::cancel_order(OrderId order_id) {
    auto it = order_lookup_.find(order_id);
    if (it == order_lookup_.end()) {
        return nullptr;
    }

    Order* order = it->second;
    order_lookup_.erase(it);

    if (order->side == Side::BUY) {
        auto level_it = bids_.find(order->price);
        if (level_it != bids_.end()) {
            level_it->second.remove(order);
            if (level_it->second.is_empty()) {
                bids_.erase(level_it);
            }
        }
    } else {
        auto level_it = asks_.find(order->price);
        if (level_it != asks_.end()) {
            level_it->second.remove(order);
            if (level_it->second.is_empty()) {
                asks_.erase(level_it);
            }
        }
    }

    return order;
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

Quantity OrderBook::get_volume_at_price(Side side, Price price) const {
    if (side == Side::BUY) {
        auto it = bids_.find(price);
        return it != bids_.end() ? it->second.total_quantity() : 0;
    } else {
        auto it = asks_.find(price);
        return it != asks_.end() ? it->second.total_quantity() : 0;
    }
}

bool OrderBook::has_order(OrderId id) const {
    return order_lookup_.count(id) > 0;
}

void OrderBook::remove_from_lookup(OrderId id) {
    order_lookup_.erase(id);
}

void OrderBook::remove_empty_level(Side side, Price price) {
    if (side == Side::BUY) {
        auto it = bids_.find(price);
        if (it != bids_.end() && it->second.is_empty()) {
            bids_.erase(it);
        }
    } else {
        auto it = asks_.find(price);
        if (it != asks_.end() && it->second.is_empty()) {
            asks_.erase(it);
        }
    }
}

void OrderBook::print(std::ostream& os) const {
    os << "=== ORDER BOOK ===\n";
    os << "--- ASKS (lowest first) ---\n";

    // Print asks in reverse (highest to lowest) for visual display
    std::vector<std::pair<Price, const PriceLevel*>> ask_levels;
    for (const auto& [price, level] : asks_) {
        ask_levels.emplace_back(price, &level);
    }
    for (auto it = ask_levels.rbegin(); it != ask_levels.rend(); ++it) {
        os << "  " << std::setw(10) << price_to_string(it->first)
           << "  |  " << std::setw(8) << it->second->total_quantity()
           << "  (" << it->second->order_count() << " orders)\n";
    }

    os << "--- SPREAD ---\n";

    os << "--- BIDS (highest first) ---\n";
    for (const auto& [price, level] : bids_) {
        os << "  " << std::setw(10) << price_to_string(price)
           << "  |  " << std::setw(8) << level.total_quantity()
           << "  (" << level.order_count() << " orders)\n";
    }
    os << "==================\n";
}

} // namespace ob
