#include "tcp_server.h"
#include "platform.h"

#include <cstring>
#include <iostream>
#include <algorithm>

namespace ob {

TcpServer::TcpServer(uint16_t port, MatchingEngine& engine)
    : port_(port), engine_(engine) {}

TcpServer::~TcpServer() {
    for (auto& c : clients_) platform_close(c.fd);
    if (listen_fd_ != INVALID_SOCK) platform_close(listen_fd_);
}

void TcpServer::setup_listener() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ == INVALID_SOCK) {
        std::cerr << "Error: socket() failed\n";
        return;
    }

    platform_set_reuseaddr(listen_fd_);
    platform_set_nodelay(listen_fd_);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) != 0) {
        std::cerr << "Error: bind() failed\n";
        platform_close(listen_fd_);
        listen_fd_ = INVALID_SOCK;
        return;
    }

    if (listen(listen_fd_, 128) != 0) {
        std::cerr << "Error: listen() failed\n";
        platform_close(listen_fd_);
        listen_fd_ = INVALID_SOCK;
        return;
    }

    platform_set_nonblocking(listen_fd_);
}

void TcpServer::accept_client() {
    sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);
    socket_t client_fd = accept(listen_fd_, (sockaddr*)&client_addr, &len);
    if (client_fd == INVALID_SOCK) return;

    platform_set_nonblocking(client_fd);
    platform_set_nodelay(client_fd);

    ClientState cs;
    cs.fd = client_fd;
    cs.bytes_read = 0;
    clients_.push_back(cs);

    std::cout << "Client connected (fd=" << client_fd << ")\n";
}

void TcpServer::remove_client(socket_t client_fd) {
    platform_close(client_fd);
    clients_.erase(
        std::remove_if(clients_.begin(), clients_.end(),
            [client_fd](const ClientState& c) { return c.fd == client_fd; }),
        clients_.end());

    std::cout << "Client disconnected (fd=" << client_fd << ")\n";
}

void TcpServer::handle_client_data(socket_t client_fd) {
    auto it = std::find_if(clients_.begin(), clients_.end(),
        [client_fd](const ClientState& c) { return c.fd == client_fd; });

    if (it == clients_.end()) return;
    ClientState& client = *it;

    constexpr size_t MSG_SIZE = sizeof(OrderMessage);

    while (true) {
        ssize_t n = platform_recv(client_fd, client.read_buf + client.bytes_read,
                                  MSG_SIZE - client.bytes_read);
        if (n <= 0) {
            if (n == 0 || !platform_would_block()) {
                remove_client(client_fd);
            }
            return;
        }

        client.bytes_read += static_cast<size_t>(n);

        if (client.bytes_read == MSG_SIZE) {
            OrderMessage msg;
            deserialize(client.read_buf, msg);
            process_message(client_fd, msg);
            client.bytes_read = 0;
        }
    }
}

void TcpServer::process_message(socket_t client_fd, const OrderMessage& msg) {
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

void TcpServer::send_response(socket_t client_fd, const ResponseMessage& resp) {
    char buf[sizeof(ResponseMessage)];
    serialize(resp, buf);

    size_t total = 0;
    while (total < sizeof(buf)) {
        ssize_t n = platform_send(client_fd, buf + total, sizeof(buf) - total);
        if (n <= 0) return;
        total += static_cast<size_t>(n);
    }
}

void TcpServer::run() {
    platform_install_signal_handler();

    setup_listener();
    if (listen_fd_ == INVALID_SOCK) {
        std::cerr << "Failed to set up listener\n";
        return;
    }

    running_ = true;
    std::cout << "Order book server listening on port " << port_ << "\n";

    // Use select() for cross-platform I/O multiplexing
    while (running_ && !platform_shutdown_requested()) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_fd_, &read_fds);

        // Track the max fd for select() (ignored on Windows but needed on POSIX)
        socket_t max_fd = listen_fd_;

        for (const auto& c : clients_) {
            FD_SET(c.fd, &read_fds);
#ifndef _WIN32
            if (c.fd > max_fd) max_fd = c.fd;
#endif
        }

        // 1-second timeout so we can check the shutdown flag periodically
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int ready = select(static_cast<int>(max_fd + 1), &read_fds, nullptr, nullptr, &timeout);
        if (ready <= 0) continue;

        // Check for new connections
        if (FD_ISSET(listen_fd_, &read_fds)) {
            accept_client();
        }

        // Check existing clients for data
        // Copy client list since handle_client_data may remove entries
        std::vector<socket_t> client_fds;
        client_fds.reserve(clients_.size());
        for (const auto& c : clients_) {
            client_fds.push_back(c.fd);
        }

        for (socket_t fd : client_fds) {
            if (FD_ISSET(fd, &read_fds)) {
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

} // namespace ob

int main(int argc, char* argv[]) {
    if (platform_init() != 0) {
        std::cerr << "Failed to initialize network stack\n";
        return 1;
    }

    uint16_t port = 9000;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

    ob::MatchingEngine engine;
    ob::TcpServer server(port, engine);
    server.run();

    platform_cleanup();
    return 0;
}
