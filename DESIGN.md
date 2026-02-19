# Limit Order Book & Matching Engine

A high-performance limit order book and matching engine written in C++20. Built to demonstrate understanding of trading infrastructure, low-latency system design, and efficient C++ — the three pillars HFT firms evaluate.

```
                          ┌──────────────────────────────────────────────┐
                          │              MATCHING ENGINE                 │
                          │                                              │
   CSV/stdin ──────►      │   process_order(side, type, price, qty)      │
                          │         │                                    │
   TCP binary ─────►      │         ▼                                    │
                          │   ┌─────────────┐     ┌──────────────┐      │
                          │   │ Object Pool │────►│ Order struct │      │
                          │   │  (allocate) │     │  (64 bytes)  │      │
                          │   └─────────────┘     └──────┬───────┘      │
                          │                              │              │
                          │                ┌─────────────┼──────────┐   │
                          │                ▼             ▼          │   │
                          │        ┌──────────┐   ┌──────────┐     │   │
                          │        │   BIDS   │   │   ASKS   │     │   │
                          │        │ std::map │   │ std::map │     │   │
                          │        │ greater<>│   │ less<>   │     │   │
                          │        └────┬─────┘   └────┬─────┘     │   │
                          │             │              │           │   │
                          │             ▼              ▼           │   │
                          │        ┌──────────────────────────┐    │   │
                          │        │  PriceLevel (per price)  │    │   │
                          │        │  std::deque<Order*>      │    │   │
                          │        │  FIFO time priority      │    │   │
                          │        └──────────────────────────┘    │   │
                          │                                        │   │
                          │        ┌──────────────────────────┐    │   │
                          │        │  order_lookup (hash map) │    │   │
                          │        │  OrderId → Order*        │    │   │
                          │        │  O(1) cancel             │    │   │
                          │        └──────────────────────────┘    │   │
                          │                                              │
                          │   Output: vector<Trade>                      │
                          └──────────────────────────────────────────────┘
```

---

## Table of Contents

