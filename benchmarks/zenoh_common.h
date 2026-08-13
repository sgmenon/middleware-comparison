#pragma once

#include <zenoh.hxx>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <stdexcept>
#include <string>

namespace mw_bench::zenoh_util {

inline constexpr const char* kKey = "bench/zenoh/shm/oneway";
inline constexpr const char* kTcpKey = "bench/zenoh/tcp/oneway";

inline zenoh::Config MakePeerConfig(const std::string& listen_or_empty, const std::string& connect_or_empty, bool shm) {
    zenoh::Config config = zenoh::Config::create_default();
    config.insert_json5(Z_CONFIG_MODE_KEY, "\"peer\"");
    config.insert_json5(Z_CONFIG_SHARED_MEMORY_KEY, shm ? "true" : "false");
    if (!listen_or_empty.empty() || !connect_or_empty.empty()) {
        config.insert_json5("scouting/multicast/enabled", "false");
    }
    if (!listen_or_empty.empty()) {
        config.insert_json5(Z_CONFIG_LISTEN_KEY, "[\"" + listen_or_empty + "\"]");
    }
    if (!connect_or_empty.empty()) {
        config.insert_json5(Z_CONFIG_CONNECT_KEY, "[\"" + connect_or_empty + "\"]");
    }
    return config;
}

inline zenoh::Config MakeShmPeerConfig(const std::string& listen_or_empty, const std::string& connect_or_empty) {
    return MakePeerConfig(listen_or_empty, connect_or_empty, /*shm=*/true);
}

inline zenoh::Config MakeTcpPeerConfig(const std::string& listen_or_empty, const std::string& connect_or_empty) {
    return MakePeerConfig(listen_or_empty, connect_or_empty, /*shm=*/false);
}

inline int PickFreePort() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("socket failed");
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        throw std::runtime_error("bind failed");
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        throw std::runtime_error("getsockname failed");
    }
    ::close(fd);
    return ntohs(addr.sin_port);
}

inline int PickFreeUdpPort() {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        throw std::runtime_error("udp socket failed");
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        throw std::runtime_error("udp bind failed");
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        throw std::runtime_error("udp getsockname failed");
    }
    ::close(fd);
    return ntohs(addr.sin_port);
}

inline std::string TcpLoopback(int port) {
    return "tcp/127.0.0.1:" + std::to_string(port);
}

}  // namespace mw_bench::zenoh_util
