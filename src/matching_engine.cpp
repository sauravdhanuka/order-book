#include "matching_engine.h"

namespace ob {

std::vector<Trade> MatchingEngine::process_order(
    Side side, OrderType type, Price price, Quantity quantity)
{
    ++orders_processed_;

    Order* order = pool_.allocate();
    order->id = next_order_id_++;
    order->timestamp = next_timestamp_++;
    order->price = price;
    order->quantity = quantity;
    order->filled_qty = 0;
    order->side = side;
    order->type = type;

    std::vector<Trade> trades;

    if (side == Side::BUY) {
        match_buy(order, trades);
    } else {
        match_sell(order, trades);
    }

    // If the order has remaining quantity, add to book (limit orders only)
    if (!order->is_filled()) {
        if (type == OrderType::LIMIT) {
            book_.add_order(order);
        } else {
            // Market order with unfilled remainder — reject/discard
            pool_.deallocate(order);
        }
    } else {
        // Fully filled — return to pool
        pool_.deallocate(order);
    }

    return trades;
}

bool MatchingEngine::cancel_order(OrderId order_id) {
    Order* order = book_.cancel_order(order_id);
    if (order) {
        pool_.deallocate(order);
        return true;
    }
    return false;
}

void MatchingEngine::match_buy(Order* incoming, std::vector<Trade>& trades) {
    auto& asks = book_.asks();

    while (!incoming->is_filled() && !asks.empty()) {
        auto it = asks.begin();
        Price ask_price = it->first;

        // For limit orders, stop if ask price exceeds our limit
        if (incoming->type == OrderType::LIMIT && ask_price > incoming->price) {
            break;
        }

        PriceLevel& level = it->second;

        while (!incoming->is_filled() && !level.is_empty()) {
            Order* resting = level.front();
            Quantity fill_qty = std::min(incoming->remaining(), resting->remaining());

            trades.push_back(execute_trade(incoming, resting, fill_qty, ask_price));

            if (resting->is_filled()) {
                level.pop_front();
                // Only remove from lookup — don't use cancel_order which also
                // manipulates the level/map we're iterating over
                book_.remove_from_lookup(resting->id);
                pool_.deallocate(resting);
            } else {
                level.reduce_quantity(fill_qty);
            }
        }

        if (level.is_empty()) {
            asks.erase(it);
        }
    }
}

void MatchingEngine::match_sell(Order* incoming, std::vector<Trade>& trades) {
    auto& bids = book_.bids();

    while (!incoming->is_filled() && !bids.empty()) {
        auto it = bids.begin();
        Price bid_price = it->first;

        // For limit orders, stop if bid price is below our limit
        if (incoming->type == OrderType::LIMIT && bid_price < incoming->price) {
            break;
        }

        PriceLevel& level = it->second;

        while (!incoming->is_filled() && !level.is_empty()) {
            Order* resting = level.front();
            Quantity fill_qty = std::min(incoming->remaining(), resting->remaining());

            trades.push_back(execute_trade(resting, incoming, fill_qty, bid_price));

            if (resting->is_filled()) {
                level.pop_front();
                book_.remove_from_lookup(resting->id);
                pool_.deallocate(resting);
            } else {
                level.reduce_quantity(fill_qty);
            }
        }

        if (level.is_empty()) {
            bids.erase(it);
        }
    }
}

Trade MatchingEngine::execute_trade(Order* buyer, Order* seller, Quantity qty, Price price) {
    buyer->filled_qty += qty;
    seller->filled_qty += qty;
    ++trade_count_;

    return Trade{
        .buyer_order_id = buyer->id,
        .seller_order_id = seller->id,
        .price = price,
        .quantity = qty,
        .timestamp = next_timestamp_++
    };
}

} // namespace ob