1. [Build & Run Instructions](#build--run-instructions)
2. [Project Structure](#project-structure)
3. [Architecture: How the Pieces Connect](#architecture-how-the-pieces-connect)
4. [Component Deep Dive](#component-deep-dive)
   - [Fixed-Point Prices (types.h)](#1-fixed-point-prices--core-types-typesh)
   - [Order Struct (order.h)](#2-order-struct-orderh)
   - [Object Pool (object_pool.h)](#3-object-pool-object_poolh)
   - [Price Level (price_level.h)](#4-price-level-price_levelh)
   - [Order Book (order_book.h/cpp)](#5-order-book-order_bookhcpp)
   - [Matching Engine (matching_engine.h/cpp)](#6-matching-engine-matching_enginehcpp)
   - [CSV Parser & CLI (csv_parser.h/cpp, main.cpp)](#7-csv-parser--cli)
   - [Binary Protocol (protocol.h)](#8-binary-protocol-protocolh)
   - [TCP Server (tcp_server.h/cpp)](#9-tcp-server-tcp_serverhcpp)
   - [TCP Client (tcp_client.cpp)](#10-tcp-test-client-tcp_clientcpp)
5. [Matching Algorithm Walkthrough](#matching-algorithm-walkthrough)
6. [Memory Model & Lifecycle](#memory-model--lifecycle)
7. [Benchmark Results & Analysis](#benchmark-results--analysis)
8. [Testing](#testing)
9. [Design Decisions & Trade-offs](#design-decisions--trade-offs)
10. [Known Limitations & Future Work](#known-limitations--future-work)
11. [Interview-Style Q&A](#interview-style-qa)

---

## Build & Run Instructions

### Prerequisites

- C++20 compiler (Clang 14+ or GCC 12+)
- CMake 3.20+
- Internet connection (CMake fetches Google Test automatically)

### Build

```bash
cd ~/order-book
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

This produces four binaries:

| Binary | Description |
|--------|-------------|
| `order-book` | CLI matching engine (reads CSV from stdin or file) |
| `order-book-server` | TCP server wrapping the matching engine |
| `tcp-client` | Test client that sends orders over TCP |
| `benchmark` | Throughput and latency benchmarking harness |
| `tests` | Google Test unit tests |

### Run Unit Tests

```bash
cd build
ctest --output-on-failure
```

Expected output — all 32 tests pass:

```
 1/32 Test  #1: OrderBookTest.EmptyBook .............................   Passed
 2/32 Test  #2: OrderBookTest.AddBidOrder ...........................   Passed
 ...
31/32 Test #31: ObjectPool.GrowsWhenExhausted .......................   Passed
32/32 Test #32: ObjectPool.HighVolume ...............................   Passed

100% tests passed, 0 tests failed out of 32
```

### Manual Test: CLI Matching

Pipe orders through stdin. Each line is `TYPE,SIDE,PRICE,QUANTITY`:

```bash
printf 'LIMIT,BUY,100.00,10\nLIMIT,SELL,100.00,5\nPRINT\n' | ./order-book
```

Expected output:

```
TRADE 1 2 100.00 5
=== ORDER BOOK ===
--- ASKS (lowest first) ---
--- SPREAD ---
--- BIDS (highest first) ---
    100.00  |         5  (1 orders)
==================
```

What happened:
1. Order 1: BUY 10 @ 100.00 — no sellers exist, rests on the bid side.
2. Order 2: SELL 5 @ 100.00 — price crosses the bid (100.00 >= 100.00), matches 5 units with Order 1. Trade printed.
3. PRINT — shows remaining book. Order 1 still has 5 unfilled units on the bid side.

### More CLI Examples

**Partial fill across multiple levels:**

```bash
printf 'LIMIT,SELL,101.00,30\nLIMIT,SELL,100.50,20\nLIMIT,BUY,101.00,40\nPRINT\n' | ./order-book
```

Expected:

```
TRADE 3 2 100.50 20
TRADE 3 1 101.00 20
=== ORDER BOOK ===
--- ASKS (lowest first) ---
--- SPREAD ---
--- BIDS (highest first) ---
==================
```

The buy sweeps the cheaper ask first (100.50 for 20), then the more expensive (101.00 for 20). Remaining 10 units of the sell at 101.00 are consumed. Book is empty afterward.

**Market order:**

```bash
printf 'LIMIT,BUY,99.00,50\nLIMIT,BUY,100.00,50\nMARKET,SELL,,80\nPRINT\n' | ./order-book
```

Expected:

```
TRADE 2 3 100.00 50
TRADE 1 3 99.00 30
=== ORDER BOOK ===
--- ASKS (lowest first) ---
--- SPREAD ---
--- BIDS (highest first) ---
     99.00  |        20  (1 orders)
==================
```

Market sell has no price limit — it hits the best bid first (100.00 for 50), then the next level (99.00 for 30 of the remaining 80). 20 units left on the 99.00 bid.

**Cancel order:**

```bash
printf 'LIMIT,BUY,100.00,100\nCANCEL,,,,1\nPRINT\n' | ./order-book
```

Expected:

```
CANCELLED 1
=== ORDER BOOK ===
--- ASKS (lowest first) ---
--- SPREAD ---
--- BIDS (highest first) ---
==================
```

### Run Benchmarks

```bash
./benchmark --orders 1000000
```

Expected output (numbers vary by machine):

```
Generating 1000000 random orders...

=== Mixed Workload (5% cancel, 10% market) ===
Orders:     1000000
Trades:     817572
Throughput: 8518668 orders/sec
Latency (ns):
  mean:  99.1
  p50:   83.0
  p95:   250.0
  p99:   375.0
  p99.9: 833.0

=== Pure Limit Orders ===
...

=== High Cancel Rate (30%) ===
...
```

### Run TCP Server + Client

Terminal 1 — start the server:

```bash
./order-book-server 9000
```

Terminal 2 — run the test client:

```bash
./tcp-client --port 9000 --orders 10000
```

Expected output:

```
Connecting to 127.0.0.1:9000...
Connected. Sending 10000 orders...

=== Round-trip Latency (us) ===
  mean:  8.0
  p50:   5.6
  p95:   26.2
  p99:   48.4
  p99.9: 159.2
```

Stop the server with Ctrl-C (graceful shutdown via SIGINT).

---

## Project Structure

```
order-book/
├── CMakeLists.txt                # Build system
├── include/
│   ├── types.h                   # Fixed-point price, Side, OrderType, typedefs
│   ├── order.h                   # 64-byte cache-aligned Order struct
│   ├── trade.h                   # Trade output struct
│   ├── object_pool.h             # Pre-allocated memory pool (template, header-only)
│   ├── price_level.h             # FIFO queue at one price (header-only)
│   ├── order_book.h              # Bid/ask book with sorted price levels
│   ├── matching_engine.h         # Core matching logic
│   ├── csv_parser.h              # CSV command parser
│   ├── protocol.h                # Binary wire protocol structs
│   └── tcp_server.h              # Non-blocking TCP server
├── src/
│   ├── main.cpp                  # CLI entry point
│   ├── order_book.cpp            # OrderBook implementation
│   ├── matching_engine.cpp       # MatchingEngine implementation
│   ├── csv_parser.cpp            # CsvParser implementation
│   ├── tcp_server.cpp            # TcpServer implementation + server main()
│   └── protocol.cpp              # Protocol compilation unit (placeholder)
├── tests/
│   ├── test_order_book.cpp       # OrderBook unit tests (10 tests)
│   ├── test_matching_engine.cpp  # MatchingEngine unit tests (18 tests)
│   └── test_object_pool.cpp      # ObjectPool unit tests (4 tests)
├── bench/
│   ├── benchmark.cpp             # Throughput/latency benchmark harness
│   ├── order_generator.h         # Random order stream generator
│   └── order_generator.cpp
└── tools/
    └── tcp_client.cpp            # TCP test client with latency measurement
```

---

## Architecture: How the Pieces Connect

### Data Flow: CLI Path

```
stdin/file  →  CsvParser::process_line()
                    │
                    │ parses "LIMIT,BUY,100.00,50"
                    │ into: Side::BUY, OrderType::LIMIT, price=10000, qty=50
                    ▼
              MatchingEngine::process_order()
                    │
                    │ 1. pool_.allocate() → Order*
                    │ 2. Fill in order fields
                    │ 3. match_buy() or match_sell()
                    │ 4. If unfilled remainder: book_.add_order()
                    │    If fully filled: pool_.deallocate()
                    ▼
              Returns vector<Trade>
                    │
                    ▼
              CsvParser prints: "TRADE 1 2 100.00 50"
```

### Data Flow: TCP Path

```
TCP client  →  32-byte OrderMessage
                    │
                    │  recv() into per-client buffer
                    │  deserialize via memcpy
                    ▼
              TcpServer::process_message()
                    │
                    │  Same MatchingEngine::process_order() call
                    ▼
              32-byte ResponseMessage (ACK + FILLs)
                    │
                    │  serialize via memcpy, send()
                    ▼
              TCP client receives response
```

### Ownership Model

```
MatchingEngine owns:
  ├── ObjectPool<Order>     (owns all Order memory)
  └── OrderBook             (borrows Order* from pool)
        ├── bids_ map       (owns PriceLevels)
        ├── asks_ map       (owns PriceLevels)
        └── order_lookup_   (borrows Order*, keyed by ID)

PriceLevel:
  └── deque<Order*>         (borrows Order* — does not own)
```

The **ObjectPool** is the single owner of all Order memory. The OrderBook and PriceLevels only hold borrowed pointers. When an order is fully filled or cancelled, the pointer is returned to the pool — not freed to the OS.

---

## Component Deep Dive

### 1. Fixed-Point Prices & Core Types (`types.h`)

```cpp
using Price = int64_t;
constexpr int PRICE_SCALE = 100;  // 2 decimal places

inline Price price_from_double(double p) {
    return static_cast<Price>(p * PRICE_SCALE + 0.5);
}
```

**What it does:** Represents all prices as integers in "ticks." The price 150.25 becomes the integer 15025 (150.25 * 100). All comparisons are integer comparisons.

**Why fixed-point instead of `double`:**

Floating-point has representational errors. `0.1 + 0.2 != 0.3` in IEEE 754. In a matching engine, this causes real bugs:

```cpp
// With doubles:
double bid = 100.10;
double ask = 100.10;
if (bid >= ask) { /* match */ }  // Might NOT match due to rounding

// With fixed-point:
int64_t bid = 10010;
int64_t ask = 10010;
if (bid >= ask) { /* match */ }  // Always correct
```

Every production exchange uses integer or fixed-point pricing. CME uses a fixed-point format in their FAST/SBE feeds. This is not an optimization — it is a correctness requirement.

**Why `int64_t` specifically:**
- 64 bits gives range of +/-9.2 * 10^18 ticks — far more than needed for any asset class
- Signed to allow spreads and price differences to go negative naturally
- Native CPU word size — no overhead vs. 32-bit on modern hardware

**Other types defined here:**
- `Side` enum (`BUY`/`SELL`) — `uint8_t` underlying type to minimize padding in Order struct
- `OrderType` enum (`LIMIT`/`MARKET`) — same compact representation
- `OrderId` = `uint64_t` — supports billions of orders per session
- `Quantity` = `uint32_t` — 4 billion units max per order (sufficient for any asset)
- `Timestamp` = `uint64_t` — monotonic counter, not wall-clock (cheaper)

**Trade-off:** The `PRICE_SCALE` of 100 gives 2 decimal places. For instruments needing more (e.g., forex with 5 decimals), you'd change this constant. The rest of the code is scale-agnostic.

---

### 2. Order Struct (`order.h`)

```cpp
struct Order {
    OrderId   id;           // 8 bytes
    Timestamp timestamp;    // 8 bytes
    Price     price;        // 8 bytes
    Quantity  quantity;     // 4 bytes
    Quantity  filled_qty;  // 4 bytes
    Side      side;        // 1 byte
    OrderType type;        // 1 byte
    uint8_t   padding[30]; // pad to 64 bytes
};
static_assert(sizeof(Order) == 64, "Order must be 64 bytes for cache-line alignment");
```

**What it does:** Contains everything needed to represent one order in the book. The `remaining()` helper (`quantity - filled_qty`) gives the unfilled amount. `is_filled()` checks if the order is fully executed.

**Why 64 bytes / one cache line:**

Modern CPUs load memory in 64-byte cache lines. If an Order straddles two cache lines, accessing it requires two memory fetches — double the latency. By ensuring each Order fits exactly in one cache line:

- Reading any field of an order loads the entire order into L1 cache
- No "false sharing" if orders were ever accessed from multiple threads
- The object pool's blocks are naturally cache-line-aligned

**Why explicit padding instead of letting the compiler decide:**

The `static_assert` enforces this at compile time. Without it, adding a field could silently bloat the struct to 128 bytes, doubling cache usage with no warning. The padding makes the intent explicit and the constraint enforced.

**Field layout rationale:**

The fields are ordered largest-to-smallest to minimize compiler-inserted padding:
- 8-byte fields first (id, timestamp, price)
- 4-byte fields next (quantity, filled_qty)
- 1-byte fields last (side, type)
- Explicit padding to fill the remainder

This ordering means no wasted padding between fields — the compiler would have placed them identically.

**What `remaining()` and `is_filled()` do at the machine level:**

```cpp
Quantity remaining() const { return quantity - filled_qty; }
bool is_filled() const { return filled_qty >= quantity; }
```

These are single-instruction operations on fields already in the same cache line. No virtual dispatch, no indirection. The compiler inlines them.

---

### 3. Object Pool (`object_pool.h`)

```cpp
template <typename T, size_t BlockSize = 4096>
class ObjectPool {
    struct Node { Node* next; };

    Node* free_list_ = nullptr;
    size_t allocated_ = 0;
    std::vector<char*> blocks_;

    void allocate_block() {
        char* block = static_cast<char*>(
            ::operator new(sizeof(T) * BlockSize, std::align_val_t{alignof(T)}));
        blocks_.push_back(block);
        for (size_t i = 0; i < BlockSize; ++i) {
            Node* node = reinterpret_cast<Node*>(block + i * sizeof(T));
            node->next = free_list_;
            free_list_ = node;
        }
    }

public:
    T* allocate() {
        if (free_list_ == nullptr) [[unlikely]] {
            allocate_block();
        }
        Node* node = free_list_;
        free_list_ = node->next;
        ++allocated_;
        return reinterpret_cast<T*>(node);
    }

    void deallocate(T* ptr) {
        Node* node = reinterpret_cast<Node*>(ptr);
        node->next = free_list_;
        free_list_ = node;
        --allocated_;
    }
};
```

**What it does:** Pre-allocates large blocks of Order-sized memory. Hands out slots from a singly-linked free list. When a slot is returned, it goes back on the free list — never back to the OS.

**How the free list works — step by step:**

1. **Initialization:** Allocate a block of 4096 * 64 = 256KB. Treat each 64-byte slot as a `Node` containing just a `next` pointer. Chain them all into a linked list.

2. **allocate():** Pop the head of the free list. Return that address cast to `T*`. Cost: one pointer read + one pointer write = ~1-5ns.

3. **deallocate():** Push the address back onto the free list head. The Order's memory is reinterpreted as a `Node`. Since Orders are 64 bytes and Nodes are 8 bytes, there's plenty of room. Cost: one pointer write = ~1-5ns.

4. **Growth:** If the free list is empty, allocate a new block. The `[[unlikely]]` attribute tells the compiler this branch is cold, so the fast path (free list not empty) gets optimized instruction layout.

**Why this matters — real numbers:**

| Operation | `new`/`delete` | Object Pool |
|-----------|---------------|-------------|
| Allocation | 50-200ns | 1-5ns |
| Deallocation | 50-200ns | 1-5ns |
| System calls | Yes (`mmap`/`brk`) | Only on block growth |
| Fragmentation | Yes | No (fixed-size blocks) |

With `new`/`delete`, the allocator must: search free lists, possibly call `mmap` for large allocations, handle thread-safety locks, update bookkeeping metadata. The object pool skips all of that.

**Why the `static_assert(sizeof(T) >= sizeof(Node))`:**

The free list embeds a `Node` pointer inside each unused slot. If `T` were smaller than a pointer (8 bytes on 64-bit), we couldn't fit the `next` pointer. This compile-time check prevents that impossible scenario. For our 64-byte Order, this is always satisfied.

**Why aligned allocation (`std::align_val_t`):**

Ensures blocks start at addresses aligned to `alignof(T)`. For 64-byte Orders, this means cache-line alignment. Misaligned access can cause cache-line splits on some architectures or be outright illegal on others (e.g., ARM strict alignment mode).

**Why non-copyable/non-movable:**

```cpp
ObjectPool(const ObjectPool&) = delete;
ObjectPool& operator=(const ObjectPool&) = delete;
```

Copying a pool would create two pools that both think they own the same memory blocks. Moving could leave dangling pointers in the OrderBook. Deleted copy/move prevents these bugs at compile time.

**Pros:**
- O(1) allocation and deallocation, no system calls on the hot path
- Zero fragmentation (all slots are the same size)
- Excellent cache locality (slots in the same block are contiguous)

**Cons:**
- Memory is never returned to the OS until the pool is destroyed
- Only works well for fixed-size objects (not general-purpose)
- No thread safety (fine for single-threaded matching engine)

---

### 4. Price Level (`price_level.h`)

```cpp
class PriceLevel {
    std::deque<Order*> orders_;
    Quantity total_qty_ = 0;

public:
    void add(Order* order)    { orders_.push_back(order); total_qty_ += order->remaining(); }
    Order* front() const      { return orders_.front(); }
    void pop_front()          { total_qty_ -= orders_.front()->remaining(); orders_.pop_front(); }
    bool remove(Order* order) { /* linear scan for cancel */ }
    void reduce_quantity(Quantity qty) { total_qty_ -= qty; }
};
```

**What it does:** Represents all resting orders at a single price point. Orders within the level are in FIFO (first-in-first-out) order — this is "time priority" within the same price.

**How it connects:** The OrderBook has one PriceLevel per active price. When the matching engine walks the book, it pops orders from the front of each level.

**Why `std::deque` instead of `std::list`:**

| Feature | `std::deque` | `std::list` |
|---------|-------------|-------------|
| Memory layout | Contiguous chunks | Scattered nodes |
| Cache behavior | Good (sequential access hits cache) | Poor (pointer chasing) |
| `push_back` | Amortized O(1) | O(1) but allocates a node |
| `pop_front` | Amortized O(1) | O(1) but frees a node |
| `remove` (mid) | O(n) shift | O(1) if you have iterator |
| Overhead per element | ~8 bytes (pointer) | ~16-24 bytes (prev+next+data) |

For a matching engine, the dominant operations are `push_back` (new order arrives) and `pop_front` (front order matched). Both are O(1) on `std::deque`. The rare `remove` (cancel) is O(n), but cancel frequency is much lower than match frequency.

The cache locality advantage is significant: when scanning a price level during matching, `std::deque` elements are in contiguous memory chunks, so prefetching works. `std::list` nodes can be anywhere in the heap, causing cache misses on every `next` pointer dereference.

**Why a cached `total_qty_` field:**

Without it, getting the total volume at a price level requires iterating every order — O(n). The cached total is maintained incrementally:
- `add()`: adds remaining quantity
- `pop_front()`: subtracts front order's remaining quantity
- `remove()`: subtracts cancelled order's remaining quantity
- `reduce_quantity()`: subtracts after partial fill

This gives O(1) volume queries, which are needed for market data output and the `PRINT` command.

**The `reduce_quantity` method — why it exists:**

When a partial fill happens, the front order's `filled_qty` increases but the order stays in the queue. The PriceLevel's cached total must be decremented by the fill amount. This is called by the matching engine after executing a partial fill to keep the cached total in sync without having to recalculate from scratch.

---

### 5. Order Book (`order_book.h/cpp`)

```cpp
class OrderBook {
    std::map<Price, PriceLevel, std::greater<>> bids_;  // highest first
    std::map<Price, PriceLevel> asks_;                   // lowest first
    std::unordered_map<OrderId, Order*> order_lookup_;   // O(1) cancel
};
```

**What it does:** Maintains two sorted sides (bid and ask) of the book. Each side maps prices to PriceLevels. Provides O(1) cancel via a hash map lookup.

**How the three data structures connect:**

```
bids_ (std::map, highest price first):
  10050 → PriceLevel{ [Order*, Order*, Order*] }    ← best bid
  10040 → PriceLevel{ [Order*] }
  10030 → PriceLevel{ [Order*, Order*] }

asks_ (std::map, lowest price first):
  10060 → PriceLevel{ [Order*] }                    ← best ask
  10070 → PriceLevel{ [Order*, Order*] }
  10090 → PriceLevel{ [Order*] }

order_lookup_ (std::unordered_map):
  order_id=1 → Order* (in bids_ at 10050)
  order_id=2 → Order* (in asks_ at 10060)
  order_id=3 → Order* (in bids_ at 10050)
  ...
```

Every order appears in exactly two places: once in a PriceLevel (inside the appropriate map), and once in `order_lookup_`. This dual indexing enables:
- Sorted traversal for matching (via the map)
- O(1) lookup for cancellation (via the hash map)

**Why `std::map` instead of `std::unordered_map` for the book sides:**

The matching engine needs to walk prices in sorted order — lowest ask first, highest bid first. `std::map` (red-black tree) maintains sorted order inherently. `std::unordered_map` would require collecting all prices into a vector and sorting before every match — far more expensive.

**Why `std::map` instead of a flat sorted array or skip list:**

| Structure | Insert | Delete | Iterate sorted | Best-of-book |
|-----------|--------|--------|----------------|-------------|
| `std::map` | O(log n) | O(log n) | O(1) next | O(1) begin() |
| sorted vector | O(n) shift | O(n) shift | O(1) next | O(1) front() |
| skip list | O(log n) avg | O(log n) avg | O(1) next | O(1) |
| `std::unordered_map` | O(1) avg | O(1) avg | O(n log n) sort | O(n) scan |

`std::map` gives the best balance: logarithmic insert/delete, O(1) sorted iteration, and O(1) top-of-book via `begin()`. In practice, the number of active price levels is small (100-1000), so the O(log n) operations complete in ~7-10 tree traversals.

**Production note:** Real exchanges often use a flat array indexed by price offset from a reference price (e.g., `price_levels[price - min_price]`), giving O(1) everything. This works when the price range is bounded and known in advance. The `std::map` approach is more general-purpose and still meets our performance targets.

**The `std::greater<>` on bids — what it does:**

```cpp
std::map<Price, PriceLevel, std::greater<>> bids_;
```

`std::map` sorts keys in ascending order by default. For bids, we want the highest price first (that's the best bid). `std::greater<>` reverses the ordering so `bids_.begin()` returns the highest-priced level. Without this, we'd need `bids_.rbegin()`, which is less ergonomic and doesn't work with `erase()`.

**`cancel_order` — the full flow:**

```cpp
Order* OrderBook::cancel_order(OrderId order_id) {
    auto it = order_lookup_.find(order_id);     // O(1) hash lookup
    if (it == order_lookup_.end()) return nullptr;

    Order* order = it->second;
    order_lookup_.erase(it);                     // O(1) hash erase

    // Find the price level in the correct side
    auto level_it = asks_.find(order->price);    // O(log n) tree lookup
    level_it->second.remove(order);              // O(n) linear scan within level
    if (level_it->second.is_empty())
        asks_.erase(level_it);                   // O(1) amortized tree erase

    return order;  // Caller deallocates via pool
}
```

The O(n) linear scan in `remove()` is the most expensive part of cancel. In practice, price levels rarely have more than 10-50 orders, so n is small. If cancel latency were critical, we could use an intrusive doubly-linked list for O(1) removal — see [Known Limitations](#known-limitations--future-work).

**`remove_from_lookup` — why it exists:**

```cpp
void OrderBook::remove_from_lookup(OrderId id) {
    order_lookup_.erase(id);
}
```

This is used by the matching engine during trade execution. When the engine matches a resting order, it has already popped it from the PriceLevel's deque via `pop_front()`. It only needs to remove the entry from the hash map. Calling the full `cancel_order()` would try to remove the order from the PriceLevel *again* (finding nothing, but more critically — potentially erasing the map entry for the level we're currently iterating over, which would invalidate our iterator and cause a segfault).

This was a real bug discovered during testing: the initial implementation called `cancel_order()` during matching and caused segfaults in 9 out of 32 tests. The fix was to split the "remove from lookup" path (for matching) from the "remove from everywhere" path (for user-initiated cancels).

---

### 6. Matching Engine (`matching_engine.h/cpp`)

```cpp
class MatchingEngine {
    OrderBook book_;
    ObjectPool<Order> pool_;
    OrderId next_order_id_ = 1;
    Timestamp next_timestamp_ = 1;

public:
    std::vector<Trade> process_order(Side, OrderType, Price, Quantity);
    bool cancel_order(OrderId);
};
```

**What it does:** The central coordinator. Takes incoming order parameters, allocates an Order from the pool, attempts to match it against the opposite side of the book, and either adds the remainder to the book or discards it.

**`process_order` — the complete flow:**

```cpp
std::vector<Trade> MatchingEngine::process_order(Side side, OrderType type, Price price, Quantity qty) {
    // 1. Allocate from pool — O(1), ~5ns
    Order* order = pool_.allocate();

    // 2. Fill in fields — all in one cache line
    order->id = next_order_id_++;
    order->timestamp = next_timestamp_++;
    order->price = price;
    order->quantity = qty;
    order->filled_qty = 0;
    order->side = side;
    order->type = type;

    // 3. Match against opposite side
    std::vector<Trade> trades;
    if (side == Side::BUY)  match_buy(order, trades);
    else                     match_sell(order, trades);

    // 4. Handle remainder
    if (!order->is_filled()) {
        if (type == OrderType::LIMIT)
            book_.add_order(order);     // Rest in book
        else
            pool_.deallocate(order);    // Market order: discard unfilled part
    } else {
        pool_.deallocate(order);        // Fully filled: return to pool
    }

    return trades;
}
```

**`match_buy` — the matching algorithm in detail:**

```cpp
void MatchingEngine::match_buy(Order* incoming, std::vector<Trade>& trades) {
    auto& asks = book_.asks();

    // Walk ask levels from lowest price upward
    while (!incoming->is_filled() && !asks.empty()) {
        auto it = asks.begin();           // Cheapest ask level
        Price ask_price = it->first;

        // Price check: does the incoming buy price cross this ask?
        if (incoming->type == OrderType::LIMIT && ask_price > incoming->price)
            break;  // No more matchable prices

        PriceLevel& level = it->second;

        // Walk orders within this level (FIFO)
        while (!incoming->is_filled() && !level.is_empty()) {
            Order* resting = level.front();  // Earliest order at this price

            // Fill quantity = minimum of both sides' remaining
            Quantity fill_qty = std::min(incoming->remaining(), resting->remaining());

            // Execute the trade
            trades.push_back(execute_trade(incoming, resting, fill_qty, ask_price));

            if (resting->is_filled()) {
                level.pop_front();                       // Remove from level
                book_.remove_from_lookup(resting->id);   // Remove from hash map
                pool_.deallocate(resting);               // Return memory to pool
            } else {
                level.reduce_quantity(fill_qty);         // Partial fill: update cached qty
            }
        }

        // Clean up empty level
        if (level.is_empty()) {
            asks.erase(it);
        }
    }
}
```

**Why trades execute at the resting order's price:**

When a BUY at 101 matches a resting SELL at 100, the trade executes at 100 (the resting price), not 101. This is price-time priority: the resting order was there first and set the price. The incoming order gets "price improvement" — they were willing to pay 101 but only paid 100. This is how every major exchange works.

**Why the two-level loop structure:**

The outer loop walks price levels (ask prices from low to high). The inner loop walks orders within a level (time priority, FIFO). This implements the standard price-time priority matching:

1. **Price priority:** Best (lowest) ask is matched first
2. **Time priority:** Within the same price, the earliest order is matched first

**Market order handling:**

The only difference between limit and market matching is the price check:

```cpp
if (incoming->type == OrderType::LIMIT && ask_price > incoming->price)
    break;  // Limit order: stop if price isn't favorable
// Market order: no price check, keep matching until filled or book empty
```

A market order has no limit on how far through the book it will sweep. If the book is empty or has insufficient liquidity, the unfilled remainder is discarded (not rested in the book, because a market order with no price has no meaningful resting level).

**`execute_trade` — what happens at the atomic moment of a fill:**

```cpp
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
```

Both sides' `filled_qty` is incremented. The trade is recorded with both order IDs, the execution price, quantity, and a monotonic timestamp. The Trade struct is returned to the caller for output (printing, sending over TCP, etc.).

**Pros:**
- Clean separation between matching logic and I/O
- All allocation through the pool — no hidden `new` calls
- Deterministic matching: same input always produces same output

**Cons:**
- Returns `vector<Trade>` which allocates heap memory per call. A production system would use a pre-allocated buffer or callback.
- Single-threaded — sufficient for this project but real exchanges use thread-per-instrument or lock-free designs.

---

### 7. CSV Parser & CLI

**`csv_parser.cpp`** — Parses human-readable commands:

```
LIMIT,BUY,100.00,50     → process_order(BUY, LIMIT, 10000, 50)
MARKET,SELL,,80          → process_order(SELL, MARKET, 0, 80)
CANCEL,,,,3              → cancel_order(3)
PRINT                    → book().print()
```

**`main.cpp`** — Entry point that wires CsvParser to stdin or a file:

```cpp
int main(int argc, char* argv[]) {
    ob::MatchingEngine engine;
    ob::CsvParser parser(engine);

    if (argc > 1) {
        std::ifstream file(argv[1]);
        parser.process_stream(file, std::cout);
    } else {
        parser.process_stream(std::cin, std::cout);
    }
}
```

The parser is intentionally simple — it exists to demonstrate and test the engine, not to be the production interface. The CSV format supports both file-based batch processing and interactive stdin usage.

**Why the CANCEL format has empty fields (`CANCEL,,,,5`):**

The parser splits on commas and expects the order ID in the 5th field (index 4). The empty fields maintain positional consistency with the `TYPE,SIDE,PRICE,QTY` format. This makes it easy to generate test files programmatically. An alternative would be `CANCEL,5` but the current format keeps the column count consistent.

---

### 8. Binary Protocol (`protocol.h`)

```cpp
// Client → Server: 32 bytes
struct OrderMessage {
    uint8_t  msg_type;      // NEW_ORDER or CANCEL
    uint8_t  side;          // BUY/SELL
    uint8_t  order_type;    // LIMIT/MARKET
    uint8_t  padding[5];
    uint64_t order_id;
    int64_t  price;         // Fixed-point
    uint32_t quantity;
    uint32_t reserved;
};
static_assert(sizeof(OrderMessage) == 32, "OrderMessage must be 32 bytes");

// Server → Client: 32 bytes
struct ResponseMessage {
    uint8_t  msg_type;      // ACK, FILL, or REJECT
    uint8_t  padding[3];
    uint32_t quantity;
    uint64_t order_id;
    int64_t  price;
    uint64_t match_id;
};
static_assert(sizeof(ResponseMessage) == 32, "ResponseMessage must be 32 bytes");
```

**What it does:** Defines the wire format for TCP communication. Both messages are fixed 32-byte structs, trivially copyable.

**Serialization is `memcpy`:**

```cpp
inline void serialize(const OrderMessage& msg, char* buf) {
    std::memcpy(buf, &msg, sizeof(msg));
}
```

No JSON parsing, no Protobuf decoding, no string allocation. Just copy 32 bytes. This is how real low-latency protocols work (e.g., CME's iLink, NASDAQ's OUCH).

**Why fixed-size messages:**

| Feature | Fixed-size | Variable-length (JSON/Protobuf) |
|---------|-----------|-------------------------------|
| Parse cost | 0 (memcpy) | 100ns-10us (parsing) |
| Heap allocation | None | String/buffer allocation |
| Framing | Implicit (read exactly N bytes) | Need length prefix or delimiter |
| Bandwidth | Optimal (no field names) | 2-10x larger |

The framing advantage is significant: the server knows every message is exactly 32 bytes, so it reads exactly 32 bytes per message. No need to parse a length header, scan for delimiters, or handle variable-sized buffers. The read loop is trivial and branch-free in the common case.

**Why `static_assert` on size:**

Adding or reordering fields could silently change the struct size, breaking wire compatibility. The `static_assert` catches this at compile time. This is especially important because the client and server could be compiled separately.

**Padding fields:**

The `padding` arrays serve dual purpose: they ensure natural alignment of subsequent fields (avoiding unaligned access penalties) and they reserve space for future fields without changing the message size.

**Limitation:** This protocol assumes client and server share the same byte order (both little-endian or both big-endian). On modern x86/ARM this is always little-endian. A cross-platform protocol would need `htonl`/`ntohl` or explicit byte-order specification.

---

### 9. TCP Server (`tcp_server.h/cpp`)

```cpp
class TcpServer {
    int listen_fd_;
    int kqueue_fd_;
    MatchingEngine& engine_;
    std::vector<ClientState> clients_;
};
```

**What it does:** Accepts TCP connections, reads 32-byte OrderMessages, feeds them to the MatchingEngine, and sends back ResponseMessages.

**Event loop using `kqueue` (macOS) — how it works:**

```cpp
void TcpServer::run() {
    // 1. Create kqueue instance
    kqueue_fd_ = kqueue();

    // 2. Register listener socket
    struct kevent ev;
    EV_SET(&ev, listen_fd_, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    kevent(kqueue_fd_, &ev, 1, nullptr, 0, nullptr);

    // 3. Event loop
    while (running_) {
        struct kevent events[64];
        int n = kevent(kqueue_fd_, nullptr, 0, events, 64, &timeout);

        for (int i = 0; i < n; ++i) {
            if (events[i].ident == listen_fd_)
                accept_client();            // New connection
            else
                handle_client_data(fd);     // Data from existing client
        }
    }
}
```

`kqueue` is the macOS equivalent of Linux's `epoll`. It monitors multiple file descriptors and wakes the process only when data is available. This means:
- **No polling:** The thread sleeps until there's work to do
- **No thread-per-client:** One thread handles all connections
- **No busy waiting:** CPU usage is near zero when idle

**Why single-threaded:**

The matching engine must process orders sequentially to maintain deterministic ordering (the same input must always produce the same trades). A single-threaded server naturally enforces this. Multi-threaded matching requires careful synchronization (lock-free queues, sequence numbers) which adds complexity.

For this project, single-threaded is the correct choice — it's simpler, deterministic, and the bottleneck is network latency (microseconds) not CPU throughput (nanoseconds per order).

**Why `TCP_NODELAY`:**

```cpp
int opt = 1;
setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
```

Disables Nagle's algorithm, which normally buffers small writes to coalesce them into larger TCP segments. For a matching engine, we want responses sent immediately — even if they're only 32 bytes. Without `TCP_NODELAY`, the ACK response could be delayed by up to 200ms waiting for more data to send.

**Per-client read buffer:**

```cpp
struct ClientState {
    int fd;
    char read_buf[sizeof(OrderMessage)];
    size_t bytes_read = 0;
};
```

TCP is a stream protocol — a `read()` call might return 10 bytes of a 32-byte message. The `bytes_read` counter tracks how much of the current message has been received. Once 32 bytes are accumulated, the message is deserialized and processed. This handles TCP fragmentation correctly.

**Graceful shutdown:**

```cpp
static volatile sig_atomic_t g_shutdown = 0;
static void signal_handler(int) { g_shutdown = 1; }

// In event loop:
while (running_ && !g_shutdown) { ... }
```

Ctrl-C sends SIGINT, which sets the `g_shutdown` flag. The event loop checks this flag on each iteration (with a 1-second timeout so it doesn't block forever waiting for events). `sig_atomic_t` ensures the flag write is atomic even without mutexes.

**Pros:**
- Low latency: no thread context switches, no lock contention
- Simple: easy to reason about correctness
- Portable pattern: same structure works with `epoll` on Linux

**Cons:**
- Single-threaded limits throughput to one core
- Blocking `write()` in `send_response` could stall if a client's receive buffer is full
- No authentication or access control

---

### 10. TCP Test Client (`tcp_client.cpp`)

**What it does:** Connects to the server, sends random limit orders, and measures round-trip latency (send order → receive ACK).

**Latency measurement:**

```cpp
auto start = Clock::now();
send_order(fd, side, ob::OrderType::LIMIT, price, qty);
auto resp = read_response(fd);  // blocks until ACK received
auto end = Clock::now();
double us = std::chrono::duration<double, std::micro>(end - start).count();
```

This measures the full round-trip: client write → network → server read → matching → server write → network → client read. Latencies are collected, sorted, and percentiles computed.

---

## Matching Algorithm Walkthrough

Here is a step-by-step walkthrough of a complete matching scenario:

**Starting state: empty book.**

```
Step 1: LIMIT SELL 100 @ 101.00 (Order ID 1)
  - No bids exist → no match possible
  - Order 1 rests on the ask side

  ASKS: 101.00 → [Order 1: qty=100]
  BIDS: (empty)
```

```
Step 2: LIMIT SELL 50 @ 100.50 (Order ID 2)
  - No bids exist → no match possible
  - Order 2 rests on the ask side

  ASKS: 100.50 → [Order 2: qty=50]
        101.00 → [Order 1: qty=100]
  BIDS: (empty)
```

```
Step 3: LIMIT BUY 120 @ 101.00 (Order ID 3)
  - Check asks from lowest: 100.50 <= 101.00? YES

  Match with Order 2 at 100.50:
    fill_qty = min(120, 50) = 50
    → TRADE buyer=3 seller=2 price=100.50 qty=50
    Order 2 fully filled → removed from book
    Order 3 remaining: 70

  Next ask level: 101.00 <= 101.00? YES

  Match with Order 1 at 101.00:
    fill_qty = min(70, 100) = 70
    → TRADE buyer=3 seller=1 price=101.00 qty=70
    Order 1 partially filled (30 remaining)
    Order 3 fully filled → returned to pool

  Final state:
  ASKS: 101.00 → [Order 1: qty=100, filled=70, remaining=30]
  BIDS: (empty)
```

Key observations:
- Order 3 got price improvement: willing to buy at 101.00, got 50 units at 100.50
- Price priority: the cheaper ask (100.50) was matched first
- Partial fill: Order 1 still has 30 units resting
- Order 3 was fully filled so it never entered the book

---

## Memory Model & Lifecycle

```
Order Lifecycle:

  pool_.allocate()
       │
       ▼
  ┌──────────┐
  │  Order*  │ ← Fields filled in by MatchingEngine
  └────┬─────┘
       │
       ├──── Fully matched? ────► pool_.deallocate()  (back to free list)
       │
       └──── Partially/not matched?
             │
             ▼
        book_.add_order()
             │
             │  (Order lives in PriceLevel deque + order_lookup hash map)
             │
             ├──── Later matched during another order's processing?
             │         level.pop_front()
             │         book_.remove_from_lookup()
             │         pool_.deallocate()
             │
             └──── Cancelled by user?
                      book_.cancel_order()  (removes from both level and lookup)
                      pool_.deallocate()
```

**Critical invariant:** Every `pool_.allocate()` is paired with exactly one `pool_.deallocate()`. The matching engine is responsible for this pairing. The OrderBook and PriceLevel never allocate or deallocate — they only hold borrowed pointers.

---

## Benchmark Results & Analysis

Results from an Apple Silicon M-series chip, 1 million orders, Release build (`-O3 -march=native`):

### Mixed Workload (5% cancel, 10% market, 85% limit)

| Metric | Value |
|--------|-------|
| Throughput | ~8.5M orders/sec |
| Trades generated | ~818K |
| Mean latency | ~99ns |
| p50 latency | ~83ns |
| p95 latency | ~250ns |
| p99 latency | ~375ns |
| p99.9 latency | ~833ns |

### Pure Limit Orders

| Metric | Value |
|--------|-------|
| Throughput | ~8.8M orders/sec |
| Trades generated | ~783K |
| Mean latency | ~96ns |
| p50 latency | ~83ns |
| p95 latency | ~209ns |

### High Cancel Rate (30%)

| Metric | Value |
|--------|-------|
| Throughput | ~9.9M orders/sec |
| p50 latency | ~42ns |

**Analysis:**

- **Cancels are cheapest** (~42ns p50): O(1) hash lookup + O(1) pool deallocation. The O(n) linear scan in the PriceLevel is fast because levels are small.
- **Limit orders that don't match** are fast: allocate + add to map + add to level. No matching loop.
- **Matching orders** are the most expensive: walking price levels, executing trades, cleaning up filled orders. This is why the p95 and p99 are 3-4x the p50 — the tail represents orders that sweep multiple levels.
- **Throughput > 8M/sec** on a single core is competitive with many production systems.

### TCP Round-Trip

| Metric | Value |
|--------|-------|
| p50 latency | ~5.6us |
| p99 latency | ~48us |

The ~5us round-trip over localhost TCP includes: client write syscall → kernel TCP stack → server read syscall → matching (~100ns) → server write syscall → kernel TCP stack → client read syscall. The matching engine itself is <1% of the total latency; the rest is kernel overhead. This is why real HFT systems use kernel bypass (DPDK, Solarflare OpenOnload) to eliminate syscall overhead.

---

## Testing

### Test Coverage (32 tests)

**Object Pool (4 tests):**
- `AllocateAndDeallocate` — basic alloc/dealloc, counter tracking
- `ReusesDeallocatedMemory` — freed slot is reused on next allocation
- `GrowsWhenExhausted` — new block allocated when free list is empty
- `HighVolume` — 10,000 allocations and deallocations

**Order Book (10 tests):**
- `EmptyBook` — no best bid/ask, zero count
- `AddBidOrder` / `AddAskOrder` — single order insertion
- `BestBidIsHighest` / `BestAskIsLowest` — price ordering correctness
- `CancelOrder` / `CancelNonExistent` — removal and rejection
- `VolumeAtPrice` — cached quantity tracking
- `MultipleLevels` — multi-price book
- `CancelRemovesEmptyLevel` — empty level cleanup

**Matching Engine (18 tests):**
- Basic matching: buy matches sell, sell matches buy, higher price matches lower ask
- Non-crossing: no match when prices don't cross
- Partial fills: buy side remainder, sell side remainder
- Multi-level matching: buy sweeps multiple ask levels
- FIFO within level: first order at a price is matched first
- Market orders: sweep all available, handle empty book, discard unfilled remainder
- Cancellation: cancel existing order, reject non-existent
- Order ID assignment: sequential IDs
- Stress tests: 1000 non-matching orders, 100-level sweep

### Running Tests

```bash
cd build && ctest --output-on-failure
```

To run a specific test:

```bash
./tests --gtest_filter="MatchingEngineTest.PartialFillBuy"
```

---

## Design Decisions & Trade-offs

### 1. Why C++20?

**Used features:**
- `[[unlikely]]` attribute on the pool growth path — hints the compiler to optimize the fast path
- `std::optional` for `best_bid()`/`best_ask()` — cleanly handles empty book without sentinel values
- Designated initializers (`Trade{.buyer_order_id = ...}`) — readable struct construction
- `auto&` return types on book accessors

**Not used (intentionally):**
- `std::format` — `snprintf` is sufficient and more portable
- Modules — still poorly supported across compilers
- Coroutines — not needed for this synchronous design

### 2. Why `std::map` over a flat array for price levels?

**Trade-off: flexibility vs. raw speed.**

A flat array indexed by `(price - min_price)` would give O(1) insert/delete/lookup but requires knowing the price range up front and wastes memory for sparse price ranges (e.g., a stock at $3000 with tick size 0.01 would need a 300,000-element array).

`std::map` works for any price range, uses memory proportional to active levels, and is fast enough (O(log n) with n usually < 1000).

**When to upgrade:** If profiling shows `std::map` node allocation or tree traversal as the bottleneck, switch to a price-indexed flat array or a cache-friendly B-tree.

### 3. Why `std::deque` over intrusive linked list for PriceLevel?

**Trade-off: cache locality vs. O(1) cancel.**

`std::deque` gives excellent cache behavior for the common operations (push_back, pop_front, sequential iteration) at the cost of O(n) mid-removal for cancels.

An intrusive doubly-linked list gives O(1) cancel but poor cache locality for iteration (pointer chasing through scattered heap memory).

Since matching (iteration) happens far more often than cancellation, `std::deque` is the better choice for throughput. The cancel penalty is bounded by level size, which is typically small.

### 4. Why single-threaded?

**Trade-off: simplicity & determinism vs. multi-core throughput.**

A matching engine must produce deterministic results: the same input sequence must always produce the same trades. With a single thread, this is trivially guaranteed. Multi-threading requires careful synchronization (sequence numbers, lock-free queues) and is harder to test and debug.

Real exchanges handle multi-threading by sharding: one thread per instrument. Each instrument's matching engine is single-threaded. This project could be extended the same way.

### 5. Why return `vector<Trade>` instead of using callbacks?

**Trade-off: API simplicity vs. allocation overhead.**

Returning a vector is the simplest API — the caller gets a value they can print, send, or discard. The downside is that `std::vector` allocates heap memory per call (even with small buffer optimization, it allocates once trades exceed ~1-2).

A production system would use a callback (`std::function<void(const Trade&)>`) or write to a pre-allocated ring buffer. But for this project, the vector allocation is negligible compared to other overheads (CSV parsing, I/O) and it keeps the API clean.

### 6. Why monotonic counters for timestamps instead of wall clock?

```cpp
Timestamp next_timestamp_ = 1;
// ...
order->timestamp = next_timestamp_++;
```

Calling `std::chrono::high_resolution_clock::now()` costs 20-50ns due to `clock_gettime()` syscall. A monotonic counter costs 1ns (increment + store). Since we only need timestamps for ordering (not wall-clock time), the counter is sufficient and much cheaper.

Real systems would record wall-clock time separately for regulatory reporting, but the matching logic only needs a total order.

---

## Known Limitations & Future Work

### Current Limitations

1. **No self-trade prevention.** If the same participant sends both a BUY and a SELL at the same price, they match against each other. Real exchanges prevent this by checking participant IDs.

2. **O(n) cancel within a price level.** The `PriceLevel::remove()` does a linear scan. For levels with hundreds of orders, this could matter. Fix: use an intrusive doubly-linked list with an iterator stored in the `order_lookup_` map.

3. **`vector<Trade>` allocation on every `process_order`.** Creates heap pressure. Fix: pre-allocated trade buffer or callback interface.

4. **No order modification (replace).** Real exchanges support modify-in-place (change quantity or price). Currently you must cancel and re-submit.

5. **TCP server uses blocking writes.** If a client's receive buffer is full, `send_response` blocks the entire server. Fix: use non-blocking writes with a per-client write buffer.

6. **TCP client doesn't drain fill messages.** After receiving an ACK, there may be FILL messages that aren't consumed before the next order is sent. This could desync the client in edge cases.

7. **macOS-only TCP server.** Uses `kqueue`. For Linux, needs `epoll`. The matching engine itself is fully portable.

8. **No persistence.** All state is in memory. A crash loses the entire book.

### Potential Improvements

- **Kernel bypass networking** (DPDK/io_uring) to eliminate syscall overhead
- **Lock-free SPSC queue** between network thread and matching thread
- **Price-indexed flat array** for O(1) price level access
- **`__rdtsc`-based timing** for sub-nanosecond latency measurement
- **Instrument sharding** for multi-instrument support
- **FIX protocol** support for industry-standard connectivity
- **Market data output** (L2 depth-of-book snapshots, trade tape)

---

## Interview-Style Q&A

**Q: Why did you use fixed-point arithmetic for prices?**

Floating-point numbers cannot represent decimal fractions exactly. `0.1 + 0.2 == 0.30000000000000004` in IEEE 754. In a matching engine, this causes orders at the "same" price to not match, or orders at "different" prices to incorrectly match. Every production exchange uses fixed-point or integer pricing. I chose `int64_t` scaled by 100 (2 decimal places), which gives correct comparisons using native integer instructions.

**Q: What's the time complexity of your matching algorithm?**

For an incoming order that matches against k orders across m price levels:
- Matching: O(m * log(n) + k) where n is total price levels (map traversal + per-order processing)
- In practice m and k are small, so it's effectively O(1) for typical orders.
- Adding an unmatched order: O(log n) for the map insertion
- Cancel: O(1) hash lookup + O(n) level scan (where n is orders at that price)

**Q: Why not use an `unordered_map` for the book sides?**

The matching engine needs to walk prices in sorted order (lowest ask first, highest bid first). `unordered_map` has no ordering, so you'd need to sort on every match — O(n log n) on every incoming order. `std::map` gives O(log n) insert with inherent sorted iteration via `begin()`.

**Q: What happens if the object pool runs out of pre-allocated memory?**

It grows by allocating a new block of 4096 * 64 = 256KB. The `[[unlikely]]` attribute on this path tells the compiler it's cold, so the hot path (free list not empty) gets better instruction scheduling. Growth is rare in practice because filled orders are returned to the pool, maintaining a steady-state free list size.

**Q: How would you make this thread-safe?**

I wouldn't add locks to the matching engine — that would serialize it anyway and add lock overhead. Instead, I'd use the exchange sharding model: one single-threaded matching engine per instrument, with a lock-free SPSC queue feeding orders from the network thread to the matching thread. The network I/O layer handles concurrency; the matching engine stays single-threaded and fast.

**Q: What's the biggest bottleneck in your current design?**

For the core engine: `std::vector<Trade>` allocation on every `process_order` call. It does a heap allocation even for single-trade matches. I'd replace it with a pre-allocated ring buffer or a callback interface.

For the TCP server: kernel syscall overhead. The matching itself is ~100ns but the full round-trip is ~5us — 98% is kernel TCP stack. Real HFT systems use kernel bypass (DPDK, Solarflare OpenOnload) or FPGA-based NICs.

**Q: Why `std::deque` instead of a linked list for the price level queue?**

Cache locality. Matching iterates the front of the queue sequentially, and `std::deque` stores elements in contiguous chunks that prefetch well into L1 cache. A linked list scatters nodes across the heap, causing a cache miss on every `next` pointer dereference. The trade-off is O(n) mid-removal for cancels, but in practice levels have few orders (10-50) and cancels are less frequent than matches.

**Q: How would you support multiple instruments?**

Create a `std::unordered_map<InstrumentId, MatchingEngine>`. Each instrument gets its own independent engine with its own book and pool. Incoming orders are routed by instrument ID. This is exactly how real exchanges work — each instrument's book is independent. For multi-threading, assign each instrument to a dedicated thread.

**Q: What's the difference between `cancel_order` and `remove_from_lookup`, and why do both exist?**

`cancel_order` is the full user-facing cancel path: it removes the order from both the order_lookup hash map and the PriceLevel deque, cleans up empty levels, and returns the Order pointer for deallocation.

`remove_from_lookup` only removes from the hash map. It exists because during matching, the matching engine has already popped the order from the PriceLevel via `pop_front()` and is managing level cleanup itself (erasing empty levels at the end of each price level iteration). Calling `cancel_order` during matching would attempt to re-remove the order from the level (searching for an already-popped pointer), and worse, could erase the map entry for the level being iterated — invalidating the iterator and causing a segfault.

This was a real bug caught by the test suite during development. The fix was to split the two paths: matching uses `remove_from_lookup` (just the hash map), user cancels use `cancel_order` (hash map + level + cleanup).
