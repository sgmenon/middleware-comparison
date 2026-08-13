#include "common.h"
#include "process.h"

#include "Bench.h"

#include <benchmark/benchmark.h>
#include <dds/dds.h>

#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void SetQos(dds_qos_t* qos, bool reliable) {
    // Shallow history keeps measured one-way latency near the critical path
    // (publish→take) instead of queueing delay under a faster publisher.
    dds_qset_reliability(qos, reliable ? DDS_RELIABILITY_RELIABLE : DDS_RELIABILITY_BEST_EFFORT, DDS_SECS(1));
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 4);
    dds_qset_durability(qos, DDS_DURABILITY_VOLATILE);
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

// Take matching seq using a reused waitset. Try take first so waitset setup
// stays off the hot path when the sample is already available.
bool TakeSeq(dds_entity_t reader, dds_entity_t waitset, void** samples, dds_sample_info_t* infos, uint64_t want_seq, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        dds_return_t rc = dds_take(reader, samples, infos, 1, 1);
        if (rc < 0) {
            return false;
        }
        if (rc > 0 && infos[0].valid_data) {
            auto* got = static_cast<Bench_Sample*>(samples[0]);
            if (got->seq == want_seq) {
                return true;
            }
            // Stale/other seq — keep draining toward want_seq.
            continue;
        }
        dds_attach_t xs[1];
        (void)dds_waitset_wait(waitset, xs, 1, DDS_MSECS(50));
    }
    return false;
}

// Take any valid sample. Distinguishes already-queued vs waited-for (fresh).
// wait_ns_out = time blocked in waitset (0 if first take succeeded).
bool TakeOne(dds_entity_t reader, dds_entity_t waitset, void** samples, dds_sample_info_t* infos, int timeout_ms, uint64_t* send_ns_out,
             bool* was_queued_out, uint64_t* wait_ns_out) {
    *was_queued_out = false;
    *wait_ns_out = 0;

    dds_return_t rc = dds_take(reader, samples, infos, 1, 1);
    if (rc < 0) {
        return false;
    }
    if (rc > 0 && infos[0].valid_data) {
        auto* got = static_cast<Bench_Sample*>(samples[0]);
        *send_ns_out = got->send_ns;
        *was_queued_out = true;
        return true;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    uint64_t wait_ns = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const uint64_t w0 = mw_bench::NowNs();
        dds_attach_t xs[1];
        (void)dds_waitset_wait(waitset, xs, 1, DDS_MSECS(50));
        wait_ns += mw_bench::NowNs() - w0;

        rc = dds_take(reader, samples, infos, 1, 1);
        if (rc < 0) {
            return false;
        }
        if (rc > 0 && infos[0].valid_data) {
            auto* got = static_cast<Bench_Sample*>(samples[0]);
            *send_ns_out = got->send_ns;
            *was_queued_out = false;
            *wait_ns_out = wait_ns;
            return true;
        }
    }
    return false;
}

int WriteSample(dds_entity_t writer, uint64_t seq, const std::vector<char>& payload) {
    Bench_Sample sample{};
    sample.seq = seq;
    sample.send_ns = mw_bench::NowNs();  // stamp at publish
    sample.data._buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(payload.data()));
    sample.data._length = static_cast<uint32_t>(payload.size());
    sample.data._maximum = sample.data._length;
    sample.data._release = false;
    return dds_write(writer, &sample);
}

constexpr const char* kShmConfig = R"(<?xml version="1.0" encoding="UTF-8" ?>
<CycloneDDS>
  <Domain id="any">
    <SharedMemory>
      <Enable>true</Enable>
      <LogLevel>info</LogLevel>
    </SharedMemory>
  </Domain>
</CycloneDDS>
)";

// iceoryx mempool must fit our largest sample (+ overhead).
constexpr const char* kIoxConfig = R"(
[general]
version = 1

[[segment]]

[[segment.mempool]]
size = 128
count = 512

[[segment.mempool]]
size = 1024
count = 512

[[segment.mempool]]
size = 16384
count = 128

[[segment.mempool]]
size = 65536
count = 128

[[segment.mempool]]
size = 131072
count = 128

[[segment.mempool]]
size = 262144
count = 64

[[segment.mempool]]
size = 524288
count = 64

[[segment.mempool]]
size = 1048576
count = 32

[[segment.mempool]]
size = 2097152
count = 16

[[segment.mempool]]
size = 4194304
count = 64

[[segment.mempool]]
size = 8388608
count = 64
)";

}  // namespace

