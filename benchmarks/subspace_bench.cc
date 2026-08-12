#include "common.h"

#include <benchmark/benchmark.h>

#include "absl/status/status.h"
#include "client/client.h"
#include "co/coroutine.h"
#include "common/async/runtime.h"
#include "server/server.h"

#include <unistd.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace {

class SubspaceFixture : public benchmark::Fixture {
   public:
    void SetUp(const ::benchmark::State& state) override {
        (void)state;
        if (started_) {
            return;
        }
        signal(SIGPIPE, SIG_IGN);

        char tmpl[] = "/tmp/mw_bench_subspaceXXXXXX";
        int fd = ::mkstemp(tmpl);
        if (fd >= 0) {
            ::close(fd);
            ::unlink(tmpl);
        }
        socket_ = tmpl;

        (void)::pipe(server_pipe_);
        server_ = std::make_unique<subspace::Server>(engine_, socket_, /*interface=*/"", /*disc_port=*/0, /*peer_port=*/0,
                                                     /*local=*/true, server_pipe_[1], /*initial_ordinal=*/1,
                                                     /*wait_for_clients=*/true);

        server_thread_ = std::thread([]() {
            absl::Status s = server_->Run();
            if (!s.ok()) {
                std::fprintf(stderr, "Subspace server error: %s\n", s.ToString().c_str());
            }
        });

        char buf[8];
        (void)::read(server_pipe_[0], buf, 8);
        started_ = true;
    }

    void TearDown(const ::benchmark::State& state) override { (void)state; }

    static void Shutdown() {
        if (!started_ || !server_) {
            return;
        }
        server_->Stop();
        char buf[8];
        (void)::read(server_pipe_[0], buf, 8);
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        server_->CleanupAfterSession();
        (void)::remove(socket_.c_str());
        server_.reset();
        started_ = false;
    }

    const std::string& Socket() const { return socket_; }

   private:
    inline static bool started_ = false;
    inline static subspace::async::RuntimeEngine engine_;
    inline static std::string socket_;
    inline static int server_pipe_[2];
    inline static std::unique_ptr<subspace::Server> server_;
    inline static std::thread server_thread_;
};

void RunPingPong(SubspaceFixture& fix, benchmark::State& state, bool reliable) {
    const std::size_t size = static_cast<std::size_t>(state.range(0));
    const std::string channel = "bench/subspace/pp/" + std::to_string(static_cast<int>(reliable)) + "/" + std::to_string(size) + "/" +
                                std::to_string(reinterpret_cast<uintptr_t>(&state));

    subspace::Client pub_client;
    pub_client.SetThreadSafe(true);
    auto st = pub_client.Init(fix.Socket());
    if (!st.ok()) {
        state.SkipWithError(st.ToString().c_str());
        return;
    }
    subspace::Client sub_client;
    sub_client.SetThreadSafe(true);
    st = sub_client.Init(fix.Socket());
    if (!st.ok()) {
        state.SkipWithError(st.ToString().c_str());
        return;
    }

    auto pub_or = pub_client.CreatePublisher(channel, static_cast<int>(size), mw_bench::kNumSlots,
                                             subspace::PublisherOptions().SetReliable(reliable));
    if (!pub_or.ok()) {
        state.SkipWithError(pub_or.status().ToString().c_str());
        return;
    }
    subspace::SubscriberOptions sub_opts;
    sub_opts.SetReliable(reliable);
    if (!reliable) {
        sub_opts.SetLogDroppedMessages(false);
    }
    auto sub_or = sub_client.CreateSubscriber(channel, sub_opts);
    if (!sub_or.ok()) {
        state.SkipWithError(sub_or.status().ToString().c_str());
        return;
    }
    auto pub = std::move(pub_or).value();
    auto sub = std::move(sub_or).value();

    uint64_t seq = 0;
    std::vector<double> lats;
    lats.reserve(4096);
    for (auto _ : state) {
        ++seq;

        auto buf_or = pub.GetMessageBuffer(static_cast<int32_t>(size));
        if (!buf_or.ok()) {
            auto w = pub.Wait();
            if (!w.ok()) {
                state.SkipWithError(w.ToString().c_str());
                break;
            }
            buf_or = pub.GetMessageBuffer(static_cast<int32_t>(size));
        }
        if (!buf_or.ok()) {
            state.SkipWithError(buf_or.status().ToString().c_str());
            break;
        }
        mw_bench::WriteHeader(*buf_or, seq);
        auto pub_st = pub.PublishMessage(static_cast<int32_t>(size));
        if (!pub_st.ok()) {
            state.SkipWithError(pub_st.status().ToString().c_str());
            break;
        }

        for (;;) {
            auto msg_or = sub.ReadMessage();
            if (!msg_or.ok()) {
                state.SkipWithError(msg_or.status().ToString().c_str());
                goto done;
            }
            auto msg = std::move(msg_or).value();
            if (msg.length == 0) {
                auto w = sub.Wait();
                if (!w.ok()) {
                    state.SkipWithError(w.ToString().c_str());
                    goto done;
                }
                continue;
            }
            if (msg.length < sizeof(mw_bench::MessageHeader)) {
                state.SkipWithError("short message");
                goto done;
            }
            const auto* hdr = mw_bench::HeaderOf(msg.buffer);
            if (hdr->seq == seq) {
                const double sec = mw_bench::OneWayLatencySec(hdr->send_ns);
                lats.push_back(sec);
                state.SetIterationTime(sec);
                break;
            }
        }
    }
done:
    mw_bench::ReportLatencyCounters(state, lats);
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(size));
    state.SetItemsProcessed(state.iterations());
}

