#pragma once

#include "Bench_generated.h"
#include "common.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

#include "flatbuffers/flatbuffers.h"

namespace mw_bench {

// FlatBuffer table overhead beyond the `data` vector (vtable, fields, vector
// header, alignment). Generous so slot/SHM alloc never undershoots.
inline constexpr std::size_t kFbsOverhead = 128;

inline std::size_t SampleSlotBytes(std::size_t payload_bytes) {
    return payload_bytes + kFbsOverhead;
}

// Build Bench::Sample matching Bench.idl: seq, send_ns, data[payload_bytes].
// Data body is left uninitialized (latency bench; avoid multi-MiB memset).
// send_ns is written as a non-zero placeholder so the field is present in the
// buffer (FlatBuffers omits default 0); StampSample() overwrites it later.
inline void BuildSample(flatbuffers::FlatBufferBuilder& fbb, uint64_t seq, std::size_t payload_bytes) {
    fbb.Clear();
    uint8_t* data_buf = nullptr;
    const auto data_off = fbb.CreateUninitializedVector(payload_bytes, &data_buf);
    (void)data_buf;
    // Placeholder must be != 0 or flatc omits the field and mutate_send_ns fails.
    const auto sample = Bench::CreateSample(fbb, seq, /*send_ns=*/1, data_off);
    fbb.Finish(sample);
}

inline void StampSample(void* buf) {
    auto* sample = Bench::GetMutableSample(buf);
    // Requires BuildSample's non-zero placeholder so the field exists in-buffer.
    if (!sample->mutate_send_ns(NowNs())) {
        throw std::runtime_error("StampSample: send_ns field missing (build with non-default)");
    }
}

// Encode into dest (must be >= fbb.GetSize()), then stamp send_ns.
inline std::size_t WriteSampleBytes(flatbuffers::FlatBufferBuilder& fbb, void* dest, uint64_t seq, std::size_t payload_bytes) {
    BuildSample(fbb, seq, payload_bytes);
    const auto nbytes = fbb.GetSize();
    std::memcpy(dest, fbb.GetBufferPointer(), nbytes);
    StampSample(dest);
    return nbytes;
}

inline const Bench::Sample* GetSample(const void* data) {
    return flatbuffers::GetRoot<Bench::Sample>(data);
}

inline bool ReadSampleMeta(const void* data, std::size_t len, uint64_t* seq_out, uint64_t* send_ns_out) {
    if (data == nullptr || len < 8) {
        return false;
    }
    const auto* sample = GetSample(data);
    if (sample == nullptr) {
        return false;
    }
    if (seq_out != nullptr) {
        *seq_out = sample->seq();
    }
    if (send_ns_out != nullptr) {
        *send_ns_out = sample->send_ns();
    }
    return true;
}

}  // namespace mw_bench
