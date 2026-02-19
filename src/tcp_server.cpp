#include "tcp_server.h"

#include <sys/socket.h>
#include <sys/event.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <algorithm>

namespace ob {

static volatile sig_atomic_t g_shutdown = 0;
static void signal_handler(int) { g_shutdown = 1; }

TcpServer::TcpServer(uint16_t port, MatchingEngine& engine)
    : port_(port), engine_(engine) {}

TcpServer::~TcpServer() {
    if (kqueue_fd_ >= 0) close(kqueue_fd_);
    for (auto& c : clients_) close(c.fd);
    if (listen_fd_ >= 0) close(listen_fd_);
}

void TcpServer::set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void TcpServer::setup_listener() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        perror("socket");
        return;
    }

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Disable Nagle's algorithm for lower latency
    setsockopt(listen_fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    if (listen(listen_fd_, 128) < 0) {
        perror("listen");
        close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    set_nonblocking(listen_fd_);
}

void TcpServer::accept_client() {
    sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);
    int client_fd = accept(listen_fd_, (sockaddr*)&client_addr, &len);
    if (client_fd < 0) return;

    set_nonblocking(client_fd);

    int opt = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    // Register with kqueue
    struct kevent ev;
    EV_SET(&ev, client_fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    kevent(kqueue_fd_, &ev, 1, nullptr, 0, nullptr);

    ClientState cs;
    cs.fd = client_fd;
    cs.bytes_read = 0;
    clients_.push_back(cs);

    std::cout << "Client connected (fd=" << client_fd << ")\n";
}

void TcpServer::remove_client(int client_fd) {
    struct kevent ev;
    EV_SET(&ev, client_fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    kevent(kqueue_fd_, &ev, 1, nullptr, 0, nullptr);

    close(client_fd);
    clients_.erase(
        std::remove_if(clients_.begin(), clients_.end(),
            [client_fd](const ClientState& c) { return c.fd == client_fd; }),
        clients_.end());

    std::cout << "Client disconnected (fd=" << client_fd << ")\n";
}

void TcpServer::handle_client_data(int client_fd) {
    auto it = std::find_if(clients_.begin(), clients_.end(),
        [client_fd](const ClientState& c) { return c.fd == client_fd; });

    if (it == clients_.end()) return;
    ClientState& client = *it;

    constexpr size_t MSG_SIZE = sizeof(OrderMessage);

    while (true) {
        ssize_t n = read(client_fd, client.read_buf + client.bytes_read,
                         MSG_SIZE - client.bytes_read);
        if (n <= 0) {
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                remove_client(client_fd);
            }
            return;
        }

        client.bytes_read += n;

        if (client.bytes_read == MSG_SIZE) {
            OrderMessage msg;
            deserialize(client.read_buf, msg);
            process_message(client_fd, msg);
            client.bytes_read = 0;
        }
    }
}

void TcpServer::process_message(int client_fd, const OrderMessage& msg) {
    MsgType type = static_cast<MsgType>(msg.msg_type);

    if (type == MsgType::CANCEL) {
        bool ok = engine_.cancel_order(msg.order_id);
        ResponseMessage resp{};
        resp.msg_type = ok ? static_cast<uint8_t>(MsgType::ACK)
                           : static_cast<uint8_t>(MsgType::REJECT);
        resp.order_id = msg.order_id;
        send_response(client_fd, resp);
        return;
    }

    if (type == MsgType::NEW_ORDER) {
        Side side = static_cast<Side>(msg.side);
        OrderType otype = static_cast<OrderType>(msg.order_type);

        auto trades = engine_.process_order(side, otype, msg.price, msg.quantity);

        // Send ACK with assigned order ID
        ResponseMessage ack{};
        ack.msg_type = static_cast<uint8_t>(MsgType::ACK);
        ack.order_id = engine_.next_order_id() - 1;
        send_response(client_fd, ack);

        // Send FILL messages for each trade
        for (const auto& trade : trades) {
            ResponseMessage fill{};
            fill.msg_type = static_cast<uint8_t>(MsgType::FILL);
            fill.order_id = ack.order_id;
            fill.price = trade.price;
            fill.quantity = trade.quantity;
            fill.match_id = (side == Side::BUY) ? trade.seller_order_id : trade.buyer_order_id;
            send_response(client_fd, fill);
        }
        return;
    }

    // Unknown message type
    ResponseMessage reject{};
    reject.msg_type = static_cast<uint8_t>(MsgType::REJECT);
    send_response(client_fd, reject);
}

void TcpServer::send_response(int client_fd, const ResponseMessage& resp) {
    char buf[sizeof(ResponseMessage)];
    serialize(resp, buf);

    // For simplicity, blocking write (messages are small)
    size_t total = 0;
    while (total < sizeof(buf)) {
        ssize_t n = write(client_fd, buf + total, sizeof(buf) - total);
        if (n <= 0) return;
        total += n;
    }
}

void TcpServer::run() {
    signal(SIGINT, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    setup_listener();
    if (listen_fd_ < 0) {
        std::cerr << "Failed to set up listener\n";
        return;
    }

    kqueue_fd_ = kqueue();
    if (kqueue_fd_ < 0) {
        perror("kqueue");
        return;
    }

    // Register listener fd
    struct kevent ev;
    EV_SET(&ev, listen_fd_, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    kevent(kqueue_fd_, &ev, 1, nullptr, 0, nullptr);

    running_ = true;
    std::cout << "Order book server listening on port " << port_ << "\n";

    struct kevent events[64];

    while (running_ && !g_shutdown) {
        struct timespec timeout = {1, 0}; // 1 second timeout for shutdown check
        int n = kevent(kqueue_fd_, nullptr, 0, events, 64, &timeout);

        for (int i = 0; i < n; ++i) {
            int fd = static_cast<int>(events[i].ident);

            if (events[i].flags & EV_EOF) {
                if (fd != listen_fd_) remove_client(fd);
                continue;
            }

            if (fd == listen_fd_) {
                accept_client();
            } else {
                handle_client_data(fd);
            }
        }
    }

    std::cout << "Server shutting down...\n";
    running_ = false;
}

void TcpServer::shutdown() {
    running_ = false;
}

// Entry point for the server binary
} // namespace ob

int main(int argc, char* argv[]) {
    uint16_t port = 9000;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

    ob::MatchingEngine engine;
    ob::TcpServer server(port, engine);
    server.run();

    return 0;
}
