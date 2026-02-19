#pragma once

#include "order.h"
#include <deque>
#include <algorithm>

namespace ob {

// All orders resting at a single price point, in FIFO (time-priority) order.
// Uses std::deque for cache-friendlier iteration than std::list.
class PriceLevel {
public:
    void add(Order* order) {
        orders_.push_back(order);
        total_qty_ += order->remaining();
    }

    Order* front() const {
        return orders_.front();
    }

    void pop_front() {
        if (!orders_.empty()) {
            total_qty_ -= orders_.front()->remaining();
            orders_.pop_front();
        }
    }

    // Remove a specific order (for cancellation). O(n) but cancels are less frequent.
    bool remove(Order* order) {
        auto it = std::find(orders_.begin(), orders_.end(), order);
        if (it != orders_.end()) {
            total_qty_ -= (*it)->remaining();
            orders_.erase(it);
            return true;
        }
        return false;
    }

    bool is_empty() const { return orders_.empty(); }

    Quantity total_quantity() const { return total_qty_; }

    size_t order_count() const { return orders_.size(); }

    // Update cached quantity after a partial fill on the front order
    void reduce_quantity(Quantity qty) {
        total_qty_ -= qty;
    }

private:
    std::deque<Order*> orders_;
    Quantity total_qty_ = 0;
};

} // namespace ob
