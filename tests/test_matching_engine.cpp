#include <gtest/gtest.h>
#include "matching_engine.h"

using namespace ob;

class MatchingEngineTest : public ::testing::Test {
protected:
    MatchingEngine engine;
};

// --- Basic Limit Order Matching ---

TEST_F(MatchingEngineTest, NoMatchWhenBookEmpty) {
    auto trades = engine.process_order(Side::BUY, OrderType::LIMIT, 10000, 100);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(engine.book().total_order_count(), 1);
}

TEST_F(MatchingEngineTest, LimitBuyMatchesSell) {
    engine.process_order(Side::SELL, OrderType::LIMIT, 10000, 100);
    auto trades = engine.process_order(Side::BUY, OrderType::LIMIT, 10000, 100);

    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].price, 10000);
    EXPECT_EQ(trades[0].quantity, 100);
    EXPECT_EQ(engine.book().total_order_count(), 0);
}

TEST_F(MatchingEngineTest, LimitSellMatchesBuy) {
    engine.process_order(Side::BUY, OrderType::LIMIT, 10000, 100);
    auto trades = engine.process_order(Side::SELL, OrderType::LIMIT, 10000, 100);

    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].price, 10000);
    EXPECT_EQ(trades[0].quantity, 100);
    EXPECT_EQ(engine.book().total_order_count(), 0);
}

TEST_F(MatchingEngineTest, BuyAtHigherPriceMatchesLowerAsk) {
    engine.process_order(Side::SELL, OrderType::LIMIT, 10000, 100);
    auto trades = engine.process_order(Side::BUY, OrderType::LIMIT, 10100, 100);

    ASSERT_EQ(trades.size(), 1);
    // Matches at resting order's price (price-time priority)
    EXPECT_EQ(trades[0].price, 10000);
    EXPECT_EQ(trades[0].quantity, 100);
}

TEST_F(MatchingEngineTest, NoMatchWhenPricesDontCross) {
    engine.process_order(Side::SELL, OrderType::LIMIT, 10100, 100);
    auto trades = engine.process_order(Side::BUY, OrderType::LIMIT, 10000, 100);

    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(engine.book().total_order_count(), 2);
}

// --- Partial Fills ---

TEST_F(MatchingEngineTest, PartialFillBuy) {
    engine.process_order(Side::SELL, OrderType::LIMIT, 10000, 50);
    auto trades = engine.process_order(Side::BUY, OrderType::LIMIT, 10000, 100);

    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].quantity, 50);
    // Remaining 50 should rest in the book
    EXPECT_EQ(engine.book().total_order_count(), 1);
    EXPECT_EQ(engine.book().get_volume_at_price(Side::BUY, 10000), 50);
}

TEST_F(MatchingEngineTest, PartialFillSell) {
    engine.process_order(Side::BUY, OrderType::LIMIT, 10000, 50);
    auto trades = engine.process_order(Side::SELL, OrderType::LIMIT, 10000, 100);

    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].quantity, 50);
    EXPECT_EQ(engine.book().total_order_count(), 1);
    EXPECT_EQ(engine.book().get_volume_at_price(Side::SELL, 10000), 50);
}

// --- Multi-level Matching ---

TEST_F(MatchingEngineTest, BuyMatchesMultipleAskLevels) {
    engine.process_order(Side::SELL, OrderType::LIMIT, 10000, 50);
    engine.process_order(Side::SELL, OrderType::LIMIT, 10100, 50);

    auto trades = engine.process_order(Side::BUY, OrderType::LIMIT, 10100, 100);

    ASSERT_EQ(trades.size(), 2);
    EXPECT_EQ(trades[0].price, 10000);
    EXPECT_EQ(trades[0].quantity, 50);
    EXPECT_EQ(trades[1].price, 10100);
    EXPECT_EQ(trades[1].quantity, 50);
    EXPECT_EQ(engine.book().total_order_count(), 0);
}

// --- Time Priority (FIFO) ---

