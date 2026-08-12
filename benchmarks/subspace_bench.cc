#include "common.h"

#include <benchmark/benchmark.h>

#include "absl/status/status.h"
#include "client/client.h"
#include "co/coroutine.h"
#include "common/async/runtime.h"
#include "server/server.h"

#include <unistd.h>
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

}  // namespace

// Ping-pong: publish one reliable message, wait until the subscriber has it,
// then publish the next. Never floods the history.
BENCHMARK_DEFINE_F(SubspaceFixture, ReliablePingPong)
(benchmark::State& state) {
    const std::size_t size = static_cast<std::size_t>(state.range(0));
    const std::string channel = "bench/subspace/" + std::to_string(size) + "/" + std::to_string(reinterpret_cast<uintptr_t>(&state));

    subspace::Client pub_client;
    pub_client.SetThreadSafe(true);
    auto st = pub_client.Init(Socket());
    if (!st.ok()) {
        state.SkipWithError(st.ToString().c_str());
        return;
    }
    subspace::Client sub_client;
    sub_client.SetThreadSafe(true);
    st = sub_client.Init(Socket());
    if (!st.ok()) {
        state.SkipWithError(st.ToString().c_str());
        return;
    }

    auto pub_or =
        pub_client.CreatePublisher(channel, static_cast<int>(size), mw_bench::kNumSlots, subspace::PublisherOptions().SetReliable(true));
    if (!pub_or.ok()) {
        state.SkipWithError(pub_or.status().ToString().c_str());
        return;
    }
    auto sub_or = sub_client.CreateSubscriber(channel, subspace::SubscriberOptions().SetReliable(true));
    if (!sub_or.ok()) {
        state.SkipWithError(sub_or.status().ToString().c_str());
        return;
    }
    auto pub = std::move(pub_or).value();
    auto sub = std::move(sub_or).value();

    uint64_t seq = 0;
    for (auto _ : state) {
        ++seq;
        auto payload = mw_bench::MakePayload(size, seq);

        auto buf_or = pub.GetMessageBuffer(static_cast<int32_t>(payload.size()));
        if (!buf_or.ok()) {
            auto w = pub.Wait();
            if (!w.ok()) {
                state.SkipWithError(w.ToString().c_str());
                break;
            }
            buf_or = pub.GetMessageBuffer(static_cast<int32_t>(payload.size()));
        }
        if (!buf_or.ok()) {
            state.SkipWithError(buf_or.status().ToString().c_str());
            break;
        }
        std::memcpy(*buf_or, payload.data(), payload.size());
        auto pub_st = pub.PublishMessage(static_cast<int32_t>(payload.size()));
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
                break;
            }
        }
    }
done:
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(size));
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK_REGISTER_F(SubspaceFixture, ReliablePingPong)
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
    SubspaceFixture::Shutdown();
    benchmark::Shutdown();
    return 0;
}
