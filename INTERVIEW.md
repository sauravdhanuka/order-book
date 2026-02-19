# HFT Interview Prep: Limit Order Book & Matching Engine

This document simulates a technical interview at a high-frequency trading firm. The interviewer is a senior systems developer. The candidate is an undergraduate who built this project. Questions start broad ("what does this do") and drill progressively deeper into implementation details, performance reasoning, failure modes, and what you'd do differently in production.

Read this end-to-end before any interview. If you can answer every question here without looking at the code, you're ready.

---

## Table of Contents

1. [The Big Picture](#part-1-the-big-picture)
2. [Data Representation](#part-2-data-representation)
3. [Memory Management](#part-3-memory-management)
4. [The Order Book](#part-4-the-order-book)
5. [The Matching Algorithm](#part-5-the-matching-algorithm)
6. [Networking & Protocol](#part-6-networking--protocol)
7. [Performance & Benchmarking](#part-7-performance--benchmarking)
8. [C++ Language & Systems Knowledge](#part-8-c-language--systems-knowledge)
9. [Failure Modes & Edge Cases](#part-9-failure-modes--edge-cases)
10. [Production Gaps & What You'd Do Differently](#part-10-production-gaps--what-youd-do-differently)
11. [Market Microstructure](#part-11-market-microstructure)
12. [The "Stump the Intern" Round](#part-12-the-stump-the-intern-round)

---

## Part 1: The Big Picture

### Q: In one sentence, what does your project do?

**A:** It's a single-threaded, in-memory limit order book that accepts buy and sell orders, matches them using price-time priority, and outputs trades — the same core logic that runs at the heart of every stock exchange.

### Q: Walk me through what happens when I type `LIMIT,BUY,100.00,50` into your program.

**A:** Here's the exact sequence:

1. **CSV Parser** reads the line, splits on commas, parses the fields.
2. It converts `100.00` to the fixed-point integer `10000` (multiplied by PRICE_SCALE of 100).
3. It calls `MatchingEngine::process_order(Side::BUY, OrderType::LIMIT, 10000, 50)`.
4. The engine **allocates** a 64-byte `Order` struct from the object pool (~5ns, no syscall).
5. It fills in the order's fields: assigns an auto-incrementing ID, timestamp, the price/qty/side/type.
6. It calls `match_buy()`, which looks at the ask side of the order book.
7. Starting from the **lowest ask price**, it checks: is this ask price <= 100.00? If yes, match.
8. Within each price level, it matches against the **earliest order first** (FIFO / time priority).
9. For each match, it creates a `Trade` struct recording buyer ID, seller ID, price, quantity.
10. If the incoming order still has unfilled quantity after sweeping all matchable asks, the remainder is **inserted into the bid side** of the order book as a resting order.
11. If it was fully filled, the Order memory is returned to the pool.
12. The `vector<Trade>` is returned to the CSV parser, which prints each trade as `TRADE buyer_id seller_id price qty`.

### Q: Why is this relevant to HFT? Doesn't the exchange already have a matching engine?

**A:** Yes, exchanges have their own matching engines. This project is relevant for three reasons:

1. **Understanding market microstructure.** To trade profitably at high frequency, you need to understand exactly how the exchange processes orders — price-time priority, partial fills, queue position. Building one forces you to internalize these mechanics.

2. **Internal matching.** Some HFT firms run internal crossing engines or dark pools to match client orders internally before routing to exchange. The same matching logic applies.

3. **Simulation.** Backtesting HFT strategies requires simulating the exchange's matching engine to model fills realistically. A fast matching engine lets you run millions of simulated trading days.

### Q: What order types does your engine support?

**A:** Two types:

- **Limit orders**: Have a price and quantity. A limit buy at 100 means "I'm willing to buy up to 100 shares at this price or better." If no matching sell exists, it rests in the book. If a matching sell exists at 100 or lower, it trades immediately.

- **Market orders**: Have only a quantity, no price. "Buy 50 shares at whatever price is available." They sweep through the book from the best price onward. If the book doesn't have enough liquidity to fill them completely, the unfilled remainder is discarded — it does not rest in the book, because a market order with no price has no meaningful level to sit at.

The engine also supports **cancel orders** — remove a previously submitted resting order by its ID.

### Q: What does "price-time priority" mean, concretely?

**A:** It's the rule that determines which resting order gets filled first when a new aggressive order arrives.

**Price priority:** If there are sell orders resting at $100 and $101, an incoming buy will match the $100 seller first — the buyer gets the better price.

**Time priority:** If there are two sell orders both resting at $100, the one that arrived first gets matched first. This is the FIFO rule within a price level.

In my code, price priority is enforced by the `std::map` ordering (asks sorted lowest-first, bids sorted highest-first). Time priority is enforced by the `std::deque` within each `PriceLevel` — orders are pushed to the back and matched from the front.

---

## Part 2: Data Representation

### Q: Why did you use fixed-point integers for prices instead of `double`?

**A:** Floating-point numbers cannot exactly represent most decimal fractions. In IEEE 754:

```
0.1 + 0.2 == 0.30000000000000004   // not 0.3
```

In a matching engine, this is a correctness bug, not just an aesthetic issue. Two orders at "the same price" might not compare as equal. An order at 100.10 might not match a resting order at 100.10 if their floating-point representations differ in the last bit.

I store prices as `int64_t` scaled by 100. So 150.25 becomes the integer 15025. All comparisons are exact integer comparisons. There is zero possibility of rounding error.

Every production exchange does this. CME uses fixed-point in their SBE/FAST market data feeds. NASDAQ's ITCH feed uses integer prices with a known scale factor.

### Q: What's the `+ 0.5` in `price_from_double`?

```cpp
inline Price price_from_double(double p) {
    return static_cast<Price>(p * PRICE_SCALE + 0.5);
}
```

**A:** It's rounding to the nearest integer instead of truncating. When you cast a `double` to `int64_t` in C++, it truncates toward zero. So `99.999999 * 100 = 9999.9999` would become `9999` instead of `10000`. Adding 0.5 before truncation converts it to a round-to-nearest operation. This handles the case where the user types `100.00` but floating-point multiplication gives `99.9999999...`.

### Q: Why is your Order struct exactly 64 bytes?

```cpp
struct Order {
    OrderId   id;           // 8
    Timestamp timestamp;    // 8
    Price     price;        // 8
    Quantity  quantity;     // 4
    Quantity  filled_qty;  // 4
    Side      side;        // 1
    OrderType type;        // 1
    uint8_t   padding[30]; // 30
};  // Total: 64
static_assert(sizeof(Order) == 64, "...");
```

**A:** 64 bytes is the cache line size on x86 and ARM. When the CPU reads any byte of an Order, it loads the entire 64-byte cache line into L1 cache. If my Order is exactly 64 bytes and aligned to a 64-byte boundary, every field access is guaranteed to be a cache hit after the first access — the entire order is in one cache line.

If the struct were 65 bytes, it would straddle two cache lines. Accessing the last field would trigger a second cache line fetch — doubling the memory latency for that access. In a tight matching loop processing millions of orders, this adds up.

The `static_assert` enforces this at compile time. If I add a field later and forget to adjust the padding, the build fails immediately instead of silently degrading performance.

### Q: Why are the fields in that specific order?

**A:** To minimize compiler-inserted padding. C++ structs have alignment requirements: an 8-byte field must be at an 8-byte-aligned offset, a 4-byte field at a 4-byte-aligned offset, etc.

By placing 8-byte fields first (id, timestamp, price), then 4-byte fields (quantity, filled_qty), then 1-byte fields (side, type), there is zero implicit padding between fields. The compiler packs them tightly.

If I had put `Side side` (1 byte) before `Price price` (8 bytes), the compiler would insert 7 bytes of padding after `side` to align `price` — wasting space and potentially pushing the struct past 64 bytes.

### Q: Why `uint32_t` for quantity instead of `uint64_t`?

**A:** A `uint32_t` can hold up to ~4.3 billion. No single order on any exchange will ever have a quantity that large — most have lot size limits in the millions at most. Using 32 bits instead of 64 saves 4 bytes per order, which helps keep the struct at 64 bytes.

In HFT, every byte matters. Not because of storage cost, but because of cache efficiency. Smaller structs mean more orders fit in L1/L2 cache, which means fewer cache misses during matching.

### Q: What's the `remaining()` function doing?

```cpp
Quantity remaining() const { return quantity - filled_qty; }
```

**A:** It returns the unfilled portion of the order. An order starts with `quantity = 100` and `filled_qty = 0`, so `remaining() = 100`. After a partial fill of 60 units, `filled_qty = 60`, so `remaining() = 40`.

This is used in the matching loop to determine how many units can trade:

```cpp
Quantity fill_qty = std::min(incoming->remaining(), resting->remaining());
```

The trade quantity is the smaller of what the buyer still wants and what the seller still has. After the fill, both sides' `filled_qty` are incremented by `fill_qty`.

### Q: Why a monotonic counter for timestamps instead of wall-clock time?

**A:** Two reasons:

1. **Performance.** Calling `std::chrono::high_resolution_clock::now()` invokes `clock_gettime()`, a syscall that costs 20-50ns. A simple `next_timestamp_++` costs ~1ns. When processing 8 million orders per second, saving 30ns per order is a 25% throughput improvement.

2. **Sufficiency.** Timestamps in the matching engine serve one purpose: establishing time priority (which order came first). A monotonic counter is a perfect total order — every order gets a unique, always-increasing value. Wall-clock time is only needed for regulatory reporting, which is a separate concern.

---

## Part 3: Memory Management

### Q: Explain your object pool. What problem does it solve?

**A:** The standard `new` operator calls `malloc`, which has to:
1. Search free lists or bins for a suitable block
2. Possibly call `mmap` or `sbrk` to get memory from the OS (a syscall)
3. Update allocator metadata
4. Handle thread-safety (locks or lock-free algorithms)

This costs 50-200ns per allocation. In a matching engine processing 8M orders/sec, that's 400ns-1.6us of allocation overhead per order — potentially more than the matching logic itself.

My object pool pre-allocates a big block of Order-sized slots (4096 * 64 = 256KB per block). It maintains a singly-linked free list through the unused slots. Allocation is just "pop the head of the free list" — one pointer read, one pointer write. Deallocation is "push onto the head" — one pointer write. Both are O(1), ~5ns, no syscalls.

### Q: Walk me through the free list mechanism. How do you store the linked list inside the unused slots?

**A:** This is the key trick. When a slot is **unused** (in the free list), I treat its first 8 bytes as a `Node*` pointer to the next free slot:

```cpp
struct Node { Node* next; };
```

Since each slot is 64 bytes and a pointer is 8 bytes, there's plenty of room. The slot is either being used as an `Order` (all 64 bytes are meaningful) or it's in the free list (only the first 8 bytes are the `next` pointer, the rest is garbage).

When `allocate()` is called, I pop the head:
```cpp
Node* node = free_list_;       // Head of free list
free_list_ = node->next;      // Advance head to next free slot
return reinterpret_cast<T*>(node);  // Return as Order*
```

When `deallocate()` is called, I push to the head:
```cpp
Node* node = reinterpret_cast<Node*>(ptr);  // Reinterpret Order* as Node*
node->next = free_list_;                     // Point to current head
free_list_ = node;                           // New head
```

The `reinterpret_cast` is safe because we're just reinterpreting the same memory. The `static_assert(sizeof(T) >= sizeof(Node))` guarantees the slot is large enough to hold the pointer.

### Q: What does `[[unlikely]]` do in the allocate path?

```cpp
if (free_list_ == nullptr) [[unlikely]] {
    allocate_block();
}
```

**A:** It's a C++20 branch prediction hint. It tells the compiler that this condition is almost never true — the free list is almost never empty. The compiler uses this to:

1. **Optimize instruction layout:** The fast path (free list not empty) gets sequential instructions without jumps. The slow path (allocate new block) goes into a separate cold section.
2. **Hint the CPU branch predictor:** The generated code biases toward predicting the branch as not-taken.

In practice, after the initial block is allocated, the free list has 4096 slots. It only empties if we have 4096+ simultaneous live orders, which triggers one growth event. After growth, the fast path resumes.

### Q: What happens if you allocate 10,000 orders and then deallocate all of them? Does the memory go back to the OS?

**A:** No. The pool never returns memory to the OS until the pool itself is destroyed. After deallocating 10,000 orders, all 10,000 slots are back on the free list, ready for reuse. The pool holds onto 3 blocks (4096 + 4096 + 1808 slots, though it rounds up to full blocks, so 3 * 4096 = 12,288 capacity).

This is intentional. In a matching engine, order volume fluctuates. You might have 10,000 live orders at peak, drop to 1,000, then spike to 10,000 again. If the pool returned memory to the OS and had to re-allocate, you'd pay `mmap` syscall cost during the spike — right when latency matters most.

The downside: if order volume was briefly 100,000 and then dropped to 100 permanently, you'd have ~6MB of unused-but-held memory. For a trading system, this is negligible. Memory is cheap; latency spikes are expensive.

### Q: Why is the pool non-copyable and non-movable?

**A:** Copying: if you copy a pool, both copies would think they own the same underlying memory blocks. When one is destroyed, it frees the blocks. The other now has dangling pointers. Every Order* in the OrderBook now points to freed memory. Instant use-after-free.

Moving: the OrderBook holds raw `Order*` pointers that point into the pool's blocks. If the pool is moved to a new memory address, the `blocks_` vector is moved, but the actual block memory stays in place — so in theory it's safe. But the `free_list_` and internal Node pointers all point into the same blocks. The risk is that someone moves the pool while the book still has references, which creates confusing ownership semantics. Deleting move prevents this class of bugs entirely.

In production code where you need movability, you'd use `std::unique_ptr` and make it explicit that ownership transfers.

### Q: What's the `std::align_val_t{alignof(T)}` doing in the allocation?

```cpp
::operator new(sizeof(T) * BlockSize, std::align_val_t{alignof(T)})
```

**A:** It requests memory aligned to the alignment requirement of `T`. For our 64-byte `Order`, `alignof(Order)` is likely 8 (the alignment of `uint64_t`, the largest field). But more importantly, this ensures that the block starts at a properly aligned address so that each slot within the block, being `sizeof(T)` apart, is also properly aligned.

If the block started at a misaligned address, some Order slots would straddle cache line boundaries, defeating the purpose of the 64-byte sizing. Aligned allocation prevents this.

On architectures with strict alignment requirements (some ARM modes), misaligned access causes a hardware fault. Even on x86 where misaligned access is allowed, it can be 2x slower due to cache line splits.

---

## Part 4: The Order Book

### Q: Describe the data structures your order book uses and why.

**A:** Three data structures, each serving a different access pattern:

1. **`std::map<Price, PriceLevel, std::greater<>> bids_`** — A red-black tree mapping prices to PriceLevels, sorted highest price first. Used for: iterating bids from best (highest) to worst during matching, and O(1) best-bid access via `begin()`.

2. **`std::map<Price, PriceLevel> asks_`** — Same structure, but sorted lowest price first (default ordering). Used for: iterating asks from best (lowest) to worst during matching.

3. **`std::unordered_map<OrderId, Order*> order_lookup_`** — A hash map from order ID to Order pointer. Used for: O(1) lookup when processing cancel requests.

An order exists in exactly two of these structures: one PriceLevel inside the appropriate map, and one entry in the lookup map. This dual-indexing gives us both sorted iteration (for matching) and fast random access (for cancellation).

### Q: Why `std::map` (tree) instead of `std::unordered_map` (hash table) for bids and asks?

**A:** The matching engine needs to walk prices **in sorted order**. When a buy order comes in, I need to check asks starting from the lowest price and moving upward. `std::map` keeps keys sorted, so I just iterate from `begin()`. That's O(1) to get the next price level.

With `std::unordered_map`, there's no ordering. To find the lowest ask, I'd have to scan every key — O(n). Or sort them first — O(n log n). Either way, I'd pay this cost on every incoming aggressive order. For a matching engine, that's a non-starter.

### Q: Why `std::map` instead of a flat sorted array?

**A:** Insertion and deletion characteristics.

A sorted vector (`std::vector` + `std::lower_bound`) has O(n) insertion and deletion because elements must be shifted. With 200 active price levels (common for liquid stocks), every new price level requires shifting up to 200 entries.

`std::map` has O(log n) insertion and deletion — about 8 comparisons for 200 levels. The trade-off is that `std::map` has worse cache locality than a vector (tree nodes are scattered in memory), but with only 200 levels, the entire tree fits in L2 cache regardless.

### Q: An interviewer pushes: "Production systems use flat arrays indexed by price. Why didn't you?"

**A:** That's the optimal approach when the price range is bounded and known in advance. You create an array of size `(max_price - min_price) / tick_size`, and index directly: `levels[price - min_price]`. This gives O(1) everything.

I didn't use it because:
1. It requires knowing the price range up front. My engine handles arbitrary prices.
2. For wide-range instruments, the array wastes memory. A stock at $3000 with tick size $0.01 needs a 300,000-element array, most of which are empty.
3. `std::map` is simpler to implement correctly and still meets my performance targets (>8M orders/sec).

If I were building for a specific instrument with a known tick range, I would absolutely use a flat array. It's a valid trade-off I chose not to make for generality.

### Q: What does `std::greater<>` do on the bids map?

```cpp
std::map<Price, PriceLevel, std::greater<>> bids_;
```

**A:** `std::map` sorts keys in ascending order by default. For bids, the "best" bid is the **highest** price. I want `bids_.begin()` to return the highest-priced level so I can start matching there.

`std::greater<>` reverses the comparison, making the map sort in **descending** order. Now `begin()` gives the highest price, which is the best bid.

The `<>` (empty angle brackets) makes it a transparent comparator, meaning it can compare different types without requiring an explicit conversion. It's a C++14 feature.

### Q: Explain the `remove_from_lookup` method. Why does it exist separately from `cancel_order`?

**A:** This was born from a real bug I discovered during testing.

`cancel_order()` does three things:
1. Finds the order in `order_lookup_` (hash map)
2. Removes the order from its PriceLevel's deque
3. Removes empty price levels from the map

`remove_from_lookup()` does only step 1.

During matching, the engine iterates over the asks map and within each PriceLevel's deque. When a resting order is fully filled, the engine has already popped it from the deque via `pop_front()`. It then needs to remove it from the hash map.

If I called `cancel_order()` here, it would try to also remove the order from the PriceLevel — but it's already been popped. Worse, `cancel_order()` checks if the level is empty and erases it from the map. But the matching loop is still holding an iterator to that map entry. Erasing it from inside `cancel_order()` invalidates the iterator, and the next iteration of the matching loop dereferences a dangling iterator — **segfault**.

The fix: `remove_from_lookup()` only touches the hash map, leaving the map and deque management to the matching loop itself. The matching loop erases empty levels at the end of each price level iteration, when it's safe to do so.

This was caught by unit tests — 9 out of 32 tests segfaulted before the fix. The lesson: never mutate a data structure from two different code paths that don't know about each other.

### Q: What's the time complexity of `cancel_order`?

**A:** Breaking it down:
- `order_lookup_.find(order_id)` — O(1) average (hash map lookup)
- `order_lookup_.erase(it)` — O(1) amortized (hash map erase)
- `bids_.find(order->price)` or `asks_.find(...)` — O(log n) where n is the number of active price levels
- `level.remove(order)` — O(k) where k is the number of orders at that price level (linear scan of the deque)
- `bids_.erase(level_it)` if empty — O(1) amortized (tree erase with iterator)

**Total: O(log n + k)** where n = price levels, k = orders at the order's price.

In practice: n is usually 100-500 (so log n ~ 7-9), and k is usually 1-50. So cancel takes roughly 10-60 operations, which at ~1-5ns each is 50-300ns.

### Q: How does your book handle the case where I add a bid at 100 and an ask at 100 simultaneously?

**A:** They can't be simultaneous — the engine is single-threaded and processes orders sequentially. If the bid arrives first, it rests in the book. When the ask arrives, it's an aggressive order (the resting bid at 100 >= the ask at 100), so the matching engine matches them. The bid came first, so the ask is the aggressor.

The trade executes at 100 (the resting order's price). Both orders are fully filled and returned to the pool.

If the ask arrived first and the bid second, the same trade would occur with the bid as the aggressor. The result is identical — same price, same quantity, same trade.

---

## Part 5: The Matching Algorithm

### Q: Walk through the `match_buy` function line by line.

**A:**

```cpp
void MatchingEngine::match_buy(Order* incoming, std::vector<Trade>& trades) {
    auto& asks = book_.asks();
```
Get a reference to the asks map. This is the side we're matching against. The reference avoids copying.

```cpp
    while (!incoming->is_filled() && !asks.empty()) {
```
Outer loop: keep going while the incoming buy has unfilled quantity AND there are asks in the book.

```cpp
        auto it = asks.begin();
        Price ask_price = it->first;
```
Get the best (lowest) ask level. `asks.begin()` is O(1) because `std::map` keeps a pointer to its leftmost node.

```cpp
        if (incoming->type == OrderType::LIMIT && ask_price > incoming->price) {
            break;
        }
```
**Price check.** For limit orders: if the cheapest ask is more expensive than our buy price, there's no match possible — stop. For market orders: this check is skipped (market orders have no price constraint), so we keep matching regardless of price.

```cpp
        PriceLevel& level = it->second;
```
Get the PriceLevel at this price. A reference because we'll modify it.

```cpp
        while (!incoming->is_filled() && !level.is_empty()) {
```
Inner loop: match against orders at this price level, FIFO.

```cpp
            Order* resting = level.front();
            Quantity fill_qty = std::min(incoming->remaining(), resting->remaining());
```
Take the front order (earliest at this price). Fill quantity is the minimum of what both sides can trade.

```cpp
            trades.push_back(execute_trade(incoming, resting, fill_qty, ask_price));
```
Execute the trade: increment both sides' `filled_qty`, create a Trade struct. The trade price is `ask_price` — the resting order's price. This is price improvement for the buyer if they bid higher.

```cpp
            if (resting->is_filled()) {
                level.pop_front();
                book_.remove_from_lookup(resting->id);
                pool_.deallocate(resting);
```
If the resting order is fully filled: remove from the deque, remove from the hash map, return memory to pool.

```cpp
            } else {
                level.reduce_quantity(fill_qty);
            }
```
If partially filled: the order stays in the deque but we update the PriceLevel's cached total quantity.

```cpp
        }
        if (level.is_empty()) {
            asks.erase(it);
        }
```
After processing all orders at this price, if the level is empty, remove it from the map. This is safe because we're done iterating within this level.

```cpp
    }
}
```

### Q: What price does a trade execute at?

**A:** Always the **resting order's price**, not the incoming order's price.

If a resting sell sits at 100.00 and a buy comes in at 102.00, the trade executes at 100.00. The buyer gets "price improvement" — they were willing to pay 102 but only paid 100.

This is how every exchange works. The resting order established a price. The aggressive order accepted that price (or better). The resting price is the fair execution price.

In my code, this is implemented by passing `ask_price` (the resting level's price) to `execute_trade`, not `incoming->price`:

```cpp
trades.push_back(execute_trade(incoming, resting, fill_qty, ask_price));
//                                                          ^^^^^^^^^
//                                                   resting order's price
```

### Q: What happens with a market order when the book is empty?

**A:** Nothing. The matching loop immediately exits (the `!asks.empty()` condition fails on the first iteration). The order is not filled at all. Since it's a market order, the engine discards it:

```cpp
if (!order->is_filled()) {
    if (type == OrderType::LIMIT) {
        book_.add_order(order);     // Limit: rest in book
    } else {
        pool_.deallocate(order);    // Market: discard
    }
}
```

A market order cannot rest in the book because it has no price. There's no meaningful price level to place it at. Real exchanges handle this the same way — a market order against an empty book is rejected.

### Q: What if a market order is only partially filled?

**A:** Same behavior — the unfilled remainder is discarded. If the book has 30 shares of liquidity and a market buy for 100 comes in, it fills 30 shares and the other 70 are rejected.

This matches exchange behavior. Some exchanges offer "Fill-or-Kill" (FOK) orders that cancel the entire order if it can't be fully filled. My engine doesn't support FOK — partial market fills are silently accepted.

### Q: Is there any self-trade prevention?

**A:** No. If the same participant sends a buy and a sell that would cross, they match against each other. In production, this is dangerous — it creates a "wash trade" which is illegal in most jurisdictions.

Real exchanges have self-trade prevention (STP) modes:
- **Cancel newest:** Cancel the incoming order if it would self-trade
- **Cancel oldest:** Cancel the resting order
- **Cancel both:** Cancel both sides

To add STP, I'd add a `participant_id` field to the Order struct and check it during matching:

```cpp
if (incoming->participant_id == resting->participant_id) {
    // Handle per the STP mode
    continue;  // or cancel
}
```

### Q: How does your engine handle the `match_sell` direction? Is it just a mirror of `match_buy`?

**A:** Yes, it's structurally identical with the direction reversed:

| | `match_buy` | `match_sell` |
|---|---|---|
| Walks which side? | asks (lowest first) | bids (highest first) |
| Price check | `ask_price > buy_price` → stop | `bid_price < sell_price` → stop |
| Trade roles | incoming = buyer, resting = seller | resting = buyer, incoming = seller |

The key difference in `execute_trade` call is the argument order:
```cpp
// match_buy:
execute_trade(incoming, resting, fill_qty, ask_price);  // incoming is buyer
// match_sell:
execute_trade(resting, incoming, fill_qty, bid_price);  // resting is buyer
```

This ensures `Trade.buyer_order_id` and `Trade.seller_order_id` are always correct regardless of which side was the aggressor.

---

## Part 6: Networking & Protocol

### Q: Why a binary protocol instead of JSON or Protobuf?

**A:** Latency. The numbers:

| Format | Parse time | Serialize time | Message size |
|--------|-----------|---------------|-------------|
| My binary protocol | 0ns (memcpy) | 0ns (memcpy) | 32 bytes |
| Protobuf | 100-500ns | 100-500ns | ~40-60 bytes |
| JSON | 1-10us | 1-10us | ~100-200 bytes |

My protocol uses fixed-size 32-byte structs that are trivially copyable. "Serialization" is literally `memcpy(&msg, buf, 32)`. There is no parsing step — the struct is the wire format.

Real exchange protocols (CME iLink, NASDAQ OUCH, LSE Native) all use fixed-size or SBE (Simple Binary Encoding) binary formats for exactly this reason. In HFT, the time spent parsing a JSON message would be longer than the time spent actually matching the order.

### Q: What's the downside of this binary approach?

**A:** Several:

1. **No versioning.** If I change the struct layout, old clients break. Protobuf handles this with field numbers and optional fields.

2. **Byte-order dependency.** My protocol assumes both sides are little-endian. If the client is big-endian (rare today but possible on some POWER/SPARC systems), the fields are garbled. A production protocol would either standardize on network byte order (`htonl`/`ntohl`) or use SBE which specifies byte order in the schema.

3. **Not self-describing.** A Protobuf message carries its own schema information. My binary messages are meaningless without knowing the struct definition. This makes debugging harder.

4. **Fixed size wastes space for small messages.** A cancel order only needs a message type + order ID (~9 bytes) but I send 32 bytes every time. With millions of cancels per second, that's wasted bandwidth. In practice, this is negligible on low-latency networks.

### Q: Explain how your TCP server handles partial reads.

**A:** TCP is a stream protocol — a `read()` call might return fewer bytes than requested. If I send 32 bytes, the server might receive it as two reads of 16 bytes each. This is especially common under high load.

Each client has a `ClientState` with a read buffer and a byte counter:

```cpp
struct ClientState {
    int fd;
    char read_buf[sizeof(OrderMessage)];  // 32 bytes
    size_t bytes_read = 0;
};
```

When data arrives:
```cpp
ssize_t n = read(client_fd, client.read_buf + client.bytes_read,
                 MSG_SIZE - client.bytes_read);
client.bytes_read += n;

if (client.bytes_read == MSG_SIZE) {
    // Full message received — process it
    deserialize(client.read_buf, msg);
    process_message(client_fd, msg);
    client.bytes_read = 0;  // Reset for next message
}
```

We always read into `read_buf` at the current offset, requesting only the remaining bytes. Once we have all 32 bytes, we process the message and reset the counter. This correctly handles any fragmentation pattern.

### Q: What is `kqueue` and why did you choose it?

**A:** `kqueue` is macOS/BSD's I/O event notification mechanism, equivalent to Linux's `epoll`. It lets a single thread efficiently monitor many file descriptors (sockets) for readability/writability without polling.

The alternative approaches and why they're worse:

| Approach | Scalability | CPU usage |
|----------|------------|-----------|
| Thread-per-client | O(n) threads, expensive context switches | High |
| `select()` | O(n) scan of FD set per call, 1024 FD limit | High |
| `poll()` | O(n) scan, no FD limit | High |
| `kqueue`/`epoll` | O(1) per event, no FD limit | Low |

With `kqueue`, the kernel maintains the list of monitored FDs. When any of them has data ready, `kevent()` returns only the ready FDs — no scanning. This is O(1) per ready event regardless of total connection count.

I chose `kqueue` because I'm building on macOS. On Linux, I'd use `epoll`, which has a nearly identical API pattern. A production system might use a library like `libuv` or `io_uring` to abstract the platform difference.

### Q: Why single-threaded server?

**A:** The matching engine must process orders in a deterministic sequence. Two orders arriving simultaneously must be resolved into a single, well-defined order. A single-threaded server makes this trivially correct — orders are processed in the order they're read from the network.

A multi-threaded server would need a sequencer: something that assigns a global sequence number to incoming orders before they're fed to the matching engine. This adds complexity and latency (the sequencing step itself becomes a bottleneck). It's only worth it when network I/O handling is the bottleneck — which it isn't in this project.

Real exchanges solve this with a combination of: hardware timestamps (FPGA-based NICs), lockfree queues (SPSC rings), and per-instrument sharding (each instrument runs on its own core). But the matching engine itself is always effectively single-threaded per instrument.

### Q: What does `TCP_NODELAY` do and why is it critical?

**A:** It disables Nagle's algorithm. Nagle's algorithm buffers small TCP writes and sends them as one larger segment, reducing network overhead. It was designed for interactive terminals in the 1980s.

In a matching engine, this is catastrophic. Without `TCP_NODELAY`:

1. Server matches an order and writes a 32-byte ACK response.
2. Nagle's algorithm thinks: "That's a tiny packet. I'll wait for more data."
3. It waits up to 200ms (the TCP delayed ACK timer) for more data to batch.
4. The client's ACK response arrives 200ms late instead of ~100us.

With `TCP_NODELAY`, the 32-byte response is sent immediately in its own TCP segment. Yes, this is less bandwidth-efficient (TCP/IP headers are ~40 bytes for a 32-byte payload), but latency is what matters in trading.

Every HFT network stack disables Nagle on all sockets.

---

## Part 7: Performance & Benchmarking

### Q: Walk me through how you measured latency.

**A:**

```cpp
for (const auto& order : orders) {
    auto start = Clock::now();  // Before processing

    if (order.is_cancel)
        engine.cancel_order(order.cancel_id);
    else
        engine.process_order(order.side, order.type, order.price, order.quantity);

    auto end = Clock::now();    // After processing
    double ns = std::chrono::duration<double, std::nano>(end - start).count();
    latencies.push_back(ns);
}

// After all orders, sort and compute percentiles
std::sort(latencies.begin(), latencies.end());
p50 = latencies[n * 50 / 100];
p99 = latencies[n * 99 / 100];
```

Each order is timed individually. The latencies are collected into a vector, sorted, and percentiles extracted by index. This gives the full latency distribution, not just the average.

### Q: Why do you report p50, p95, p99, p99.9 instead of just the average?

**A:** Because averages lie. If 99% of orders take 50ns and 1% take 10,000ns, the average is ~150ns — which doesn't describe anyone's actual experience.

In trading, what matters is the **tail latency** — the worst-case performance. A strategy that's fast 99% of the time but has 10us spikes at p99.9 will miss fills during volatile market moments, which is exactly when the best opportunities exist.

The percentiles tell you:
- **p50 (median):** What does "typical" look like? (~83ns for my engine)
- **p95:** What does "most orders" look like? (~250ns)
- **p99:** What's the bad case? (~375ns) — This is where `std::map` tree rebalancing and multi-level sweeps show up.
- **p99.9:** What's the really bad case? (~833ns) — This is where pool growth, large sweeps, or `unordered_map` rehashing shows up.

HFT firms focus on p99 and p99.9. If your p50 is 100ns but your p99.9 is 100us, you have a jitter problem.

### Q: Your benchmark shows ~83ns p50 latency. What's actually happening in those 83 nanoseconds?

**A:** For a typical order that doesn't match (rests in the book):

1. `pool_.allocate()` — Pop from free list: ~3-5ns
2. Fill in 7 fields of the Order struct (all in one cache line): ~2-3ns
3. `match_buy()` or `match_sell()` — Check asks/bids begin(), find no match, return: ~10-15ns
4. `book_.add_order()` — Insert into `std::map` (O(log n) tree traversal, ~8 comparisons): ~30-40ns
5. Insert into `unordered_map` (hash + bucket lookup): ~10-20ns
6. `PriceLevel::add()` — `deque.push_back()`: ~5-10ns
7. Return empty `vector<Trade>`: ~5ns

Total: ~65-93ns. That lines up with the observed ~83ns p50.

### Q: Why is the high-cancel benchmark faster than the others?

**A:** The 30% cancel workload has a p50 of ~42ns. Cancels are cheap:

1. `order_lookup_.find(id)` — Hash lookup: ~10-15ns
2. `order_lookup_.erase()` — Hash erase: ~5-10ns
3. `level.remove(order)` — Small deque scan: ~5-15ns (levels are small)
4. `pool_.deallocate()` — Push to free list: ~3-5ns

Total: ~23-45ns. And 30% of orders are this fast, pulling down the median.

Also, cancels remove orders from the book, keeping it smaller. Smaller book means faster `std::map` operations for the remaining 70% of orders.

### Q: You got 8.5M orders/sec. Is that good?

**A:** For a single-threaded C++ implementation on consumer hardware, yes. For context:

- **NASDAQ's matching engine**: handles ~1-3M messages/sec per instrument, but with hardware-optimized networking (FPGA NICs, kernel bypass).
- **A production HFT internal matching engine**: typically targets single-digit microsecond latency with 1-10M orders/sec.
- **This project**: 8.5M orders/sec with p99 at 375ns on a laptop. That's competitive.

Where it falls short of production: the `vector<Trade>` allocation per order, the `std::map` overhead (production would use flat arrays), and the lack of kernel bypass networking. But the core matching logic is in the right ballpark.

### Q: What would happen to your benchmarks if you replaced the object pool with `new`/`delete`?

**A:** I'd expect throughput to drop to roughly 3-5M orders/sec and p50 to increase to 150-300ns. Every `process_order` call would hit `new` (50-200ns) and every filled/cancelled order would hit `delete` (50-200ns). That's 100-400ns of allocator overhead per order.

The object pool reduces this to ~10ns total (5ns allocate + 5ns deallocate), saving 90-390ns per order. On 8M orders/sec, that's 0.7-3.1 seconds saved per million orders.

I didn't implement this comparison in the benchmark (it would require refactoring the engine to optionally bypass the pool), but the expected impact is well-documented in systems programming literature.

---

## Part 8: C++ Language & Systems Knowledge

### Q: What does `#pragma once` do and why use it instead of include guards?

**A:** It tells the compiler "only include this file once per translation unit." It's equivalent to the traditional:

```cpp
#ifndef TYPES_H
#define TYPES_H
// ...
#endif
```

I use `#pragma once` because it's shorter, less error-prone (no risk of mismatched guard names), and supported by every modern compiler (GCC, Clang, MSVC). It's not part of the C++ standard, but it's a de facto standard.

### Q: Explain `reinterpret_cast` — when is it safe, and why do you use it in the pool?

**A:** `reinterpret_cast` tells the compiler "treat this memory as a different type." In the pool:

```cpp
T* allocate() {
    Node* node = free_list_;
    return reinterpret_cast<T*>(node);
}
```

This is safe here because:
1. The memory is properly aligned for `T` (guaranteed by aligned allocation)
2. We're about to write to this memory as a `T` (the caller fills in Order fields)
3. The memory is large enough (sizeof(T) >= sizeof(Node), enforced by static_assert)
4. We never read the `Node` data after casting to `T`

It would be **unsafe** if we tried to read the `Node::next` pointer after the memory has been filled with Order data — that would be type punning / undefined behavior.

### Q: What's the difference between `::operator new` and the `new` keyword?

**A:** The `new` keyword does two things: (1) allocates memory, (2) calls the constructor. `::operator new` only allocates memory — it's the raw allocation function that `new` calls internally.

In the pool:
```cpp
char* block = static_cast<char*>(
    ::operator new(sizeof(T) * BlockSize, std::align_val_t{alignof(T)}));
```

I only want raw memory. I don't want to construct 4096 Order objects — that would waste time initializing fields that the caller will overwrite immediately. The caller constructs the object by writing to the fields after `allocate()` returns.

Similarly, the destructor uses `::operator delete` to free memory without calling destructors:
```cpp
::operator delete(block, std::align_val_t{alignof(T)});
```

### Q: You use `auto&` return types on `bids()` and `asks()`. Doesn't that leak implementation details?

```cpp
auto& bids() { return bids_; }
```

**A:** Yes, it does. The matching engine gets direct access to the internal `std::map`. This is a deliberate trade-off:

**Pro:** The matching engine can iterate the map directly without going through accessor methods. This avoids virtual dispatch, method call overhead, and iterator wrapper indirection. In a hot matching loop, this matters.

**Con:** The OrderBook can't change its internal representation without breaking the MatchingEngine. If I wanted to swap `std::map` for a flat array, I'd have to change the matching engine too.

For this project, performance over encapsulation is the right trade-off. The OrderBook and MatchingEngine are tightly coupled by design — they're part of the same system, maintained by the same developer. In a larger codebase with multiple teams, I'd add a proper iterator interface.

### Q: Why `std::optional` for `best_bid()` / `best_ask()`?

```cpp
std::optional<Price> best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}
```

**A:** An empty book has no best bid. The function needs to express "there is no value." The alternatives:

- **Return a sentinel (-1 or INT_MAX):** Error-prone. Caller might forget to check and use the sentinel as a real price.
- **Return a boolean + output parameter:** Verbose, C-style.
- **Throw an exception:** Exceptions are expensive and this isn't an error — an empty book is normal.
- **`std::optional`:** Type-safe, explicit "maybe there's a value." Caller must check `.has_value()` before using it.

`std::optional` also has zero overhead when a value is present — it's just the value plus a boolean flag. The compiler often optimizes away the flag check after inlining.

---

## Part 9: Failure Modes & Edge Cases

### Q: What happens if two orders have the same timestamp?

**A:** They can't. The timestamp is a monotonically incrementing counter (`next_timestamp_++`), so every order gets a unique timestamp. Unlike wall-clock time, which can have ties at nanosecond resolution, a counter guarantees strict total ordering.

If I used wall-clock time and two orders had the same timestamp, the tiebreaker would be insertion order into the deque — which is still FIFO since the engine is single-threaded.

### Q: What if someone sends a cancel for an order that was already filled?

**A:** `cancel_order` looks up the order ID in the hash map. A filled order has already been removed from both the hash map and the PriceLevel (during matching). The hash map lookup returns `end()`, and `cancel_order` returns `nullptr`. The MatchingEngine returns `false`. The CSV parser prints `CANCEL_REJECT <id> (not found)`. No crash, no corruption.

### Q: What happens if the object pool's blocks vector needs to grow?

**A:** `blocks_` is a `std::vector<char*>`. When the vector needs to grow, it reallocates its internal buffer (which is just an array of pointers). This is safe because:

1. The pointers in the vector point to heap-allocated blocks. Moving the vector's internal buffer (copying pointers) doesn't affect the blocks themselves.
2. No one else holds pointers to the vector's internal buffer.
3. The blocks themselves are never moved — only pointers to them are stored and copied.

The growth of `blocks_` itself is rare (one new entry per 4096 orders) and fast (copying a few pointers).

### Q: Can `deallocate` be called with a pointer that wasn't from this pool?

**A:** That would be undefined behavior. The pool pushes the pointer onto the free list, which corrupts it if the pointer is from a different allocator. On the next `allocate()`, the pool returns this corrupt pointer, and the caller writes Order data to memory it doesn't own — a heap corruption bug.

There's no runtime check for this. In a debug build, you could add a check (e.g., verify the pointer falls within a known block's address range), but in release builds, the overhead isn't worth it. This is a programmer error, like passing an invalid pointer to `free()`.

### Q: What if the hash map for `order_lookup_` needs to rehash?

**A:** `std::unordered_map` rehashes when its load factor exceeds a threshold (default 1.0). Rehashing allocates a new bucket array and re-inserts all entries — O(n) cost. This shows up as a latency spike.

In the benchmark, this would appear as an occasional p99.9 outlier. With ~4000 live orders in the book, the hash map has ~4000 entries. Rehashing 4000 entries takes ~4000 * 50ns = ~200us — a massive spike.

To prevent this: call `order_lookup_.reserve(expected_max_orders)` at startup. This pre-allocates enough buckets to avoid rehashing. I didn't do this in my implementation — it's an easy optimization I'd add in production.

---

## Part 10: Production Gaps & What You'd Do Differently

### Q: What's missing to make this production-ready?

**A:** In roughly priority order:

1. **Persistence / crash recovery.** Everything is in memory. If the process crashes, the entire book state is lost. Production systems log every order to a write-ahead log (WAL) and replay on restart.

2. **Self-trade prevention.** No participant ID concept. Wash trades are a securities law violation.

3. **Order modification (replace).** Real exchanges support changing an order's price or quantity without losing queue position. Currently I only support cancel + re-submit, which loses time priority.

4. **Rate limiting / access control.** Anyone can connect and flood the TCP server. Production systems authenticate clients and enforce message rate limits.

5. **Market data output.** The engine produces trades but doesn't broadcast price updates. A real exchange sends L1 (best bid/offer), L2 (full depth), and trade messages to all subscribers.

6. **Multiple instruments.** One book, one instrument. Production: thousands of instruments, each with its own book, sharded across cores.

7. **Kernel bypass networking.** The TCP stack adds ~5us of latency. DPDK, io_uring, or FPGA NICs reduce this to ~1-2us.

8. **Pre-sized containers.** `order_lookup_.reserve()`, `trades.reserve()`, pre-allocated response buffers. Avoid any heap allocation in the hot path.

### Q: If you had one more week, what would you add first?

**A:** Order modification (amend/replace). It's the most important missing feature from a trading perspective. In real markets, 80-95% of messages are cancels and amends — not new orders. A firm constantly adjusts its quotes by a tick as the market moves. Without amend, they must cancel and re-enter, losing queue position.

Implementation: add a `modify_order(OrderId, new_price, new_qty)` method that removes the order from its current level and re-inserts at the new price. If only quantity decreases (downtick), some exchanges preserve queue position — that's a more nuanced rule to implement.

### Q: How would you make this multi-threaded?

**A:** I would NOT make the matching engine itself multi-threaded. Instead:

```
Network Thread                 Matching Thread
     │                              │
     │  recv() orders               │
     │         │                    │
     │         ▼                    │
     │  ┌──────────────┐           │
     │  │ SPSC Queue   │──────────►│  process_order()
     │  │ (lock-free)  │           │  match, book update
     │  └──────────────┘           │
     │                              │
     │  ┌──────────────┐           │
     │  │ SPSC Queue   │◄──────────│  trades, acks
     │  │ (lock-free)  │           │
     │  └──────────────┘           │
     │         │                    │
     │         ▼                    │
     │  send() responses            │
```

Two threads: one for network I/O, one for matching. Connected by two lock-free single-producer single-consumer (SPSC) ring buffers. The matching thread never touches a socket; the network thread never touches the book. No locks, no contention.

For multiple instruments: one matching thread per instrument (or per group of instruments). The network thread routes orders to the correct queue based on instrument ID.

### Q: How would you debug a latency spike in production?

**A:** Systematic approach:

1. **Check the percentile data.** Is it p99.9 (rare) or p50 (systemic)? Rare spikes = cache misses, rehashing, pool growth. Systemic = algorithmic issue.

2. **Profiling.** Use `perf record` (Linux) or Instruments (macOS) to capture CPU samples during the spike. Look for hot functions.

3. **Targeted instrumentation.** Add `rdtsc` timestamps before/after key operations: pool allocate, map lookup, level iteration, hash map insert. Identify which step is slow.

4. **Cache analysis.** Use `perf stat` to count L1/L2/L3 cache misses. If cache miss rate is high, the working set is too large or data layout has locality problems.

5. **Allocator analysis.** Check if spikes correlate with pool growth or hash map rehashing. Add counters for these events.

6. **Page faults.** Check `perf stat` for minor/major page faults. First access to a new pool block causes minor faults. Pre-fault pages at startup with `mlock()`.

---

## Part 11: Market Microstructure

### Q: Explain the spread and why it matters.

**A:** The spread is `best_ask - best_bid`. If the best bid is 99.50 and the best ask is 100.00, the spread is 0.50.

The spread represents the cost of immediacy. If you want to buy right now, you pay the ask (100.00). If you want to sell right now, you get the bid (99.50). The difference (0.50) is the profit captured by whoever provided those quotes — the market maker.

In my engine, you can see the spread with the `PRINT` command:

```
--- ASKS ---
    100.00  |  qty=200
--- SPREAD ---          ← spread = 100.00 - 99.50 = 0.50
--- BIDS ---
     99.50  |  qty=150
```

Tighter spreads mean more liquid markets. HFT firms compete to tighten spreads, earning the bid-ask spread on millions of trades per day.

### Q: What's "queue position" and why does it matter for HFT?

**A:** When multiple orders rest at the same price, they're matched in FIFO order. If I'm 5th in line at price 100.00, I won't get filled until the 4 orders ahead of me are filled. My "queue position" is 5.

This matters enormously for HFT market-making strategies. If a market maker can get to the front of the queue (by being the first to place an order at a new price level), they get filled first and earn the spread. If they're at the back, the price might move away before they're filled.

In my engine, queue position is determined by the `std::deque` order within a `PriceLevel`. First `push_back` = first `front()` during matching.

This is why order modification (amend) is critical: if you can change your order's quantity without losing your queue position, you can manage risk without sacrificing your place in line.

### Q: What does "price improvement" mean in the context of your engine?

**A:** When an aggressive order gets a better price than it asked for.

Example: A buy limit order at 101.00 arrives. The best resting ask is at 100.00. The trade executes at 100.00 — the buyer wanted to pay up to 101, but only paid 100. That's $1.00 of price improvement per share.

In my code, this happens naturally because the matching loop always trades at the **resting** order's price:

```cpp
trades.push_back(execute_trade(incoming, resting, fill_qty, ask_price));
//                                                          ^^^^^^^^^
//                                                    resting price (100.00)
//                                                    not incoming price (101.00)
```

### Q: What happens to the order book around market events? How would that stress your engine?

**A:** During high-volatility events (earnings, FOMC announcements, flash crashes):

1. **Cancel storm:** Market makers pull their quotes. Cancel rate goes from 5% to 80%+. My engine handles this efficiently — cancels are the cheapest operation (~42ns p50).

2. **Price sweeps:** Large aggressive orders sweep through multiple price levels. This triggers the multi-level matching loop, which is the most expensive code path. The p99 latency increases.

3. **Book rebuilding:** After the sweep, new limit orders flood in to rebuild the book. Many insertions at different price levels. `std::map` insert is O(log n), which scales well.

4. **Order rate spike:** Overall message rate might 10x. If my engine normally processes 1M orders/sec and the spike is 10M/sec, I'm still under my 8.5M/sec capacity. If it exceeded capacity, I'd need to queue orders, adding latency.

---

## Part 12: The "Stump the Intern" Round

### Q: Your benchmark measures latency per order. But you're using `chrono::high_resolution_clock` which has its own overhead. How does that affect your results?

**A:** Good catch. `chrono::high_resolution_clock::now()` calls `clock_gettime()`, which takes ~20-30ns on macOS. I call it **twice** per order (start and end). That's ~40-60ns of measurement overhead added to every latency sample.

This means my reported latencies are inflated by ~40-60ns. The true matching latency is probably ~25-40ns for p50, not the reported ~83ns. The relative differences between operations are still valid (cancels are cheaper than matches), but the absolute numbers include the measurement tax.

To fix this: use `__rdtsc()` (read timestamp counter), which is a single instruction (~1ns overhead). Or batch timing: measure the total time for 1000 orders and divide, which amortizes the measurement overhead.

### Q: Your `PriceLevel` uses `std::deque`. What's actually happening in memory when you call `push_back`?

**A:** `std::deque` is implemented as an array of pointers to fixed-size chunks (typically 512 bytes or enough for ~8-64 elements per chunk depending on element size). Each chunk is a contiguous array.

When `push_back` is called:
1. If the last chunk has space, the pointer is appended there. O(1), no allocation.
2. If the last chunk is full, a new chunk is allocated and the pointer goes there. This causes a heap allocation — a latency spike.

For my `PriceLevel` storing `Order*` (8 bytes each), each chunk holds ~64 pointers. So every 64th `push_back` triggers a `malloc`. This is why production systems might use a pre-allocated ring buffer instead.

### Q: In `execute_trade`, you increment `filled_qty` on both orders. What if there's an integer overflow?

```cpp
buyer->filled_qty += qty;
seller->filled_qty += qty;
```

**A:** `filled_qty` is `uint32_t` (max ~4.3 billion). `quantity` is also `uint32_t`. Since `filled_qty` can never exceed `quantity` (we only fill up to `remaining()`), and `quantity` is at most ~4.3 billion, overflow is impossible — you'd need a single order for 4.3 billion shares, which no exchange allows.

However, there's a subtler issue: `filled_qty` could theoretically overflow if there's a bug that causes double-counting. The `is_filled()` check uses `>=`:

```cpp
bool is_filled() const { return filled_qty >= quantity; }
```

The `>=` (instead of `==`) is defensive — if `filled_qty` somehow exceeds `quantity` due to a bug, the order is still considered filled rather than having a negative `remaining()` (which would underflow since Quantity is unsigned).

### Q: The matching engine owns the ObjectPool, but the OrderBook holds raw `Order*` pointers. What guarantees these pointers remain valid?

**A:** The ownership contract:

1. The MatchingEngine creates Orders via `pool_.allocate()`.
2. Unmatched orders are placed in the OrderBook via `book_.add_order(order)`.
3. The OrderBook holds borrowed `Order*` pointers — it never allocates or deallocates.
4. Orders leave the book in only two ways: `cancel_order()` (returns the pointer to the caller) or `remove_from_lookup()` during matching (caller has already popped from the level).
5. After removal, the MatchingEngine calls `pool_.deallocate(order)`.

The invariant: **every `Order*` in the book points to a live, pool-allocated object.** This is guaranteed because:
- Only the MatchingEngine adds orders to the book (after allocating from the pool)
- Only the MatchingEngine removes orders from the book (and immediately deallocates)
- The pool never moves or frees blocks while orders are live

If the pool's destructor runs while the book still has orders, those pointers become dangling. But the pool lives inside the MatchingEngine, which also owns the book — they're destroyed together. The book's destructor runs first (member destruction order is reverse declaration order), but it doesn't dereference any pointers during destruction.

### Q: Your TCP server uses a `vector<ClientState>` for clients. What happens when a client is removed while you're iterating events?

**A:** This is a subtle correctness issue. The event loop processes events in a batch:

```cpp
int n = kevent(kqueue_fd_, nullptr, 0, events, 64, &timeout);
for (int i = 0; i < n; ++i) {
    if (events[i].ident == listen_fd_)
        accept_client();
    else
        handle_client_data(fd);  // May call remove_client()
}
```

`handle_client_data` may call `remove_client`, which erases from the `clients_` vector. But the event array (`events`) is a local copy — it doesn't reference the clients vector. Each event just has an `fd` number.

The potential issue: if the same client has multiple events in the batch (e.g., data + EOF), and we remove the client on the first event, the second event references a closed fd. The code handles this because:
1. `handle_client_data` uses `std::find_if` to locate the client by fd — if removed, it's not found, and the function returns early.
2. EOF events are handled first (the `EV_EOF` check comes before the read path).

But there's a remaining edge case: if a client's fd is reused by `accept_client` within the same event batch (new connection gets the same fd number). This is extremely unlikely but theoretically possible. A production server would use a generation counter or handle all events for one fd before processing the next.

### Q: If an HFT firm asked you to optimize this for 2x throughput, where would you look first?

**A:** In order of expected impact:

1. **Replace `vector<Trade>` return with a callback or pre-allocated buffer.** Eliminates heap allocation on every `process_order` call. Expected improvement: 10-20%.

2. **`order_lookup_.reserve()` at startup.** Eliminates hash map rehashing spikes. Improves p99.9 dramatically.

3. **Replace `std::map` with a flat price-indexed array** (for instruments with known price ranges). Eliminates tree traversal overhead. Expected improvement: 20-30%.

4. **Compile with `-O3 -march=native -flto`.** Link-time optimization can inline across translation units. Expected improvement: 5-15%.

5. **Pin to a CPU core with `taskset`/`cpuset`.** Eliminates context switches and keeps caches warm. Expected improvement: 5-10%.

6. **Use `__rdtsc` instead of `chrono` in benchmarks.** Reduces measurement overhead, though this doesn't improve production performance.

7. **Use `mmap` + `mlock` for the pool blocks.** Pre-faults pages and prevents them from being swapped. Eliminates minor page faults on first access to new blocks.

Combined, these could realistically double throughput to ~15-20M orders/sec.

### Q: Final question. You're an undergrad. Why should we believe you understand this well enough to work on our production systems?

**A:** I won't pretend this is production-grade. It's not. It doesn't have crash recovery, self-trade prevention, order modification, multi-instrument support, or kernel bypass networking.

What it demonstrates:

1. **I understand the core data structures.** I chose `std::map` for sorted access, `std::deque` for FIFO queues, `unordered_map` for O(1) lookups, and I know the trade-offs of each choice. I can tell you exactly when to use each one and when to upgrade to something more specialized.

2. **I understand memory.** I wrote an object pool from scratch, I know why cache-line alignment matters, I know the cost of `new` vs pool allocation, and I can reason about cache behavior.

3. **I understand the matching algorithm.** Price-time priority, partial fills, market vs limit orders, why trades execute at the resting price. I didn't just implement it — I can walk through it instruction by instruction.

4. **I found and fixed a real bug.** The iterator invalidation bug in the matching loop (calling `cancel_order` during iteration) is the kind of bug that causes production outages. I found it with unit tests, diagnosed it correctly, and fixed it by separating the code paths.

5. **I know what I don't know.** I can list the gaps between this and production. I know what kernel bypass is. I know what SPSC queues are. I know why exchanges use FPGA NICs. I haven't built those things, but I understand why they exist and what problems they solve.

I built this to learn, and I learned a lot. I'm looking to learn the rest by working on real systems with people who've done this at scale.