TEST_F(MatchingEngineTest, FIFOWithinPriceLevel) {
    // First sell order (order_id = 1)
    engine.process_order(Side::SELL, OrderType::LIMIT, 10000, 100);
    // Second sell order (order_id = 2)
    engine.process_order(Side::SELL, OrderType::LIMIT, 10000, 100);

    auto trades = engine.process_order(Side::BUY, OrderType::LIMIT, 10000, 100);

    ASSERT_EQ(trades.size(), 1);
    // Should match with first order (id=1), not second
    EXPECT_EQ(trades[0].seller_order_id, 1);
}

// --- Market Orders ---

TEST_F(MatchingEngineTest, MarketBuyMatchesAllAvailable) {
    engine.process_order(Side::SELL, OrderType::LIMIT, 10000, 50);
    engine.process_order(Side::SELL, OrderType::LIMIT, 10100, 50);

    auto trades = engine.process_order(Side::BUY, OrderType::MARKET, 0, 100);

    ASSERT_EQ(trades.size(), 2);
    EXPECT_EQ(trades[0].quantity, 50);
    EXPECT_EQ(trades[1].quantity, 50);
    EXPECT_EQ(engine.book().total_order_count(), 0);
}

TEST_F(MatchingEngineTest, MarketSellMatchesBids) {
    engine.process_order(Side::BUY, OrderType::LIMIT, 10000, 100);
    auto trades = engine.process_order(Side::SELL, OrderType::MARKET, 0, 50);

    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].quantity, 50);
    // Remaining 50 on bid side
    EXPECT_EQ(engine.book().get_volume_at_price(Side::BUY, 10000), 50);
}

TEST_F(MatchingEngineTest, MarketOrderEmptyBookDoesNotRest) {
    auto trades = engine.process_order(Side::BUY, OrderType::MARKET, 0, 100);
    EXPECT_TRUE(trades.empty());
    // Market order should not rest in book
    EXPECT_EQ(engine.book().total_order_count(), 0);
}

TEST_F(MatchingEngineTest, MarketOrderPartialFillRemainsDiscarded) {
    engine.process_order(Side::SELL, OrderType::LIMIT, 10000, 30);
    auto trades = engine.process_order(Side::BUY, OrderType::MARKET, 0, 100);

    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].quantity, 30);
    // Unfilled remainder of market order is discarded
    EXPECT_EQ(engine.book().total_order_count(), 0);
}

// --- Cancel Orders ---

TEST_F(MatchingEngineTest, CancelExistingOrder) {
    engine.process_order(Side::BUY, OrderType::LIMIT, 10000, 100);
    EXPECT_EQ(engine.book().total_order_count(), 1);

    bool cancelled = engine.cancel_order(1);
    EXPECT_TRUE(cancelled);
    EXPECT_EQ(engine.book().total_order_count(), 0);
}

TEST_F(MatchingEngineTest, CancelNonExistentOrder) {
    bool cancelled = engine.cancel_order(999);
    EXPECT_FALSE(cancelled);
}

// --- Order ID Assignment ---

TEST_F(MatchingEngineTest, OrderIdsIncrement) {
    engine.process_order(Side::BUY, OrderType::LIMIT, 10000, 100);
    engine.process_order(Side::SELL, OrderType::LIMIT, 10100, 100);

    // These orders should have IDs 1 and 2
    EXPECT_TRUE(engine.book().has_order(1));
    EXPECT_TRUE(engine.book().has_order(2));
}

// --- Stress: many orders ---

TEST_F(MatchingEngineTest, ManyOrdersNoMatch) {
    for (int i = 0; i < 1000; ++i) {
        engine.process_order(Side::BUY, OrderType::LIMIT, 10000 - i, 10);
    }
    EXPECT_EQ(engine.book().total_order_count(), 1000);
    EXPECT_EQ(engine.book().best_bid().value(), 10000);
}

TEST_F(MatchingEngineTest, LargeMatchSweep) {
    // Place 100 sell orders at different prices
    for (int i = 0; i < 100; ++i) {
        engine.process_order(Side::SELL, OrderType::LIMIT, 10000 + i, 10);
    }

    // One big buy that sweeps them all
    auto trades = engine.process_order(Side::BUY, OrderType::LIMIT, 10099, 1000);

    EXPECT_EQ(trades.size(), 100);
    EXPECT_EQ(engine.book().total_order_count(), 0);
}
