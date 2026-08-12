// One-way Zenoh SHM latency, apples-to-apples with Cyclone/Subspace:
// two sessions in one process (pub listens, sub connects) so traffic cannot
// use same-session local delivery. Critical path: stamp at put → recv.
#include "common.h"
#include "zenoh_common.h"

#include <benchmark/benchmark.h>
#include <zenoh.hxx>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>

namespace {

using mw_bench::zenoh_util::kKey;
using mw_bench::zenoh_util::MakeShmPeerConfig;
using mw_bench::zenoh_util::PickFreePort;
using mw_bench::zenoh_util::TcpLoopback;
using zenoh::AllocAlignment;
using zenoh::KeyExpr;
using zenoh::MemoryLayout;
using zenoh::PosixShmProvider;
using zenoh::Sample;
using zenoh::Session;
using zenoh::ZShmMut;

using ZenohSub = decltype(std::declval<Session&>().declare_subscriber(KeyExpr(""), zenoh::channels::FifoChannel(16)));

class ZenohShmFixture : public benchmark::Fixture {
   public:
    void SetUp(const ::benchmark::State&) override {
        if (started_) {
            return;
        }

        const int port = PickFreePort();
        const std::string endpoint = TcpLoopback(port);

        // Two sessions → transport path (incl. SHM). Same-session loopback is
        // impossible across sessions; we also pin locality to REMOTE.
        pub_session_ = Session::open(MakeShmPeerConfig(endpoint, /*connect=*/{}));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        sub_session_ = Session::open(MakeShmPeerConfig(/*listen=*/{}, endpoint));

        // Large enough for a few outstanding multi-MiB SHM buffers.
        constexpr std::size_t kSegment = 64 * 1024 * 1024;
        provider_.emplace(MemoryLayout(kSegment, AllocAlignment({2})));

        // Warmup on a dedicated key so later benches start clean.
        Warmup();
        started_ = true;
    }

    void TearDown(const ::benchmark::State&) override {}

    static void Shutdown() {
        provider_.reset();
        sub_session_.reset();
        pub_session_.reset();
        started_ = false;
    }

    Session& PubSession() { return *pub_session_; }
    Session& SubSession() { return *sub_session_; }
    PosixShmProvider& Provider() { return *provider_; }

    void PutShm(zenoh::Publisher& pub, std::size_t size, uint64_t seq) {
        auto alloc = provider_->alloc_gc_defrag_blocking(size, AllocAlignment({0}));
        if (!std::holds_alternative<ZShmMut>(alloc)) {
            throw std::runtime_error("SHM alloc failed");
        }
        ZShmMut buf = std::get<ZShmMut>(std::move(alloc));
        // Header only — avoid multi-MiB memcpy on the timed path.
        mw_bench::WriteHeader(buf.data(), seq);
        pub.put(std::move(buf));
    }

    zenoh::Publisher MakePublisher(const KeyExpr& key, bool reliable) {
        zenoh::Session::PublisherOptions pub_opts;
        pub_opts.congestion_control = reliable ? Z_CONGESTION_CONTROL_BLOCK : Z_CONGESTION_CONTROL_DROP;
        pub_opts.is_express = true;
        pub_opts.allowed_destination = Z_LOCALITY_REMOTE;
        return pub_session_->declare_publisher(key, std::move(pub_opts));
    }

    ZenohSub MakeSubscriber(const KeyExpr& key, std::size_t fifo) {
        zenoh::Session::SubscriberOptions sub_opts;
        sub_opts.allowed_origin = Z_LOCALITY_REMOTE;
        return sub_session_->declare_subscriber(key, zenoh::channels::FifoChannel(fifo), std::move(sub_opts));
    }

