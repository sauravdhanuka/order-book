#pragma once

#include "matching_engine.h"
#include "protocol.h"
#include "platform.h"
#include <cstdint>
#include <vector>
#include <string>
#include <functional>

namespace ob {

class TcpServer {
public:
    TcpServer(uint16_t port, MatchingEngine& engine);
    ~TcpServer();

    // Run the event loop (blocks until shutdown)
    void run();

    // Signal the server to stop
    void shutdown();

private:
    uint16_t port_;
    MatchingEngine& engine_;
    socket_t listen_fd_ = INVALID_SOCK;
    bool running_ = false;

    // Per-client read buffer (handles partial reads)
    struct ClientState {
        socket_t fd;
        char read_buf[sizeof(OrderMessage)];
        size_t bytes_read = 0;
    };

    std::vector<ClientState> clients_;

    void setup_listener();
    void accept_client();
    void handle_client_data(socket_t client_fd);
    void remove_client(socket_t client_fd);
    void process_message(socket_t client_fd, const OrderMessage& msg);
    void send_response(socket_t client_fd, const ResponseMessage& resp);
};

} // namespace ob
