#include "common.h"
#include "process.h"

#include "Bench.h"

#include <benchmark/benchmark.h>
#include <dds/dds.h>

#include <unistd.h>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void SetReliableQos(dds_qos_t* qos) {
    // Paced ping-pong only needs a tiny history; deep KEEP_LAST burns iceoryx
    // chunks (each sample holds a loan until taken/returned).
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
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

bool TakeSeq(dds_entity_t reader, void** samples, dds_sample_info_t* infos, uint64_t want_seq, int timeout_ms) {
    dds_entity_t waitset = dds_create_waitset(dds_get_participant(reader));
    if (waitset < 0) {
        waitset = dds_create_waitset(DDS_CYCLONEDDS_HANDLE);
    }
    dds_entity_t rdcond = dds_create_readcondition(reader, DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE);
    dds_waitset_attach(waitset, rdcond, reader);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        dds_attach_t xs[1];
        (void)dds_waitset_wait(waitset, xs, 1, DDS_MSECS(50));
        dds_return_t rc = dds_take(reader, samples, infos, 1, 1);
        if (rc < 0) {
            dds_delete(waitset);
            return false;
        }
        if (rc == 0 || !infos[0].valid_data) {
            continue;
        }
        auto* got = static_cast<Bench_Sample*>(samples[0]);
        if (got->seq == want_seq) {
            dds_delete(waitset);
            return true;
        }
    }
    dds_delete(waitset);
    return false;
}

int WriteSample(dds_entity_t writer, uint64_t seq, const std::vector<char>& payload) {
    Bench_Sample sample{};
    sample.seq = seq;
    sample.send_ns = mw_bench::HeaderOf(payload.data())->send_ns;
    // Full payload (header + bytes) in the sequence so sizes match the Arg.
    sample.data._buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(payload.data()));
    sample.data._length = static_cast<uint32_t>(payload.size());
    sample.data._maximum = sample.data._length;
    sample.data._release = false;
    return dds_write(writer, &sample);
}

constexpr const char* kUdpConfig = R"(<?xml version="1.0" encoding="UTF-8" ?>
<CycloneDDS>
  <Domain id="any">
    <SharedMemory>
      <Enable>false</Enable>
    </SharedMemory>
  </Domain>
</CycloneDDS>
)";

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
count = 100

[[segment.mempool]]
size = 1024
count = 100

[[segment.mempool]]
size = 16384
count = 50

[[segment.mempool]]
size = 131072
count = 128

[[segment.mempool]]
size = 524288
count = 32
)";

}  // namespace

class CycloneUdpFixture : public benchmark::Fixture {
   public:
    void SetUp(const ::benchmark::State&) override {
        if (participant_ > 0) {
            return;
        }
        config_path_ = mw_bench::WriteTempFile("cyclone_udp", kUdpConfig);
        ::setenv("CYCLONEDDS_URI", ("file://" + config_path_).c_str(), 1);
        participant_ = dds_create_participant(DDS_DOMAIN_DEFAULT, nullptr, nullptr);
        if (participant_ < 0) {
            throw std::runtime_error("udp: dds_create_participant failed");
        }
    }

    static void Shutdown() {
        if (participant_ > 0) {
            dds_delete(participant_);
            participant_ = 0;
        }
    }

    dds_entity_t Participant() const { return participant_; }

   private:
    inline static dds_entity_t participant_{0};
    inline static std::string config_path_;
};

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

template <typename Fixture>
void RunPingPong(Fixture& fix, benchmark::State& state, const char* prefix) {
    const std::size_t size = static_cast<std::size_t>(state.range(0));
    const std::string topic_name =
        std::string(prefix) + "_" + std::to_string(size) + "_" + std::to_string(reinterpret_cast<uintptr_t>(&state));

    dds_qos_t* qos = dds_create_qos();
    SetReliableQos(qos);

    dds_entity_t topic = dds_create_topic(fix.Participant(), &Bench_Sample_desc, topic_name.c_str(), nullptr, nullptr);
    dds_entity_t writer = dds_create_writer(fix.Participant(), topic, qos, nullptr);
    dds_entity_t reader = dds_create_reader(fix.Participant(), topic, qos, nullptr);
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

    uint64_t seq = 0;
    for (auto _ : state) {
        ++seq;
        auto payload = mw_bench::MakePayload(size, seq);
        if (WriteSample(writer, seq, payload) < 0) {
            state.SkipWithError("dds_write failed");
            break;
        }
        if (!TakeSeq(reader, samples, infos, seq, 2000)) {
            state.SkipWithError("timed out waiting for sample");
            break;
        }
    }

    Bench_Sample_free(samples[0], DDS_FREE_ALL);
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(size));
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK_DEFINE_F(CycloneUdpFixture, ReliablePingPong)
(benchmark::State& state) {
    RunPingPong(*this, state, "udp");
}
BENCHMARK_REGISTER_F(CycloneUdpFixture, ReliablePingPong)
    ->Arg(64)
    ->Arg(1024)
    ->Arg(16384)
    ->Arg(65536)
    ->ArgName("bytes")
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime();

BENCHMARK_DEFINE_F(CycloneShmFixture, ReliablePingPong)
(benchmark::State& state) {
    RunPingPong(*this, state, "shm");
}
BENCHMARK_REGISTER_F(CycloneShmFixture, ReliablePingPong)
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
    CycloneShmFixture::Shutdown();
    CycloneUdpFixture::Shutdown();
    benchmark::Shutdown();
    return 0;
}
