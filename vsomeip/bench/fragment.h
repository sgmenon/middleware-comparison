#pragma once

#include "constants.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace vsomeip_bench {

// A frame is one contiguous block sent as `count` notifies of about `len` bytes each. Every chunk
// starts with a ChunkHeader; chunk 0 also carries the frame's send_ns right after it.
#pragma pack(push, 1)
struct ChunkHeader {
    std::uint32_t seq = 0;
    std::uint16_t index = 0;
    std::uint16_t count = 1;
};
#pragma pack(pop)

inline constexpr std::size_t kChunkSendNsOffset = sizeof(ChunkHeader);
inline constexpr std::size_t kMinFrameBytes = kChunkSendNsOffset + 8;

struct ChunkPlan {
    std::size_t count = 1;
    std::size_t len = 0;  // chunk i covers [i*len, min((i+1)*len, size))
};

// Even split so no chunk is smaller than size/count; the E2E header shares the datagram on both stacks.
inline ChunkPlan PlanChunks(std::size_t size, std::size_t max_datagram) {
    const std::size_t budget = max_datagram - kE2eHeaderBytes;
    ChunkPlan plan;
    plan.count = (size + budget - 1) / budget;
    plan.len = (size + plan.count - 1) / plan.count;
    return plan;
}

inline void WriteChunkHeaders(std::vector<std::uint8_t>& block, std::uint32_t seq, const ChunkPlan& plan) {
    for (std::size_t i = 0; i < plan.count; ++i) {
        const ChunkHeader hdr{seq, static_cast<std::uint16_t>(i), static_cast<std::uint16_t>(plan.count)};
        std::memcpy(block.data() + i * plan.len, &hdr, sizeof(hdr));
    }
}

inline void StampSendNs(std::vector<std::uint8_t>& block, std::uint64_t send_ns) {
    std::memcpy(block.data() + kChunkSendNsOffset, &send_ns, 8);
}

struct CompletedFrame {
    std::uint32_t seq = 0;
    std::uint64_t send_ns = 0;
};

// Follows chunks in arrival order; reports a frame when its last chunk arrives and none were missed.
class FrameTracker {
   public:
    std::optional<CompletedFrame> ingest(const std::uint8_t* data, std::size_t length) {
        // COVESA: skip the in-payload E2E header the stack leaves in place.
        if (length < kE2eHoleBytes + sizeof(ChunkHeader)) {
            return std::nullopt;
        }
        data += kE2eHoleBytes;
        length -= kE2eHoleBytes;
        ChunkHeader hdr{};
        std::memcpy(&hdr, data, sizeof(hdr));
        if (hdr.index == 0) {
            if (length < kMinFrameBytes) {
                active_ = false;
                return std::nullopt;
            }
            active_ = true;
            seq_ = hdr.seq;
            next_ = 0;
            std::memcpy(&send_ns_, data + kChunkSendNsOffset, 8);
        }
        if (!active_ || hdr.seq != seq_ || hdr.index != next_) {
            active_ = false;
            return std::nullopt;
        }
        if (++next_ < hdr.count) {
            return std::nullopt;
        }
        active_ = false;
        return CompletedFrame{seq_, send_ns_};
    }

   private:
    bool active_ = false;
    std::uint32_t seq_ = 0;
    std::uint16_t next_ = 0;
    std::uint64_t send_ns_ = 0;
};

}  // namespace vsomeip_bench
