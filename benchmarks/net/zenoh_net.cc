// Zenoh network ping-pong CLI: SHM off, TCP peers, RTT/2 with local timestamps.
#include "bench_sample.h"
#include "net_common.h"
#include "zenoh_common.h"

#include <zenoh.hxx>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "flatbuffers/flatbuffers.h"

namespace {

using mw_bench::net::Options;
using mw_bench::net::ParseHostPort;
using mw_bench::net::ParseOptions;
using mw_bench::net::PrintCsv;
using mw_bench::net::RepKey;
using mw_bench::net::ReqKey;
using mw_bench::zenoh_util::MakeTcpPeerConfig;
using zenoh::KeyExpr;
using zenoh::Sample;
using zenoh::Session;

using ZenohSub = decltype(std::declval<Session&>().declare_subscriber(KeyExpr(""), zenoh::channels::FifoChannel(16)));

zenoh::Publisher MakePub(Session& s, const KeyExpr& key) {
    zenoh::Session::PublisherOptions opts;
    opts.congestion_control = Z_CONGESTION_CONTROL_BLOCK;
    opts.is_express = true;
    opts.allowed_destination = Z_LOCALITY_REMOTE;
    return s.declare_publisher(key, std::move(opts));
}

ZenohSub MakeSub(Session& s, const KeyExpr& key) {
    zenoh::Session::SubscriberOptions opts;
    opts.allowed_origin = Z_LOCALITY_REMOTE;
    return s.declare_subscriber(key, zenoh::channels::FifoChannel(64), std::move(opts));
}

void PutBytes(zenoh::Publisher& pub, flatbuffers::FlatBufferBuilder& fbb, uint64_t seq, std::size_t size) {
    mw_bench::BuildSample(fbb, seq, size);
    std::vector<uint8_t> buf(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
    mw_bench::StampSample(buf.data());
    pub.put(zenoh::Bytes(std::move(buf)));
}

bool RecvMatching(ZenohSub& sub, uint64_t want, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        auto res = sub.handler().try_recv();
        if (std::holds_alternative<zenoh::channels::RecvError>(res)) {
            std::this_thread::yield();
            continue;
        }
        auto& sample = std::get<Sample>(res);
        auto payload = sample.get_payload().as_vector();
        uint64_t got = 0;
        if (mw_bench::ReadSampleMeta(payload.data(), payload.size(), &got, nullptr) && got == want) {
            return true;
        }
    }
    return false;
}

bool TryRecvSeq(ZenohSub& sub, uint64_t* seq_out) {
    auto res = sub.handler().try_recv();
    if (std::holds_alternative<zenoh::channels::RecvError>(res)) {
        return false;
    }
    auto& sample = std::get<Sample>(res);
    auto payload = sample.get_payload().as_vector();
    return mw_bench::ReadSampleMeta(payload.data(), payload.size(), seq_out, nullptr);
}

int RunPub(const Options& opt) {
    std::string host;
    int port = 0;
    if (!ParseHostPort(opt.peer, &host, &port)) {
        std::fprintf(stderr, "bad --peer=%s\n", opt.peer.c_str());
        return 1;
    }
    const std::string endpoint = "tcp/" + host + ":" + std::to_string(port);
    // Sub listens; pub connects.
    auto session = Session::open(MakeTcpPeerConfig(/*listen=*/{}, endpoint));

    KeyExpr req(ReqKey(opt));
    KeyExpr rep(RepKey(opt));
    auto sub = MakeSub(session, rep);
    auto pub = MakePub(session, req);

    flatbuffers::FlatBufferBuilder fbb;
    bool linked = false;
    for (int i = 0; i < 100 && !linked; ++i) {
        const uint64_t probe = static_cast<uint64_t>(i + 1);
        PutBytes(pub, fbb, probe, opt.size);
        if (RecvMatching(sub, probe, 200)) {
            linked = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!linked) {
        std::fprintf(stderr, "zenoh failed to link (no echo)\n");
        return 1;
    }

    std::vector<double> half_us;
    half_us.reserve(static_cast<std::size_t>(opt.count));
    uint64_t seq = 1000;
    for (int i = 0; i < opt.warmup + opt.count; ++i) {
        ++seq;
        const uint64_t t0 = mw_bench::net::NowNs();
        PutBytes(pub, fbb, seq, opt.size);
        if (!RecvMatching(sub, seq, 5000)) {
            std::fprintf(stderr, "timeout waiting echo seq %llu\n", static_cast<unsigned long long>(seq));
            return 1;
        }
        const uint64_t t1 = mw_bench::net::NowNs();
        if (i >= opt.warmup) {
            half_us.push_back(static_cast<double>(t1 - t0) / 2.0 * 1e-3);
        }
    }
    PrintCsv(opt, half_us);
    return 0;
}

int RunSub(const Options& opt) {
    const std::string listen = "tcp/0.0.0.0:" + std::to_string(opt.disc_port);
    auto session = Session::open(MakeTcpPeerConfig(listen, /*connect=*/{}));

    KeyExpr req(ReqKey(opt));
    KeyExpr rep(RepKey(opt));
    auto sub = MakeSub(session, req);
    auto pub = MakePub(session, rep);

    flatbuffers::FlatBufferBuilder fbb;
    for (;;) {
        uint64_t seq = 0;
        if (!TryRecvSeq(sub, &seq)) {
            std::this_thread::yield();
            continue;
        }
        PutBytes(pub, fbb, seq, opt.size);
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    opt.stack = "zenoh";
    opt.disc_port = 7447;
    if (!ParseOptions(argc, argv, &opt)) {
        return 2;
    }
    return opt.role == "pub" ? RunPub(opt) : RunSub(opt);
}
