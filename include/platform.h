#pragma once

// Cross-platform socket and signal abstractions.
// Wraps Winsock2 (Windows) vs POSIX sockets (macOS/Linux) behind a common API.

#ifdef _WIN32
    // Windows — Winsock2
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>

    // Link against ws2_32.lib (also handled in CMakeLists.txt)
    #pragma comment(lib, "ws2_32.lib")

    using socket_t = SOCKET;
    constexpr socket_t INVALID_SOCK = INVALID_SOCKET;

    inline int platform_init() {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa);
    }

    inline void platform_cleanup() {
        WSACleanup();
    }

    inline int platform_close(socket_t fd) {
        return closesocket(fd);
    }

    inline ssize_t platform_recv(socket_t fd, char* buf, size_t len) {
        return recv(fd, buf, static_cast<int>(len), 0);
    }

    inline ssize_t platform_send(socket_t fd, const char* buf, size_t len) {
        return send(fd, buf, static_cast<int>(len), 0);
    }

    inline int platform_set_nonblocking(socket_t fd) {
        u_long mode = 1;
        return ioctlsocket(fd, FIONBIO, &mode);
    }

    inline int platform_last_error() {
        return WSAGetLastError();
    }

    inline bool platform_would_block() {
        int err = WSAGetLastError();
        return err == WSAEWOULDBLOCK;
    }

    inline void platform_set_nodelay(socket_t fd) {
        int opt = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&opt, sizeof(opt));
    }

    inline void platform_set_reuseaddr(socket_t fd) {
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    }

#else
    // POSIX (macOS, Linux)
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <cerrno>

    using socket_t = int;
    constexpr socket_t INVALID_SOCK = -1;

    inline int platform_init() { return 0; } // No-op on POSIX
    inline void platform_cleanup() {}         // No-op on POSIX

    inline int platform_close(socket_t fd) {
        return close(fd);
    }

    inline ssize_t platform_recv(socket_t fd, char* buf, size_t len) {
        return read(fd, buf, len);
    }

    inline ssize_t platform_send(socket_t fd, const char* buf, size_t len) {
        return write(fd, buf, len);
    }

    inline int platform_set_nonblocking(socket_t fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    inline int platform_last_error() {
        return errno;
    }

    inline bool platform_would_block() {
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }

    inline void platform_set_nodelay(socket_t fd) {
        int opt = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    }

    inline void platform_set_reuseaddr(socket_t fd) {
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }

#endif

// Portable signal handling for graceful shutdown
#include <csignal>

#ifdef _WIN32
    #include <windows.h>

    namespace ob::detail {
        inline volatile long g_shutdown_flag = 0;

        inline BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
            if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT) {
                InterlockedExchange(&g_shutdown_flag, 1);
                return TRUE;
            }
            return FALSE;
        }
    }

    inline void platform_install_signal_handler() {
        SetConsoleCtrlHandler(ob::detail::console_ctrl_handler, TRUE);
    }

    inline bool platform_shutdown_requested() {
        return InterlockedCompareExchange(&ob::detail::g_shutdown_flag, 0, 0) != 0;
    }

#else
    namespace ob::detail {
        inline volatile sig_atomic_t g_shutdown_flag = 0;

        inline void signal_handler(int) {
            g_shutdown_flag = 1;
        }
    }

    inline void platform_install_signal_handler() {
        signal(SIGINT, ob::detail::signal_handler);
        signal(SIGPIPE, SIG_IGN); // Ignore broken pipe on POSIX
    }

    inline bool platform_shutdown_requested() {
        return ob::detail::g_shutdown_flag != 0;
    }

#endif
