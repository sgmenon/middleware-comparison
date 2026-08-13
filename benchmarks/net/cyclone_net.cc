// Cyclone DDS network ping-pong CLI: SHM off, reliable, peer via Discovery Peers.
// RTT/2 with local timestamps only (two containers on a Docker bridge).
#include "net_common.h"
#include "process.h"

#include "Bench.h"

#include <dds/dds.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using mw_bench::net::Options;
using mw_bench::net::ParseHostPort;
using mw_bench::net::ParseOptions;
using mw_bench::net::PrintCsv;
using mw_bench::net::RepKey;
using mw_bench::net::ReqKey;

void SetQos(dds_qos_t* qos) {
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 4);
    dds_qset_durability(qos, DDS_DURABILITY_VOLATILE);
}

std::string MakeConfig(const std::string& peer_host) {
    // SHM off; unicast peer so discovery works across the Docker bridge.
    std::string xml = R"(<?xml version="1.0" encoding="UTF-8" ?>
<CycloneDDS>
  <Domain id="any">
    <General>
      <AllowMulticast>spdp</AllowMulticast>
    </General>
    <Discovery>
      <ParticipantIndex>auto</ParticipantIndex>
)";
    if (!peer_host.empty()) {
        xml += "      <Peers><Peer address=\"" + peer_host + "\"/></Peers>\n";
    }
    xml += R"(    </Discovery>
    <SharedMemory>
      <Enable>false</Enable>
    </SharedMemory>
  </Domain>
</CycloneDDS>
)";
    return xml;
}

bool WaitMatched(dds_entity_t writer, int timeout_ms) {
    dds_publication_matched_status_t st{};
    const int steps = timeout_ms / 10;
    for (int i = 0; i < steps; ++i) {
        if (dds_get_publication_matched_status(writer, &st) == 0 && st.current_count > 0) {
            return true;
        }
        dds_sleepfor(DDS_MSECS(10));
    }
    return false;
}

int WriteSample(dds_entity_t writer, uint64_t seq, std::vector<char>& payload) {
    Bench_Sample sample{};
    sample.seq = seq;
    sample.send_ns = mw_bench::net::NowNs();
    sample.data._buffer = reinterpret_cast<uint8_t*>(payload.data());
    sample.data._length = static_cast<uint32_t>(payload.size());
    sample.data._maximum = sample.data._length;
    sample.data._release = false;
    return dds_write(writer, &sample);
}

bool TakeSeq(dds_entity_t reader, dds_entity_t waitset, void** samples, dds_sample_info_t* infos, uint64_t want, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        dds_return_t rc = dds_take(reader, samples, infos, 1, 1);
        if (rc < 0) {
            return false;
        }
        if (rc > 0 && infos[0].valid_data) {
            auto* got = static_cast<Bench_Sample*>(samples[0]);
            if (got->seq == want) {
                return true;
            }
            continue;
        }
        dds_attach_t xs[1];
        (void)dds_waitset_wait(waitset, xs, 1, DDS_MSECS(50));
    }
    return false;
}

struct Entities {
    dds_entity_t pp{0};
    dds_entity_t writer{0};
    dds_entity_t reader{0};
    dds_entity_t waitset{0};
    void* samples[1]{};
    dds_sample_info_t infos[1]{};
    std::string cfg_path;

    ~Entities() {
        if (samples[0] != nullptr) {
            Bench_Sample_free(samples[0], DDS_FREE_ALL);
        }
        if (waitset > 0) {
            dds_delete(waitset);
        }
        if (pp > 0) {
            dds_delete(pp);
        }
    }
};