// Multithreaded pub→sub with a small in-flight credit limit so each sample's
// send_ns→take latency stays on the critical path (not deep-queue delay).
void RunMtOneWay(SubspaceFixture& fix, benchmark::State& state, bool reliable) {
    const std::size_t size = static_cast<std::size_t>(state.range(0));
    const std::string channel = "bench/subspace/mt/" + std::to_string(static_cast<int>(reliable)) + "/" + std::to_string(size) + "/" +
                                std::to_string(reinterpret_cast<uintptr_t>(&state));

    subspace::Client pub_client;
    pub_client.SetThreadSafe(true);
    auto st = pub_client.Init(fix.Socket());
    if (!st.ok()) {
        state.SkipWithError(st.ToString().c_str());
        return;
    }
    subspace::Client sub_client;
    sub_client.SetThreadSafe(true);
    st = sub_client.Init(fix.Socket());
    if (!st.ok()) {
        state.SkipWithError(st.ToString().c_str());
        return;
    }

    auto pub_or =
        pub_client.CreatePublisher(channel, static_cast<int>(size), mw_bench::kMtSlots, subspace::PublisherOptions().SetReliable(reliable));
    if (!pub_or.ok()) {
        state.SkipWithError(pub_or.status().ToString().c_str());
        return;
    }
    subspace::SubscriberOptions sub_opts;
    sub_opts.SetReliable(reliable);
    if (!reliable) {
        sub_opts.SetLogDroppedMessages(false);
    }
    auto sub_or = sub_client.CreateSubscriber(channel, sub_opts);
    if (!sub_or.ok()) {
        state.SkipWithError(sub_or.status().ToString().c_str());
        return;
    }
    auto pub = std::move(pub_or).value();
    auto sub = std::move(sub_or).value();

    std::atomic<bool> stop{false};
    std::atomic<int> in_flight{0};
    std::atomic<uint64_t> seq{0};
    // Credit limit for both: without it, unreliable flood makes drain never empty.
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
            auto buf_or = pub.GetMessageBuffer(static_cast<int32_t>(size));
            if (!buf_or.ok()) {
                break;
            }
            if (*buf_or == nullptr) {
                if (reliable) {
                    auto w = pub.Wait();
                    if (!w.ok()) {
                        break;
                    }
                } else {
                    std::this_thread::yield();
                }
                continue;
            }
            auto* raw = static_cast<char*>(*buf_or);
            mw_bench::WriteHeader(raw, s);
            auto pub_st = pub.PublishMessage(static_cast<int32_t>(size));
            if (!pub_st.ok()) {
                break;
            }
            if (max_in_flight > 0) {
                in_flight.fetch_add(1, std::memory_order_release);
            }
        }
    });

    for (auto _ : state) {
        // Drain already-queued (not timed). Cap iterations so a racing pub cannot
        // keep the queue non-empty forever.
        for (int drained = 0; drained < 32; ++drained) {
            auto msg_or = sub.ReadMessage();
            if (!msg_or.ok()) {
                state.SkipWithError(msg_or.status().ToString().c_str());
                goto mt_done;
            }
            auto msg = std::move(msg_or).value();
            if (msg.length == 0) {
                break;
            }
            if (msg.length < sizeof(mw_bench::MessageHeader)) {
                state.SkipWithError("short message");
                goto mt_done;
            }
            const double e2e = mw_bench::OneWayLatencySec(mw_bench::HeaderOf(msg.buffer)->send_ns);
            take_stats.Record(e2e, /*was_queued=*/true, /*wait_sec=*/0.0);
            int cur = in_flight.load(std::memory_order_relaxed);
            while (cur > 0 && !in_flight.compare_exchange_weak(cur, cur - 1, std::memory_order_release, std::memory_order_relaxed)) {}
        }

        // Timed: wait for a fresh sample.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        uint64_t wait_ns = 0;
        bool got = false;
        const mw_bench::MessageHeader* hdr = nullptr;
        while (std::chrono::steady_clock::now() < deadline) {
            auto msg_or = sub.ReadMessage();
            if (!msg_or.ok()) {
                state.SkipWithError(msg_or.status().ToString().c_str());
                goto mt_done;
            }
            auto msg = std::move(msg_or).value();
            if (msg.length == 0) {
                bool spun = false;
                for (int spin = 0; spin < 64; ++spin) {
                    msg_or = sub.ReadMessage();
                    if (!msg_or.ok()) {
                        state.SkipWithError(msg_or.status().ToString().c_str());
                        goto mt_done;
                    }
                    msg = std::move(msg_or).value();
                    if (msg.length > 0) {
                        spun = true;
                        break;
                    }
                }
                if (!spun) {
                    if (!reliable) {
                        // Unreliable: Wait can miss wakeups under drop/overwrite.
                        const uint64_t w0 = mw_bench::NowNs();
                        std::this_thread::yield();
                        wait_ns += mw_bench::NowNs() - w0;
                        continue;
                    }
                    const uint64_t w0 = mw_bench::NowNs();
                    auto w = sub.Wait();
                    wait_ns += mw_bench::NowNs() - w0;
                    if (!w.ok()) {
                        state.SkipWithError(w.ToString().c_str());
                        goto mt_done;
                    }
                    continue;
                }
            }
            if (msg.length < sizeof(mw_bench::MessageHeader)) {
                state.SkipWithError("short message");
                goto mt_done;
            }
            hdr = mw_bench::HeaderOf(msg.buffer);
            got = true;
            break;
        }
        if (!got) {
            state.SkipWithError("timed out waiting for fresh sample");
            break;
        }
        const double e2e = mw_bench::OneWayLatencySec(hdr->send_ns);
        const bool was_queued = (wait_ns == 0);
        take_stats.Record(e2e, was_queued, static_cast<double>(wait_ns) * 1e-9);
        lats.push_back(e2e);
        state.SetIterationTime(e2e);
        {
            int cur = in_flight.load(std::memory_order_relaxed);
            while (cur > 0 && !in_flight.compare_exchange_weak(cur, cur - 1, std::memory_order_release, std::memory_order_relaxed)) {}
        }
    }
mt_done:
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

BENCHMARK_DEFINE_F(SubspaceFixture, ReliablePingPong)
(benchmark::State& state) {
    RunPingPong(*this, state, /*reliable=*/true);
}
BENCHMARK_REGISTER_F(SubspaceFixture, ReliablePingPong)->Apply(mw_bench::PingPongArgs);

BENCHMARK_DEFINE_F(SubspaceFixture, MtReliableOneWay)
(benchmark::State& state) {
    RunMtOneWay(*this, state, /*reliable=*/true);
}
BENCHMARK_REGISTER_F(SubspaceFixture, MtReliableOneWay)->Apply(mw_bench::OneWayLatencyArgs);

BENCHMARK_DEFINE_F(SubspaceFixture, MtUnreliableOneWay)
(benchmark::State& state) {
    RunMtOneWay(*this, state, /*reliable=*/false);
}
BENCHMARK_REGISTER_F(SubspaceFixture, MtUnreliableOneWay)->Apply(mw_bench::OneWayLatencyArgs);

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
        return 1;
    }
    benchmark::RunSpecifiedBenchmarks();
    SubspaceFixture::Shutdown();
    benchmark::Shutdown();
    return 0;
}
