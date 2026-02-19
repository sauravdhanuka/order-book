#pragma once

#include "matching_engine.h"
#include "protocol.h"
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
    int listen_fd_ = -1;
    int kqueue_fd_ = -1;
    bool running_ = false;

    // Per-client read buffer (handles partial reads)
    struct ClientState {
        int fd;
        char read_buf[sizeof(OrderMessage)];
        size_t bytes_read = 0;
    };

    std::vector<ClientState> clients_;

    void setup_listener();
    void accept_client();
    void handle_client_data(int client_fd);
    void remove_client(int client_fd);
    void process_message(int client_fd, const OrderMessage& msg);
    void send_response(int client_fd, const ResponseMessage& resp);
    void set_nonblocking(int fd);
};

} // namespace ob