Entities Setup(const Options& opt, bool is_pub) {
    Entities e;
    std::string peer_host;
    if (is_pub) {
        int port = 0;
        if (!ParseHostPort(opt.peer, &peer_host, &port)) {
            // Peer may be host-only for Cyclone SPDP; allow host without requiring port use.
            peer_host = opt.peer;
            const auto colon = peer_host.rfind(':');
            if (colon != std::string::npos) {
                peer_host = peer_host.substr(0, colon);
            }
        }
    } else if (!opt.peer.empty()) {
        int port = 0;
        if (!ParseHostPort(opt.peer, &peer_host, &port)) {
            peer_host = opt.peer;
        }
    }

    e.cfg_path = mw_bench::WriteTempFile("cyclone_net", MakeConfig(peer_host));
    ::setenv("CYCLONEDDS_URI", ("file://" + e.cfg_path).c_str(), 1);

    e.pp = dds_create_participant(DDS_DOMAIN_DEFAULT, nullptr, nullptr);
    if (e.pp < 0) {
        throw std::runtime_error("dds_create_participant failed");
    }

    const std::string req = ReqKey(opt);
    const std::string rep = RepKey(opt);
    // Pub: write req, read rep. Sub: read req, write rep.
    const std::string& write_topic = is_pub ? req : rep;
    const std::string& read_topic = is_pub ? rep : req;

    dds_qos_t* qos = dds_create_qos();
    SetQos(qos);
    dds_entity_t wtopic = dds_create_topic(e.pp, &Bench_Sample_desc, write_topic.c_str(), nullptr, nullptr);
    dds_entity_t rtopic = dds_create_topic(e.pp, &Bench_Sample_desc, read_topic.c_str(), nullptr, nullptr);
    e.writer = dds_create_writer(e.pp, wtopic, qos, nullptr);
    e.reader = dds_create_reader(e.pp, rtopic, qos, nullptr);
    dds_delete_qos(qos);
    if (wtopic < 0 || rtopic < 0 || e.writer < 0 || e.reader < 0) {
        throw std::runtime_error("failed to create writer/reader");
    }

    e.samples[0] = Bench_Sample__alloc();
    e.waitset = dds_create_waitset(e.pp);
    dds_entity_t rdcond = dds_create_readcondition(e.reader, DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE);
    if (e.waitset < 0 || rdcond < 0 || dds_waitset_attach(e.waitset, rdcond, e.reader) < 0) {
        throw std::runtime_error("failed to create waitset");
    }
    return e;
}

int RunPub(const Options& opt) {
    auto e = Setup(opt, /*is_pub=*/true);
    std::vector<char> payload(opt.size);

    bool linked = false;
    for (int i = 0; i < 200 && !linked; ++i) {
        if (WaitMatched(e.writer, 100)) {
            const uint64_t probe = static_cast<uint64_t>(i + 1);
            if (WriteSample(e.writer, probe, payload) >= 0 && TakeSeq(e.reader, e.waitset, e.samples, e.infos, probe, 200)) {
                linked = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!linked) {
        std::fprintf(stderr, "cyclone failed to link (no echo)\n");
        return 1;
    }

    std::vector<double> half_us;
    half_us.reserve(static_cast<std::size_t>(opt.count));
    uint64_t seq = 1000;
    for (int i = 0; i < opt.warmup + opt.count; ++i) {
        ++seq;
        const uint64_t t0 = mw_bench::net::NowNs();
        if (WriteSample(e.writer, seq, payload) < 0) {
            std::fprintf(stderr, "dds_write failed\n");
            return 1;
        }
        if (!TakeSeq(e.reader, e.waitset, e.samples, e.infos, seq, 5000)) {
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
    auto e = Setup(opt, /*is_pub=*/false);
    std::vector<char> payload(opt.size);

    // Wait until our reply writer matches the pub's reply reader.
    (void)WaitMatched(e.writer, 30000);

    for (;;) {
        dds_return_t rc = dds_take(e.reader, e.samples, e.infos, 1, 1);
        if (rc < 0) {
            return 1;
        }
        if (rc == 0 || !e.infos[0].valid_data) {
            dds_attach_t xs[1];
            (void)dds_waitset_wait(e.waitset, xs, 1, DDS_MSECS(50));
            continue;
        }
        auto* got = static_cast<Bench_Sample*>(e.samples[0]);
        if (WriteSample(e.writer, got->seq, payload) < 0) {
            std::fprintf(stderr, "echo dds_write failed\n");
            return 1;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    opt.stack = "cyclone";
    if (!ParseOptions(argc, argv, &opt)) {
        return 2;
    }
    // Sub also benefits from knowing the pub hostname for Peers.
    try {
        return opt.role == "pub" ? RunPub(opt) : RunSub(opt);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "cyclone_net: %s\n", ex.what());
        return 1;
    }
}
