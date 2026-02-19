#include "protocol.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>
#include <random>
#include <iomanip>

using Clock = std::chrono::high_resolution_clock;

int connect_to_server(const char* host, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

void send_order(int fd, ob::Side side, ob::OrderType type, int64_t price, uint32_t qty) {
    ob::OrderMessage msg{};
    msg.msg_type = static_cast<uint8_t>(ob::MsgType::NEW_ORDER);
    msg.side = static_cast<uint8_t>(side);
    msg.order_type = static_cast<uint8_t>(type);
    msg.price = price;
    msg.quantity = qty;

    char buf[sizeof(ob::OrderMessage)];
    ob::serialize(msg, buf);
    write(fd, buf, sizeof(buf));
}

ob::ResponseMessage read_response(int fd) {
    char buf[sizeof(ob::ResponseMessage)];
    size_t total = 0;
    while (total < sizeof(buf)) {
        ssize_t n = read(fd, buf + total, sizeof(buf) - total);
        if (n <= 0) break;
        total += n;
    }
    ob::ResponseMessage resp{};
    ob::deserialize(buf, resp);
    return resp;
}

int main(int argc, char* argv[]) {
    const char* host = "127.0.0.1";
    uint16_t port = 9000;
    int num_orders = 10000;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) host = argv[++i];
        else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = std::stoi(argv[++i]);
        else if (std::strcmp(argv[i], "--orders") == 0 && i + 1 < argc) num_orders = std::stoi(argv[++i]);
    }

    std::cout << "Connecting to " << host << ":" << port << "...\n";
    int fd = connect_to_server(host, port);
    if (fd < 0) return 1;

    std::cout << "Connected. Sending " << num_orders << " orders...\n";

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> price_dist(9900, 10100);
    std::uniform_int_distribution<uint32_t> qty_dist(1, 100);

    std::vector<double> latencies;
    latencies.reserve(num_orders);

    for (int i = 0; i < num_orders; ++i) {
        ob::Side side = side_dist(rng) ? ob::Side::BUY : ob::Side::SELL;
        int64_t price = price_dist(rng);
        uint32_t qty = qty_dist(rng);

        auto start = Clock::now();
        send_order(fd, side, ob::OrderType::LIMIT, price, qty);
        auto resp = read_response(fd);
        auto end = Clock::now();

        double us = std::chrono::duration<double, std::micro>(end - start).count();
        latencies.push_back(us);

        // Drain any fill messages
        if (static_cast<ob::MsgType>(resp.msg_type) == ob::MsgType::ACK) {
            // There may be fill messages following, but for simplicity
            // we only measure the ACK round-trip
        }
    }

    close(fd);

    std::sort(latencies.begin(), latencies.end());
    size_t n = latencies.size();

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "\n=== Round-trip Latency (us) ===\n";
    std::cout << "  mean:  " << std::accumulate(latencies.begin(), latencies.end(), 0.0) / n << "\n";
    std::cout << "  p50:   " << latencies[n * 50 / 100] << "\n";
    std::cout << "  p95:   " << latencies[n * 95 / 100] << "\n";
    std::cout << "  p99:   " << latencies[n * 99 / 100] << "\n";
    std::cout << "  p99.9: " << latencies[std::min(n - 1, n * 999 / 1000)] << "\n";

    return 0;
}