   private:
    void Warmup() {
        auto pub = MakePublisher(KeyExpr(kKey), /*reliable=*/true);
        auto sub = MakeSubscriber(KeyExpr(kKey), /*fifo=*/16);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        uint64_t warm_seq = 0;
        bool ok = false;
        while (std::chrono::steady_clock::now() < deadline && warm_seq < 64) {
            ++warm_seq;
            auto alloc = provider_->alloc_gc_defrag(64, AllocAlignment({0}));
            if (!std::holds_alternative<ZShmMut>(alloc)) {
                // Free pressure: drain anything pending, then retry.
                while (true) {
                    auto res = sub.handler().try_recv();
                    if (std::holds_alternative<zenoh::channels::RecvError>(res)) {
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            ZShmMut buf = std::get<ZShmMut>(std::move(alloc));
            mw_bench::WriteHeader(buf.data(), warm_seq);
            pub.put(std::move(buf));

            const auto drain_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            while (std::chrono::steady_clock::now() < drain_until) {
                auto res = sub.handler().try_recv();
                if (std::holds_alternative<zenoh::channels::RecvError>(res)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                auto& sample = std::get<Sample>(res);
                if (!sample.get_payload().as_shm().has_value()) {
                    throw std::runtime_error("warmup payload was not SHM — expected transport SHM path");
                }
                auto view = sample.get_payload().as_vector();
                if (view.size() >= sizeof(mw_bench::MessageHeader) && mw_bench::HeaderOf(view.data())->seq == warm_seq) {
                    ok = true;
                    break;
                }
            }
            if (ok) {
                break;
            }
        }
        if (!ok) {
            throw std::runtime_error("warmup put/sub timed out");
        }
    }

    inline static bool started_ = false;
    inline static std::optional<Session> pub_session_;
    inline static std::optional<Session> sub_session_;
    inline static std::optional<PosixShmProvider> provider_;
};

bool RecvMatching(ZenohSub& sub, uint64_t want_seq, int timeout_ms, uint64_t* send_ns_out) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        auto res = sub.handler().try_recv();
        if (std::holds_alternative<zenoh::channels::RecvError>(res)) {
            std::this_thread::yield();
            continue;
        }
        auto& sample = std::get<Sample>(res);
        if (!sample.get_payload().as_shm().has_value()) {
            continue;
        }
        auto shm = sample.get_payload().as_shm();
        if (shm->get().len() < sizeof(mw_bench::MessageHeader)) {
            continue;
        }
        const auto* hdr = mw_bench::HeaderOf(shm->get().data());
        if (want_seq == 0 || hdr->seq == want_seq) {
            if (send_ns_out != nullptr) {
                *send_ns_out = hdr->send_ns;
            }
            return true;
        }
    }
    return false;
}

bool RecvOne(ZenohSub& sub, int timeout_ms, uint64_t* send_ns_out, bool* was_queued_out, uint64_t* wait_ns_out) {
    *was_queued_out = false;
    *wait_ns_out = 0;

    {
        auto res = sub.handler().try_recv();
        if (!std::holds_alternative<zenoh::channels::RecvError>(res)) {
            auto& sample = std::get<Sample>(res);
            if (sample.get_payload().as_shm().has_value()) {
                auto shm = sample.get_payload().as_shm();
                if (shm->get().len() >= sizeof(mw_bench::MessageHeader)) {
                    *send_ns_out = mw_bench::HeaderOf(shm->get().data())->send_ns;
                    *was_queued_out = true;
                    return true;
                }
            }
        }
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    uint64_t wait_ns = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const uint64_t w0 = mw_bench::NowNs();
        std::this_thread::yield();
        wait_ns += mw_bench::NowNs() - w0;
        auto res = sub.handler().try_recv();
        if (std::holds_alternative<zenoh::channels::RecvError>(res)) {
            continue;
        }
        auto& sample = std::get<Sample>(res);
        if (!sample.get_payload().as_shm().has_value()) {
            continue;
        }
        auto shm = sample.get_payload().as_shm();
        if (shm->get().len() < sizeof(mw_bench::MessageHeader)) {
            continue;
        }
        *send_ns_out = mw_bench::HeaderOf(shm->get().data())->send_ns;
        *was_queued_out = false;
        *wait_ns_out = wait_ns;
        return true;
    }
    return false;
}

bool WaitUntilLinked(ZenohShmFixture& fix, zenoh::Publisher& pub, ZenohSub& sub, std::size_t size) {
    // Few probes at the real size (multi-MiB × dozens would exhaust the SHM segment).
    const uint64_t probes = size > 256 * 1024 ? 4 : 32;
    for (uint64_t i = 1; i <= probes; ++i) {
        fix.PutShm(pub, size, i);
        if (RecvMatching(sub, i, /*timeout_ms=*/500, nullptr)) {
            return true;
        }
    }
    return false;
}

void RunPingPong(ZenohShmFixture& fix, benchmark::State& state, bool reliable) {
    const std::size_t size = static_cast<std::size_t>(state.range(0));
    const std::string key_str = std::string("bench/zenoh/pp/") + (reliable ? "r/" : "u/") + std::to_string(size) + "/" +
                                std::to_string(reinterpret_cast<uintptr_t>(&state));
    KeyExpr key(key_str);
    // Subscriber first so the put cannot race an undeclared interest.
    auto sub = fix.MakeSubscriber(key, /*fifo=*/16);
    auto pub = fix.MakePublisher(key, reliable);
    if (!WaitUntilLinked(fix, pub, sub, size)) {
        state.SkipWithError("zenoh pub/sub failed to link");
        return;
    }

    uint64_t seq = 0;
    std::vector<double> lats;
    lats.reserve(4096);
    for (auto _ : state) {
        ++seq;
        fix.PutShm(pub, size, seq);
        uint64_t send_ns = 0;
        if (!RecvMatching(sub, seq, /*timeout_ms=*/2000, &send_ns)) {
            state.SkipWithError("timed out waiting for sample");
            break;
        }
        const double sec = mw_bench::OneWayLatencySec(send_ns);
        lats.push_back(sec);
        state.SetIterationTime(sec);
    }
    mw_bench::ReportLatencyCounters(state, lats);
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(size));
    state.SetItemsProcessed(state.iterations());
}

void RunMtOneWay(ZenohShmFixture& fix, benchmark::State& state, bool reliable) {
    const std::size_t size = static_cast<std::size_t>(state.range(0));
    const std::string key_str = std::string("bench/zenoh/mt/") + (reliable ? "r/" : "u/") + std::to_string(size) + "/" +
                                std::to_string(reinterpret_cast<uintptr_t>(&state));
    KeyExpr key(key_str);
    auto sub = fix.MakeSubscriber(key, /*fifo=*/8);
    auto pub = fix.MakePublisher(key, reliable);
    if (!WaitUntilLinked(fix, pub, sub, size)) {
        state.SkipWithError("zenoh pub/sub failed to link");
        return;
    }

    std::atomic<bool> stop{false};
    std::atomic<int> in_flight{0};
    std::atomic<uint64_t> seq{0};
    const int max_in_flight = 2;
    std::vector<double> lats;
    lats.reserve(4096);
    mw_bench::MtTakeStats take_stats;
    std::thread pub_thread([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            if (max_in_flight > 0 && in_flight.load(std::memory_order_acquire) >= max_in_flight) {
                std::this_thread::yield();
                continue;
            }
            const uint64_t s = seq.fetch_add(1, std::memory_order_relaxed) + 1;
            try {
                fix.PutShm(pub, size, s);
                if (max_in_flight > 0) {
                    in_flight.fetch_add(1, std::memory_order_release);
                }
            } catch (...) {
                if (reliable) {
                    std::this_thread::sleep_for(std::chrono::microseconds(10));
                    continue;
                }
                break;
            }
        }
    });

