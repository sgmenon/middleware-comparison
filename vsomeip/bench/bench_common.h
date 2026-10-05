#pragma once

#include "constants.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vsomeip_bench {

inline std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Options {
    std::string stack;
    std::string transport = "udp";
    std::size_t size = 4096;
    int count = 1000;
    int warmup = 50;
    double rate_hz = 1000.0;
    std::size_t max_datagram = 1400;
    int rpc_calls = 0;
};

inline bool StartsWith(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(0, p.size()) == p;
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
        if (const char* v = take("--size=")) {
            out->size = static_cast<std::size_t>(std::strtoull(v, nullptr, 10));
        } else if (const char* v = take("--count=")) {
            out->count = std::atoi(v);
        } else if (const char* v = take("--warmup=")) {
            out->warmup = std::atoi(v);
        } else if (const char* v = take("--rate-hz=")) {
            out->rate_hz = std::atof(v);
        } else if (const char* v = take("--max-datagram=")) {
            out->max_datagram = static_cast<std::size_t>(std::strtoull(v, nullptr, 10));
        } else if (const char* v = take("--stack=")) {
            out->stack = v;
        } else if (const char* v = take("--transport=")) {
            out->transport = v;
        } else if (const char* v = take("--rpc-calls=")) {
            out->rpc_calls = std::atoi(v);
        } else if (a == "--help" || a == "-h") {
            std::fprintf(stderr,
                         "Usage: %s [--size=N] [--count=N] [--warmup=N] "
                         "[--rate-hz=F] [--max-datagram=N] [--stack=NAME] "
                         "[--transport=udp|tcp] [--rpc-calls=N]\n",
                         argv[0]);
            return false;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
            return false;
        }
    }
    if (out->size < kBenchHeaderBytes || out->count <= 0 || out->rate_hz <= 0) {
        std::fprintf(stderr, "invalid size/count/rate-hz (size must be >= %zu)\n", kBenchHeaderBytes);
        return false;
    }
    if (out->transport != "udp" && out->transport != "tcp") {
        std::fprintf(stderr, "transport must be udp or tcp\n");
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

inline void PrintCsv(const Options& opt, const std::vector<double>& latency_us, std::uint64_t gap_count) {
    std::printf("%s,%zu,%.1f,%zu,%.3f,%.3f,%.3f,%llu\n", opt.stack.c_str(), opt.size, opt.rate_hz, latency_us.size(), MeanUs(latency_us),
                PercentileUs(latency_us, 0.50), PercentileUs(latency_us, 0.99), static_cast<unsigned long long>(gap_count));
    std::fflush(stdout);
}

/** Lets pub container exit while vsomeip shutdown may still block in app_->stop(). */
inline void TouchBenchSubDone() {
    const char* sync_dir = std::getenv("VSOMEIP_BENCH_SYNC_DIR");
    if (!sync_dir) {
        return;
    }
    std::string path = std::string(sync_dir) + "/.bench-sub-done";
    if (FILE* f = std::fopen(path.c_str(), "w")) {
        std::fputc('1', f);
        std::fclose(f);
    }
}

inline void StampPayload(std::uint8_t* data, std::size_t length, std::uint64_t send_ns, std::uint32_t seq) {
    if (length < kBenchHeaderBytes) {
        return;
    }
    std::memcpy(data + kBenchSendNsOffset, &send_ns, 8);
    std::memcpy(data + kBenchSeqOffset, &seq, 4);
    for (std::size_t i = kBenchHeaderBytes; i < length; ++i) {
        data[i] = static_cast<std::uint8_t>((i * 131u + seq) & 0xffu);
    }
}

inline bool ReadStamp(const std::uint8_t* data, std::size_t length, std::uint64_t* send_ns, std::uint32_t* seq) {
    if (length < kBenchHeaderBytes) {
        return false;
    }
    std::memcpy(send_ns, data + kBenchSendNsOffset, 8);
    std::memcpy(seq, data + kBenchSeqOffset, 4);
    return true;
}

}  // namespace vsomeip_bench
