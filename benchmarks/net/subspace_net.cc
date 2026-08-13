// Subspace network ping-pong CLI: RTT/2 with local timestamps only.
// Each process embeds a Subspace server (local=false); pub dials --peer=host:port.
#include "bench_sample.h"
#include "net_common.h"

#include "absl/status/status.h"
#include "client/client.h"
#include "common/async/runtime.h"
#include "server/server.h"

#include <unistd.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "flatbuffers/flatbuffers.h"
#include "toolbelt/sockets.h"

namespace {

using mw_bench::net::Options;
using mw_bench::net::ParseHostPort;
using mw_bench::net::ParseOptions;
using mw_bench::net::PrintCsv;
using mw_bench::net::RepKey;
using mw_bench::net::ReqKey;

struct EmbeddedServer {
    subspace::async::RuntimeEngine engine;
    std::string socket;
    int pipe_fd[2]{-1, -1};
    std::unique_ptr<subspace::Server> server;
    std::thread thread;

    ~EmbeddedServer() {
        if (server) {
            server->Stop();
            char buf[8];
            if (pipe_fd[0] >= 0) {
                (void)::read(pipe_fd[0], buf, 8);
            }
            if (thread.joinable()) {
                thread.join();
            }
            server->CleanupAfterSession();
            (void)::remove(socket.c_str());
        }
        if (pipe_fd[0] >= 0) {
            ::close(pipe_fd[0]);
        }
        if (pipe_fd[1] >= 0) {
            ::close(pipe_fd[1]);
        }
    }
};

std::unique_ptr<EmbeddedServer> StartServer(const Options& opt) {
    auto s = std::make_unique<EmbeddedServer>();
    signal(SIGPIPE, SIG_IGN);

    char tmpl[] = "/tmp/mw_net_subspaceXXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd >= 0) {
        ::close(fd);
        ::unlink(tmpl);
    }
    s->socket = tmpl;
    if (::pipe(s->pipe_fd) != 0) {
        std::fprintf(stderr, "pipe failed\n");
        return nullptr;
    }

    const int disc = opt.disc_port;
    if (opt.role == "pub") {
        std::string host;
        int peer_port = 0;
        if (!ParseHostPort(opt.peer, &host, &peer_port)) {
            std::fprintf(stderr, "bad --peer=%s\n", opt.peer.c_str());
            return nullptr;
        }
        toolbelt::InetAddress peer(host, peer_port);
        s->server = std::make_unique<subspace::Server>(s->engine, s->socket, /*interface=*/"", peer, disc, peer_port,
                                                       /*local=*/false, s->pipe_fd[1], /*initial_ordinal=*/1,
                                                       /*wait_for_clients=*/true);
        s->server->SetMachineName("pub");
    } else {
        s->server = std::make_unique<subspace::Server>(s->engine, s->socket, /*interface=*/"", disc, /*peer_port=*/0,
                                                       /*local=*/false, s->pipe_fd[1], /*initial_ordinal=*/1,
                                                       /*wait_for_clients=*/true);
        s->server->SetMachineName("sub");
    }
    s->server->SetTcpDiscovery(true);

    EmbeddedServer* raw = s.get();
    s->thread = std::thread([raw]() {
        absl::Status st = raw->server->Run();
        if (!st.ok()) {
            std::fprintf(stderr, "subspace server error: %s\n", st.ToString().c_str());
        }
    });
    char buf[8];
    (void)::read(s->pipe_fd[0], buf, 8);
    return s;
}

bool PublishFbs(subspace::Publisher& pub, flatbuffers::FlatBufferBuilder& fbb, uint64_t seq, std::size_t size) {
    mw_bench::BuildSample(fbb, seq, size);
    const auto nbytes = static_cast<int32_t>(fbb.GetSize());
    auto buf_or = pub.GetMessageBuffer(nbytes);
    if (!buf_or.ok()) {
        auto w = pub.Wait();
        if (!w.ok()) {
            return false;
        }
        buf_or = pub.GetMessageBuffer(nbytes);
    }
    if (!buf_or.ok() || *buf_or == nullptr) {
        return false;
    }
    std::memcpy(*buf_or, fbb.GetBufferPointer(), static_cast<std::size_t>(nbytes));
    mw_bench::StampSample(*buf_or);
    return pub.PublishMessage(nbytes).ok();
}

bool ReadMatching(subspace::Subscriber& sub, uint64_t want, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        auto msg_or = sub.ReadMessage();
        if (!msg_or.ok()) {
            return false;
        }
        auto msg = std::move(msg_or).value();
        if (msg.length == 0) {
            (void)sub.Wait();
            continue;
        }
        uint64_t got = 0;
        if (mw_bench::ReadSampleMeta(msg.buffer, static_cast<std::size_t>(msg.length), &got, nullptr) && got == want) {
            return true;
        }
    }
    return false;
}

