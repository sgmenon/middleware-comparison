#pragma once

#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mw_bench {

inline constexpr int kNumSlots = 32;
// Shallow ring for MT one-way so latency ≈ publish→take, not deep queue delay.
inline constexpr int kMtSlots = 8;

// Payload size sweeps (bytes). MT starts at 1 KiB — 64 B is too small to be
// representative under concurrent pub/sub.
inline constexpr int64_t kSize64 = 64;
inline constexpr int64_t kSize1KiB = 1024;
inline constexpr int64_t kSize16KiB = 16 * 1024;
inline constexpr int64_t kSize64KiB = 64 * 1024;
inline constexpr int64_t kSize256KiB = 256 * 1024;
inline constexpr int64_t kSize1MiB = 1024 * 1024;
inline constexpr int64_t kSize4MiB = 4 * 1024 * 1024;

struct MessageHeader {
    uint64_t seq;
    uint64_t send_ns;
};

inline uint64_t NowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Buffer sized for `size`; only the header is written (no multi-MB memset).
inline std::vector<char> MakePayload(std::size_t size, uint64_t seq) {
    if (size < sizeof(MessageHeader)) {
        size = sizeof(MessageHeader);
    }
    std::vector<char> buf(size);
    auto* hdr = reinterpret_cast<MessageHeader*>(buf.data());
    hdr->seq = seq;
    hdr->send_ns = NowNs();
    return buf;
}

inline void WriteHeader(void* data, uint64_t seq) {
    auto* hdr = reinterpret_cast<MessageHeader*>(data);
    hdr->seq = seq;
    hdr->send_ns = NowNs();
}

// Stamp send time as late as possible (critical-path start = publish).
inline void StampSendNs(void* data) {
    reinterpret_cast<MessageHeader*>(data)->send_ns = NowNs();
}

inline const MessageHeader* HeaderOf(const void* data) {
    return reinterpret_cast<const MessageHeader*>(data);
}

// One-way publish→subscribe latency in seconds (for UseManualTime).
inline double OneWayLatencySec(uint64_t send_ns) {
    const uint64_t now = NowNs();
    if (now <= send_ns) {
        return 0.0;
    }
    return static_cast<double>(now - send_ns) * 1e-9;
}

inline double Percentile(std::vector<double> v, double p) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const double idx = p * static_cast<double>(v.size() - 1);
    const auto lo = static_cast<std::size_t>(idx);
    const auto hi = std::min(lo + 1, v.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - frac) + v[hi] * frac;
}

inline void ReportLatencyCounters(benchmark::State& state, const std::vector<double>& lat_sec) {
    if (lat_sec.empty()) {
        return;
    }
    state.counters["min_ns"] = *std::min_element(lat_sec.begin(), lat_sec.end()) * 1e9;
    state.counters["max_ns"] = *std::max_element(lat_sec.begin(), lat_sec.end()) * 1e9;
    state.counters["median_ns"] = Percentile(lat_sec, 0.50) * 1e9;
    state.counters["p99_ns"] = Percentile(lat_sec, 0.99) * 1e9;
}

// MT take instrumentation: was the sample already waiting, and how long did we block?
struct MtTakeStats {
    uint64_t n = 0;
    uint64_t n_queued = 0;
    double sum_e2e_sec = 0;
    double sum_queued_e2e_sec = 0;
    double sum_fresh_e2e_sec = 0;
    double sum_wait_sec = 0;

    void Record(double e2e_sec, bool was_queued, double wait_sec) {
        ++n;
        sum_e2e_sec += e2e_sec;
        sum_wait_sec += wait_sec;
        if (was_queued) {
            ++n_queued;
            sum_queued_e2e_sec += e2e_sec;
        } else {
            sum_fresh_e2e_sec += e2e_sec;
        }
    }

    void Report(benchmark::State& state) const {
        if (n == 0) {
            return;
        }
        state.counters["queued_pct"] = 100.0 * static_cast<double>(n_queued) / static_cast<double>(n);
        state.counters["wait_ns"] = (sum_wait_sec / static_cast<double>(n)) * 1e9;
        if (n_queued > 0) {
            state.counters["queued_lat_ns"] = (sum_queued_e2e_sec / static_cast<double>(n_queued)) * 1e9;
        }
        const uint64_t n_fresh = n - n_queued;
        if (n_fresh > 0) {
            state.counters["fresh_lat_ns"] = (sum_fresh_e2e_sec / static_cast<double>(n_fresh)) * 1e9;
        }
    }
};

// Paced ping-pong: include 64 B through several MiB.
inline void PingPongArgs(::benchmark::Benchmark* b) {
    b->Arg(kSize64)
        ->Arg(kSize1KiB)
        ->Arg(kSize16KiB)
        ->Arg(kSize64KiB)
        ->Arg(kSize256KiB)
        ->Arg(kSize1MiB)
        ->Arg(kSize4MiB)
        ->ArgName("bytes")
        ->Unit(benchmark::kNanosecond)
        ->UseManualTime();
}

// MT one-way: start at 1 KiB (64 B is not representative) through several MiB.
inline void OneWayLatencyArgs(::benchmark::Benchmark* b) {
    b->Arg(kSize1KiB)
        ->Arg(kSize16KiB)
        ->Arg(kSize64KiB)
        ->Arg(kSize256KiB)
        ->Arg(kSize1MiB)
        ->Arg(kSize4MiB)
        ->ArgName("bytes")
        ->Unit(benchmark::kNanosecond)
        ->UseManualTime();
}

}  // namespace mw_bench
