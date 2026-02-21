#include "matching_engine.h"
#include "order_generator.h"
#include <chrono>
#include <iostream>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cstring>
#include <iomanip>

using Clock = std::chrono::high_resolution_clock;

struct BenchResult {
    double throughput;  // orders/sec
    double p50_ns, p95_ns, p99_ns, p999_ns;
    double mean_ns;
    uint64_t total_trades;
};

BenchResult run_benchmark(const std::vector<ob::GeneratedOrder>& orders) {
    ob::MatchingEngine engine;

    std::vector<double> latencies;
    latencies.reserve(orders.size());

    auto total_start = Clock::now();

    for (const auto& order : orders) {
        auto start = Clock::now();

        if (order.is_cancel) {
            engine.cancel_order(order.cancel_id);
        } else {
            engine.process_order(order.side, order.type, order.price, order.quantity);
        }

        auto end = Clock::now();
        double ns = std::chrono::duration<double, std::nano>(end - start).count();
        latencies.push_back(ns);
    }

    auto total_end = Clock::now();
    double total_sec = std::chrono::duration<double>(total_end - total_start).count();

    std::sort(latencies.begin(), latencies.end());
    size_t n = latencies.size();

    BenchResult result;
    result.throughput = n / total_sec;
    result.mean_ns = std::accumulate(latencies.begin(), latencies.end(), 0.0) / n;
    result.p50_ns = latencies[n * 50 / 100];
    result.p95_ns = latencies[n * 95 / 100];
    result.p99_ns = latencies[n * 99 / 100];
    result.p999_ns = latencies[std::min(n - 1, n * 999 / 1000)];
    result.total_trades = engine.trade_count();

    return result;
}

void print_result(const char* label, const BenchResult& r, size_t order_count) {
    std::cout << "\n=== " << label << " ===\n";
    std::cout << "Orders:     " << order_count << "\n";
    std::cout << "Trades:     " << r.total_trades << "\n";
    std::cout << std::fixed << std::setprecision(0);
    std::cout << "Throughput: " << r.throughput << " orders/sec\n";
    std::cout << std::setprecision(1);
    std::cout << "Latency (ns):\n";
    std::cout << "  mean:  " << r.mean_ns << "\n";
    std::cout << "  p50:   " << r.p50_ns << "\n";
    std::cout << "  p95:   " << r.p95_ns << "\n";
    std::cout << "  p99:   " << r.p99_ns << "\n";
    std::cout << "  p99.9: " << r.p999_ns << "\n";
}

int main(int argc, char* argv[]) {
    size_t order_count = 1'000'000;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--orders") == 0 && i + 1 < argc) {
            order_count = std::stoull(argv[++i]);
        }
    }

    std::cout << "Generating " << order_count << " random orders...\n";
    ob::OrderGenerator gen;

    // Benchmark 1: Mixed workload (limit + market + cancel)
    auto mixed_orders = gen.generate(order_count, 5, 10);
    auto mixed_result = run_benchmark(mixed_orders);
    print_result("Mixed Workload (5% cancel, 10% market)", mixed_result, order_count);

    // Benchmark 2: Pure limit orders (stress the book)
    auto limit_orders = gen.generate(order_count, 0, 0);
    auto limit_result = run_benchmark(limit_orders);
    print_result("Pure Limit Orders", limit_result, order_count);

    // Benchmark 3: High cancel rate
    auto cancel_orders = gen.generate(order_count, 30, 5);
    auto cancel_result = run_benchmark(cancel_orders);
    print_result("High Cancel Rate (30%)", cancel_result, order_count);

    return 0;
}