class CycloneShmFixture : public benchmark::Fixture {
   public:
    void SetUp(const ::benchmark::State&) override {
        if (participant_ > 0) {
            return;
        }
        iox_cfg_ = mw_bench::WriteTempFile("iox", kIoxConfig);
        dds_cfg_ = mw_bench::WriteTempFile("cyclone_shm", kShmConfig);
        ::setenv("CYCLONEDDS_URI", ("file://" + dds_cfg_).c_str(), 1);

        const char* roudi = std::getenv("MW_BENCH_ROUDI");
        std::string roudi_path;
        if (roudi != nullptr) {
            roudi_path = roudi;
        } else {
            const char* rel = "/+third_party+iceoryx/iceoryx/bin/iox-roudi";
            std::vector<std::string> roots;
            if (const char* dir = std::getenv("RUNFILES_DIR")) {
                roots.emplace_back(dir);
            }
            char self[4096];
            ssize_t n = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
            if (n > 0) {
                self[n] = '\0';
                roots.emplace_back(std::string(self) + ".runfiles");
            }
            for (const auto& root : roots) {
                std::string cand = root + rel;
                if (::access(cand.c_str(), X_OK) == 0) {
                    roudi_path = cand;
                    break;
                }
            }
        }
        if (roudi_path.empty()) {
            throw std::runtime_error("iox-roudi not found; set MW_BENCH_ROUDI or run via `bazel run`");
        }
        roudi_ = std::make_unique<mw_bench::ChildProcess>(roudi_path, std::vector<std::string>{"-c", iox_cfg_});

        participant_ = dds_create_participant(DDS_DOMAIN_DEFAULT, nullptr, nullptr);
        if (participant_ < 0) {
            throw std::runtime_error("shm: dds_create_participant failed");
        }
    }

    static void Shutdown() {
        if (participant_ > 0) {
            dds_delete(participant_);
            participant_ = 0;
        }
        roudi_.reset();
    }

    dds_entity_t Participant() const { return participant_; }

   private:
    inline static dds_entity_t participant_{0};
    inline static std::string iox_cfg_;
    inline static std::string dds_cfg_;
    inline static std::unique_ptr<mw_bench::ChildProcess> roudi_;
};

void RunPingPong(dds_entity_t pp, benchmark::State& state, const char* prefix, bool reliable) {
    const std::size_t size = static_cast<std::size_t>(state.range(0));
    const std::string topic_name = std::string(prefix) + "_pp_" + std::to_string(static_cast<int>(reliable)) + "_" + std::to_string(size) +
                                   "_" + std::to_string(reinterpret_cast<uintptr_t>(&state));

    dds_qos_t* qos = dds_create_qos();
    SetQos(qos, reliable);

    dds_entity_t topic = dds_create_topic(pp, &Bench_Sample_desc, topic_name.c_str(), nullptr, nullptr);
    dds_entity_t writer = dds_create_writer(pp, topic, qos, nullptr);
    dds_entity_t reader = dds_create_reader(pp, topic, qos, nullptr);
    dds_delete_qos(qos);

    if (topic < 0 || writer < 0 || reader < 0) {
        state.SkipWithError("failed to create topic/writer/reader");
        return;
    }
    if (!WaitMatched(writer, 2000)) {
        state.SkipWithError("publication not matched");
        return;
    }

    void* samples[1];
    dds_sample_info_t infos[1];
    samples[0] = Bench_Sample__alloc();

    dds_entity_t waitset = dds_create_waitset(pp);
    dds_entity_t rdcond = dds_create_readcondition(reader, DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE);
    if (waitset < 0 || rdcond < 0 || dds_waitset_attach(waitset, rdcond, reader) < 0) {
        state.SkipWithError("failed to create waitset");
        Bench_Sample_free(samples[0], DDS_FREE_ALL);
        return;
    }

    uint64_t seq = 0;
    std::vector<double> lats;
    lats.reserve(4096);
    // Reused body — WriteSample stamps send_ns; avoid per-iter multi-MiB alloc.
    std::vector<char> payload(size);
    for (auto _ : state) {
        ++seq;
        if (WriteSample(writer, seq, payload) < 0) {
            state.SkipWithError("dds_write failed");
            break;
        }
        if (!TakeSeq(reader, waitset, samples, infos, seq, 2000)) {
            state.SkipWithError("timed out waiting for sample");
            break;
        }
        auto* got = static_cast<Bench_Sample*>(samples[0]);
        const double sec = mw_bench::OneWayLatencySec(got->send_ns);
        lats.push_back(sec);
        state.SetIterationTime(sec);
    }

    dds_delete(waitset);
    Bench_Sample_free(samples[0], DDS_FREE_ALL);
    mw_bench::ReportLatencyCounters(state, lats);
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(size));
    state.SetItemsProcessed(state.iterations());
}

