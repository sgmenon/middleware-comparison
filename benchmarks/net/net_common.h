#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace mw_bench::net {

inline uint64_t NowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Options {
    std::string role;  // "pub" | "sub"
    std::string peer;  // host:port (pub side)
    std::string channel = "bench/net";
    std::size_t size = 1024;
    int count = 1000;
    int disc_port = 7420;  // subspace / zenoh listen
    int warmup = 50;
    std::string stack;  // filled by each binary for CSV
};

inline bool StartsWith(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(0, p.size()) == p;
}

inline bool ParseHostPort(const std::string& peer, std::string* host, int* port) {
    const auto colon = peer.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= peer.size()) {
        return false;
    }
    *host = peer.substr(0, colon);
    *port = std::atoi(peer.c_str() + colon + 1);
    return !host->empty() && *port > 0;
}

inline bool ParseOptions(int argc, char** argv, Options* out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto take = [&](std::string_view key) -> const char* {
            if (StartsWith(a, key)) {
                return argv[i] + key.size();
            }
            return nullptr;
        };
        if (const char* v = take("--role=")) {
            out->role = v;
        } else if (const char* v = take("--peer=")) {
            out->peer = v;
        } else if (const char* v = take("--channel=")) {
            out->channel = v;
        } else if (const char* v = take("--size=")) {
            out->size = static_cast<std::size_t>(std::strtoull(v, nullptr, 10));
        } else if (const char* v = take("--count=")) {
            out->count = std::atoi(v);
        } else if (const char* v = take("--disc-port=")) {
            out->disc_port = std::atoi(v);
        } else if (const char* v = take("--warmup=")) {
            out->warmup = std::atoi(v);
        } else if (a == "--help" || a == "-h") {
            std::fprintf(stderr,
                         "Usage: %s --role=pub|sub [--peer=host:port] [--size=N] [--count=N] "
                         "[--channel=NAME] [--disc-port=N] [--warmup=N]\n",
                         argv[0]);
            return false;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
            return false;
        }
    }
    if (out->role != "pub" && out->role != "sub") {
        std::fprintf(stderr, "--role=pub|sub is required\n");
        return false;
    }
    if (out->role == "pub" && out->peer.empty()) {
        std::fprintf(stderr, "pub requires --peer=host:port\n");
        return false;
    }
    if (out->size == 0 || out->count <= 0) {
        std::fprintf(stderr, "invalid --size/--count\n");
        return false;
    }
    return true;
}

inline double MeanUs(const std::vector<double>& us) {
    if (us.empty()) {
        return 0;
    }
    double s = 0;
    for (double x : us) {
        s += x;
    }
    return s / static_cast<double>(us.size());
}

inline double PercentileUs(std::vector<double> us, double p) {
    if (us.empty()) {
        return 0;
    }
    std::sort(us.begin(), us.end());
    const double idx = p * static_cast<double>(us.size() - 1);
    const auto lo = static_cast<std::size_t>(idx);
    const auto hi = std::min(lo + 1, us.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return us[lo] * (1.0 - frac) + us[hi] * frac;
}

// Pub prints: stack,size,n,mean_us,p50_us,p99_us
inline void PrintCsv(const Options& opt, const std::vector<double>& half_rtt_us) {
    std::printf("%s,%zu,%zu,%.3f,%.3f,%.3f\n", opt.stack.c_str(), opt.size, half_rtt_us.size(), MeanUs(half_rtt_us),
                PercentileUs(half_rtt_us, 0.50), PercentileUs(half_rtt_us, 0.99));
    std::fflush(stdout);
}

inline std::string ReqKey(const Options& opt) {
    return opt.channel + "/req";
}

inline std::string RepKey(const Options& opt) {
    return opt.channel + "/rep";
}

}  // namespace mw_bench::net
