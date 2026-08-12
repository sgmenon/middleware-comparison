#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mw_bench {

inline constexpr int kNumSlots = 32;

struct MessageHeader {
  uint64_t seq;
  uint64_t send_ns;
};

inline uint64_t NowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

inline std::vector<char> MakePayload(std::size_t size, uint64_t seq) {
  if (size < sizeof(MessageHeader)) {
    size = sizeof(MessageHeader);
  }
  std::vector<char> buf(size, 0);
  MessageHeader hdr{seq, NowNs()};
  std::memcpy(buf.data(), &hdr, sizeof(hdr));
  return buf;
}

inline const MessageHeader *HeaderOf(const void *data) {
  return reinterpret_cast<const MessageHeader *>(data);
}

}  // namespace mw_bench