    for (auto _ : state) {
        // Drain already-queued (not timed). Cap so a racing pub cannot keep
        // the channel non-empty forever.
        for (int drained = 0; drained < 32; ++drained) {
            auto res = sub.handler().try_recv();
            if (std::holds_alternative<zenoh::channels::RecvError>(res)) {
                break;
            }
            auto& sample = std::get<Sample>(res);
            if (!sample.get_payload().as_shm().has_value()) {
                continue;
            }
            auto shm = sample.get_payload().as_shm();
            if (shm->get().len() < sizeof(mw_bench::MessageHeader)) {
                continue;
            }
            const uint64_t send_ns = mw_bench::HeaderOf(shm->get().data())->send_ns;
            const double e2e = mw_bench::OneWayLatencySec(send_ns);
            take_stats.Record(e2e, /*was_queued=*/true, /*wait_sec=*/0.0);
            int cur = in_flight.load(std::memory_order_relaxed);
            while (cur > 0 && !in_flight.compare_exchange_weak(cur, cur - 1, std::memory_order_release, std::memory_order_relaxed)) {}
        }

        uint64_t send_ns = 0;
        bool was_queued = false;
        uint64_t wait_ns = 0;
        if (!RecvOne(sub, /*timeout_ms=*/2000, &send_ns, &was_queued, &wait_ns)) {
            state.SkipWithError("timed out waiting for sample");
            break;
        }
        const double e2e = mw_bench::OneWayLatencySec(send_ns);
        take_stats.Record(e2e, was_queued, static_cast<double>(wait_ns) * 1e-9);
        lats.push_back(e2e);
        state.SetIterationTime(e2e);
        {
            int cur = in_flight.load(std::memory_order_relaxed);
            while (cur > 0 && !in_flight.compare_exchange_weak(cur, cur - 1, std::memory_order_release, std::memory_order_relaxed)) {}
        }
    }
    stop.store(true, std::memory_order_relaxed);
    if (pub_thread.joinable()) {
        pub_thread.join();
    }
    mw_bench::ReportLatencyCounters(state, lats);
    take_stats.Report(state);
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(size));
    state.SetItemsProcessed(state.iterations());
}

}  // namespace

BENCHMARK_DEFINE_F(ZenohShmFixture, ReliablePingPong)
(benchmark::State& state) {
    RunPingPong(*this, state, /*reliable=*/true);
}
BENCHMARK_REGISTER_F(ZenohShmFixture, ReliablePingPong)->Apply(mw_bench::PingPongArgs);

BENCHMARK_DEFINE_F(ZenohShmFixture, MtReliableOneWay)
(benchmark::State& state) {
    RunMtOneWay(*this, state, /*reliable=*/true);
}
BENCHMARK_REGISTER_F(ZenohShmFixture, MtReliableOneWay)->Apply(mw_bench::OneWayLatencyArgs);

BENCHMARK_DEFINE_F(ZenohShmFixture, MtUnreliableOneWay)
(benchmark::State& state) {
    RunMtOneWay(*this, state, /*reliable=*/false);
}
BENCHMARK_REGISTER_F(ZenohShmFixture, MtUnreliableOneWay)->Apply(mw_bench::OneWayLatencyArgs);

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
        return 1;
    }
    benchmark::RunSpecifiedBenchmarks();
    ZenohShmFixture::Shutdown();
    benchmark::Shutdown();
    return 0;
}
