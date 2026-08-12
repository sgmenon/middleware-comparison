// One-way Zenoh SHM latency, apples-to-apples with Cyclone/Subspace:
// two sessions in one process (pub listens, sub connects) so traffic cannot
// use same-session local delivery. Timer is put → matching sample on the sub.
#include "common.h"
#include "zenoh_common.h"

#include <benchmark/benchmark.h>
#include <zenoh.hxx>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>

namespace {

using zenoh::AllocAlignment;
using zenoh::KeyExpr;
using zenoh::MemoryLayout;
using zenoh::PosixShmProvider;
using zenoh::Sample;
using zenoh::Session;
using zenoh::ZShmMut;

namespace {
inline constexpr const char* kKey = "bench/zenoh/shm/oneway";

inline zenoh::Config MakeShmPeerConfig(const std::string& listen_or_empty, const std::string& connect_or_empty) {
    zenoh::Config config = zenoh::Config::create_default();
    config.insert_json5(Z_CONFIG_MODE_KEY, "\"peer\"");
    config.insert_json5(Z_CONFIG_SHARED_MEMORY_KEY, "true");
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

inline std::string TcpLoopback(int port) {
    return "tcp/127.0.0.1:" + std::to_string(port);
}
}  // namespace

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

        zenoh::Session::PublisherOptions pub_opts;
        pub_opts.congestion_control = Z_CONGESTION_CONTROL_BLOCK;
        pub_opts.is_express = true;
        pub_opts.allowed_destination = Z_LOCALITY_REMOTE;
        pub_ = pub_session_->declare_publisher(KeyExpr(kKey), std::move(pub_opts));

        zenoh::Session::SubscriberOptions sub_opts;
        sub_opts.allowed_origin = Z_LOCALITY_REMOTE;
        sub_ = sub_session_->declare_subscriber(KeyExpr(kKey), zenoh::channels::FifoChannel(16), std::move(sub_opts));

        constexpr std::size_t kSegment = 8 * 1024 * 1024;
        provider_.emplace(MemoryLayout(kSegment, AllocAlignment({2})));

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        uint64_t warm_seq = 0;
        bool ok = false;
        while (std::chrono::steady_clock::now() < deadline) {
            ++warm_seq;
            PutShm(/*size=*/64, warm_seq);
            const auto drain_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
            while (std::chrono::steady_clock::now() < drain_until) {
                auto res = sub_->handler().try_recv();
                if (std::holds_alternative<zenoh::channels::RecvError>(res)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                auto& sample = std::get<Sample>(res);
                if (!sample.get_payload().as_shm().has_value()) {
                    throw std::runtime_error("warmup payload was not SHM — expected transport SHM path");
                }
                auto buf = sample.get_payload().as_vector();
                if (buf.size() >= sizeof(mw_bench::MessageHeader) && mw_bench::HeaderOf(buf.data())->seq == warm_seq) {
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
        started_ = true;
    }

    void TearDown(const ::benchmark::State&) override {}

    static void Shutdown() {
        sub_.reset();
        pub_.reset();
        provider_.reset();
        sub_session_.reset();
        pub_session_.reset();
        started_ = false;
    }

    void PutShm(std::size_t size, uint64_t seq) {
        auto alloc = provider_->alloc_gc_defrag_blocking(size, AllocAlignment({0}));
        if (!std::holds_alternative<ZShmMut>(alloc)) {
            throw std::runtime_error("SHM alloc failed");
        }
        ZShmMut buf = std::get<ZShmMut>(std::move(alloc));
        auto payload = mw_bench::MakePayload(size, seq);
        std::memcpy(buf.data(), payload.data(), payload.size());
        pub_->put(std::move(buf));
    }

    ZenohSub& sub() { return *sub_; }

   private:
    inline static bool started_ = false;
    inline static std::optional<Session> pub_session_;
    inline static std::optional<Session> sub_session_;
    inline static std::optional<zenoh::Publisher> pub_;
    inline static std::optional<ZenohSub> sub_;
    inline static std::optional<PosixShmProvider> provider_;
};

}  // namespace

// Timed: SHM put → wait for matching seq on the other session (one-way).
BENCHMARK_DEFINE_F(ZenohShmFixture, ReliablePingPong)
(benchmark::State& state) {
    const std::size_t size = static_cast<std::size_t>(state.range(0));
    auto& sub = this->sub();

    uint64_t seq = 0;
    for (auto _ : state) {
        ++seq;
        PutShm(size, seq);
        for (;;) {
            auto res = sub.handler().recv();
            if (!std::holds_alternative<Sample>(res)) {
                state.SkipWithError("subscriber recv failed");
                goto done;
            }
            const auto& sample = std::get<Sample>(res);
            if (!sample.get_payload().as_shm().has_value()) {
                state.SkipWithError("received non-SHM payload");
                goto done;
            }
            auto shm = sample.get_payload().as_shm();
            if (shm->get().len() < sizeof(mw_bench::MessageHeader)) {
                state.SkipWithError("short message");
                goto done;
            }
            if (mw_bench::HeaderOf(shm->get().data())->seq == seq) {
                break;
            }
        }
    }
done:
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(size));
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK_REGISTER_F(ZenohShmFixture, ReliablePingPong)
    ->Arg(64)
    ->Arg(1024)
    ->Arg(16384)
    ->Arg(65536)
    ->ArgName("bytes")
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime();

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