int RunPub(const Options& opt, EmbeddedServer& srv) {
    const int slot_bytes = static_cast<int>(mw_bench::SampleSlotBytes(opt.size));
    const std::string req = ReqKey(opt);
    const std::string rep = RepKey(opt);

    subspace::Client client;
    client.SetThreadSafe(true);
    auto st = client.Init(srv.socket);
    if (!st.ok()) {
        std::fprintf(stderr, "client init: %s\n", st.ToString().c_str());
        return 1;
    }

    auto pub_or = client.CreatePublisher(
        req, subspace::PublisherOptions().SetSlotSize(slot_bytes).SetNumSlots(mw_bench::kNumSlots).SetReliable(true).SetLocal(false));
    if (!pub_or.ok()) {
        std::fprintf(stderr, "create pub: %s\n", pub_or.status().ToString().c_str());
        return 1;
    }
    auto sub_or = client.CreateSubscriber(rep, subspace::SubscriberOptions().SetReliable(true));
    if (!sub_or.ok()) {
        std::fprintf(stderr, "create sub: %s\n", sub_or.status().ToString().c_str());
        return 1;
    }
    auto pub = std::move(pub_or).value();
    auto sub = std::move(sub_or).value();

    flatbuffers::FlatBufferBuilder fbb;
    // Wait for bridge + peer echoer.
    bool linked = false;
    for (int i = 0; i < 200 && !linked; ++i) {
        const uint64_t probe = static_cast<uint64_t>(i + 1);
        if (PublishFbs(pub, fbb, probe, opt.size) && ReadMatching(sub, probe, 200)) {
            linked = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!linked) {
        std::fprintf(stderr, "subspace bridge failed to link (no echo)\n");
        return 1;
    }

    std::vector<double> half_us;
    half_us.reserve(static_cast<std::size_t>(opt.count));
    uint64_t seq = 1000;
    for (int i = 0; i < opt.warmup + opt.count; ++i) {
        ++seq;
        const uint64_t t0 = mw_bench::net::NowNs();
        if (!PublishFbs(pub, fbb, seq, opt.size)) {
            std::fprintf(stderr, "publish failed at seq %llu\n", static_cast<unsigned long long>(seq));
            return 1;
        }
        if (!ReadMatching(sub, seq, 5000)) {
            std::fprintf(stderr, "timeout waiting echo seq %llu\n", static_cast<unsigned long long>(seq));
            return 1;
        }
        const uint64_t t1 = mw_bench::net::NowNs();
        if (i >= opt.warmup) {
            half_us.push_back(static_cast<double>(t1 - t0) / 2.0 * 1e-3);  // ns → µs of RTT/2
        }
    }
    PrintCsv(opt, half_us);
    return 0;
}

int RunSub(const Options& opt, EmbeddedServer& srv) {
    const int slot_bytes = static_cast<int>(mw_bench::SampleSlotBytes(opt.size));
    const std::string req = ReqKey(opt);
    const std::string rep = RepKey(opt);

    subspace::Client client;
    client.SetThreadSafe(true);
    auto st = client.Init(srv.socket);
    if (!st.ok()) {
        std::fprintf(stderr, "client init: %s\n", st.ToString().c_str());
        return 1;
    }

    auto sub_or = client.CreateSubscriber(req, subspace::SubscriberOptions().SetReliable(true));
    if (!sub_or.ok()) {
        std::fprintf(stderr, "create sub: %s\n", sub_or.status().ToString().c_str());
        return 1;
    }
    auto pub_or = client.CreatePublisher(
        rep, subspace::PublisherOptions().SetSlotSize(slot_bytes).SetNumSlots(mw_bench::kNumSlots).SetReliable(true).SetLocal(false));
    if (!pub_or.ok()) {
        std::fprintf(stderr, "create pub: %s\n", pub_or.status().ToString().c_str());
        return 1;
    }
    auto sub = std::move(sub_or).value();
    auto pub = std::move(pub_or).value();

    flatbuffers::FlatBufferBuilder fbb;
    // Echo forever until killed (compose stops after pub finishes).
    for (;;) {
        auto msg_or = sub.ReadMessage();
        if (!msg_or.ok()) {
            std::fprintf(stderr, "read: %s\n", msg_or.status().ToString().c_str());
            return 1;
        }
        auto msg = std::move(msg_or).value();
        if (msg.length == 0) {
            (void)sub.Wait();
            continue;
        }
        uint64_t seq = 0;
        if (!mw_bench::ReadSampleMeta(msg.buffer, static_cast<std::size_t>(msg.length), &seq, nullptr)) {
            continue;
        }
        if (!PublishFbs(pub, fbb, seq, opt.size)) {
            std::fprintf(stderr, "echo publish failed\n");
            return 1;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    opt.stack = "subspace";
    if (!ParseOptions(argc, argv, &opt)) {
        return 2;
    }
    auto srv = StartServer(opt);
    if (!srv) {
        return 1;
    }
    // Give TCP discovery a moment before clients create bridged pubs.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return opt.role == "pub" ? RunPub(opt, *srv) : RunSub(opt, *srv);
}
