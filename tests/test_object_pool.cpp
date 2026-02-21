#include <gtest/gtest.h>
#include "object_pool.h"
#include "order.h"

using namespace ob;

TEST(ObjectPool, AllocateAndDeallocate) {
    ObjectPool<Order, 16> pool;

    Order* o1 = pool.allocate();
    ASSERT_NE(o1, nullptr);
    EXPECT_EQ(pool.allocated_count(), 1);

    Order* o2 = pool.allocate();
    ASSERT_NE(o2, nullptr);
    EXPECT_NE(o1, o2);
    EXPECT_EQ(pool.allocated_count(), 2);

    pool.deallocate(o1);
    EXPECT_EQ(pool.allocated_count(), 1);

    pool.deallocate(o2);
    EXPECT_EQ(pool.allocated_count(), 0);
}

TEST(ObjectPool, ReusesDeallocatedMemory) {
    ObjectPool<Order, 16> pool;

    Order* o1 = pool.allocate();
    pool.deallocate(o1);

    // Should get back the same memory
    Order* o2 = pool.allocate();
    EXPECT_EQ(o1, o2);
}

TEST(ObjectPool, GrowsWhenExhausted) {
    ObjectPool<Order, 4> pool;  // Small block size to force growth
    EXPECT_EQ(pool.capacity(), 4);

    std::vector<Order*> ptrs;
    for (int i = 0; i < 4; ++i) {
        ptrs.push_back(pool.allocate());
    }
    EXPECT_EQ(pool.allocated_count(), 4);

    // Next allocation should trigger new block
    Order* extra = pool.allocate();
    ASSERT_NE(extra, nullptr);
    EXPECT_EQ(pool.capacity(), 8);
    EXPECT_EQ(pool.allocated_count(), 5);

    pool.deallocate(extra);
    for (auto* p : ptrs) pool.deallocate(p);
}

TEST(ObjectPool, HighVolume) {
    ObjectPool<Order> pool;  // Default block size 4096

    std::vector<Order*> ptrs;
    for (int i = 0; i < 10000; ++i) {
        ptrs.push_back(pool.allocate());
    }
    EXPECT_EQ(pool.allocated_count(), 10000);

    for (auto* p : ptrs) pool.deallocate(p);
    EXPECT_EQ(pool.allocated_count(), 0);
}
