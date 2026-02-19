#include <gtest/gtest.h>
#include "order_book.h"
#include "object_pool.h"

using namespace ob;

class OrderBookTest : public ::testing::Test {
protected:
    ObjectPool<Order> pool;
    OrderBook book;

    Order* make_order(OrderId id, Side side, Price price, Quantity qty) {
        Order* o = pool.allocate();
        o->id = id;
        o->timestamp = id;
        o->price = price;
        o->quantity = qty;
        o->filled_qty = 0;
        o->side = side;
        o->type = OrderType::LIMIT;
        return o;
    }
};

TEST_F(OrderBookTest, EmptyBook) {
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.total_order_count(), 0);
}

TEST_F(OrderBookTest, AddBidOrder) {
    Order* o = make_order(1, Side::BUY, 10000, 100);
    book.add_order(o);

    EXPECT_EQ(book.best_bid().value(), 10000);
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.total_order_count(), 1);
    EXPECT_EQ(book.get_volume_at_price(Side::BUY, 10000), 100);
}

TEST_F(OrderBookTest, AddAskOrder) {
    Order* o = make_order(1, Side::SELL, 10100, 50);
    book.add_order(o);

    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.best_ask().value(), 10100);
    EXPECT_EQ(book.get_volume_at_price(Side::SELL, 10100), 50);
}

TEST_F(OrderBookTest, BestBidIsHighest) {
    book.add_order(make_order(1, Side::BUY, 10000, 100));
    book.add_order(make_order(2, Side::BUY, 10100, 100));
    book.add_order(make_order(3, Side::BUY, 9900, 100));

    EXPECT_EQ(book.best_bid().value(), 10100);
}

TEST_F(OrderBookTest, BestAskIsLowest) {
    book.add_order(make_order(1, Side::SELL, 10200, 100));
    book.add_order(make_order(2, Side::SELL, 10100, 100));
    book.add_order(make_order(3, Side::SELL, 10300, 100));

    EXPECT_EQ(book.best_ask().value(), 10100);
}

TEST_F(OrderBookTest, CancelOrder) {
    Order* o = make_order(1, Side::BUY, 10000, 100);
    book.add_order(o);
    EXPECT_EQ(book.total_order_count(), 1);

    Order* cancelled = book.cancel_order(1);
    EXPECT_EQ(cancelled, o);
    EXPECT_EQ(book.total_order_count(), 0);
    EXPECT_FALSE(book.best_bid().has_value());

    pool.deallocate(cancelled);
}

TEST_F(OrderBookTest, CancelNonExistent) {
    Order* cancelled = book.cancel_order(999);
    EXPECT_EQ(cancelled, nullptr);
}

TEST_F(OrderBookTest, VolumeAtPrice) {
    book.add_order(make_order(1, Side::BUY, 10000, 100));
    book.add_order(make_order(2, Side::BUY, 10000, 200));
    book.add_order(make_order(3, Side::BUY, 9900, 50));

    EXPECT_EQ(book.get_volume_at_price(Side::BUY, 10000), 300);
    EXPECT_EQ(book.get_volume_at_price(Side::BUY, 9900), 50);
    EXPECT_EQ(book.get_volume_at_price(Side::BUY, 9800), 0);
}

TEST_F(OrderBookTest, MultipleLevels) {
    book.add_order(make_order(1, Side::BUY, 10000, 100));
    book.add_order(make_order(2, Side::BUY, 9900, 200));
    book.add_order(make_order(3, Side::SELL, 10100, 150));
    book.add_order(make_order(4, Side::SELL, 10200, 250));

    EXPECT_EQ(book.bid_level_count(), 2);
    EXPECT_EQ(book.ask_level_count(), 2);
    EXPECT_EQ(book.total_order_count(), 4);
}

TEST_F(OrderBookTest, CancelRemovesEmptyLevel) {
    Order* o = make_order(1, Side::SELL, 10100, 100);
    book.add_order(o);
    EXPECT_EQ(book.ask_level_count(), 1);

    Order* cancelled = book.cancel_order(1);
    EXPECT_EQ(book.ask_level_count(), 0);
    pool.deallocate(cancelled);
}