void RunMtOneWay(dds_entity_t pp, benchmark::State& state, const char* prefix, bool reliable) {
    const std::size_t size = static_cast<std::size_t>(state.range(0));
    const std::string topic_name = std::string(prefix) + "_mt_" + std::to_string(static_cast<int>(reliable)) + "_" + std::to_string(size) +
                                   "_" + std::to_string(reinterpret_cast<uintptr_t>(&state));

    dds_qos_t* qos = dds_create_qos();
    SetQos(qos, reliable);

    dds_entity_t topic = dds_create_topic(pp, &Bench_Sample_desc, topic_name.c_str(), nullptr, nullptr);
    dds_entity_t writer = dds_create_writer(pp, topic, qos, nullptr);
    dds_entity_t reader = dds_create_reader(pp, topic, qos, nullptr);
    dds_delete_qos(qos);

    if (topic < 0 || writer < 0 || reader < 0) {
        state.SkipWithError("failed to create topic/writer/reader");
        return;
    }
    if (!WaitMatched(writer, 5000)) {
        state.SkipWithError("publication not matched");
        return;
    }

    void* samples[1];
    dds_sample_info_t infos[1];
    samples[0] = Bench_Sample__alloc();

    dds_entity_t waitset = dds_create_waitset(pp);
    dds_entity_t rdcond = dds_create_readcondition(reader, DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE);
    if (waitset < 0 || rdcond < 0 || dds_waitset_attach(waitset, rdcond, reader) < 0) {
        state.SkipWithError("failed to create waitset");
        Bench_Sample_free(samples[0], DDS_FREE_ALL);
        return;
    }

    std::atomic<bool> stop{false};
    std::atomic<int> in_flight{0};
    std::atomic<uint64_t> seq{0};
    const int max_in_flight = 2;
    std::vector<char> payload(size, 0);
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
            if (WriteSample(writer, s, payload) < 0) {
                if (reliable) {
                    std::this_thread::sleep_for(std::chrono::microseconds(10));
                    continue;
                }
                break;
            }
            if (max_in_flight > 0) {
                in_flight.fetch_add(1, std::memory_order_release);
            }
        }
    });

    for (auto _ : state) {
        // Drain already-queued samples (not timed). Cap so a racing pub cannot
        // keep the queue non-empty forever.
        for (int drained = 0; drained < 32; ++drained) {
            dds_return_t rc = dds_take(reader, samples, infos, 1, 1);
            if (rc < 0) {
                state.SkipWithError("dds_take failed while draining");
                goto mt_done;
            }
            if (rc == 0 || !infos[0].valid_data) {
                break;
            }
            auto* got = static_cast<Bench_Sample*>(samples[0]);
            const double e2e = mw_bench::OneWayLatencySec(got->send_ns);
            take_stats.Record(e2e, /*was_queued=*/true, /*wait_sec=*/0.0);
            int cur = in_flight.load(std::memory_order_relaxed);
            while (cur > 0 && !in_flight.compare_exchange_weak(cur, cur - 1, std::memory_order_release, std::memory_order_relaxed)) {}
        }

        // Timed path: wait for a newly arriving sample (queue should be empty).
        uint64_t send_ns = 0;
        bool was_queued = false;
        uint64_t wait_ns = 0;
        if (!TakeOne(reader, waitset, samples, infos, 2000, &send_ns, &was_queued, &wait_ns)) {
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
mt_done:
    stop.store(true, std::memory_order_relaxed);
    if (pub_thread.joinable()) {
        pub_thread.join();
    }
    dds_delete(waitset);
    Bench_Sample_free(samples[0], DDS_FREE_ALL);
    mw_bench::ReportLatencyCounters(state, lats);
    take_stats.Report(state);
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(size));
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK_DEFINE_F(CycloneShmFixture, ReliablePingPong)
(benchmark::State& state) {
    RunPingPong(Participant(), state, "shm", /*reliable=*/true);
}
BENCHMARK_REGISTER_F(CycloneShmFixture, ReliablePingPong)->Apply(mw_bench::PingPongArgs);

BENCHMARK_DEFINE_F(CycloneShmFixture, MtReliableOneWay)
(benchmark::State& state) {
    RunMtOneWay(Participant(), state, "shm", /*reliable=*/true);
}
BENCHMARK_REGISTER_F(CycloneShmFixture, MtReliableOneWay)->Apply(mw_bench::OneWayLatencyArgs);

BENCHMARK_DEFINE_F(CycloneShmFixture, MtUnreliableOneWay)
(benchmark::State& state) {
    RunMtOneWay(Participant(), state, "shm", /*reliable=*/false);
}
BENCHMARK_REGISTER_F(CycloneShmFixture, MtUnreliableOneWay)->Apply(mw_bench::OneWayLatencyArgs);

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
        return 1;
    }
    benchmark::RunSpecifiedBenchmarks();
    CycloneShmFixture::Shutdown();
    benchmark::Shutdown();
    return 0;
}
